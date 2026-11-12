#include "TimerManager.h"
#include <limits>
#include <map>
#include "Utils.h"
#include "EPGManager.h"

namespace {

// The backend serialises timers in PascalCase (same convention as recordings
// and channels, see script.service.ultimate/tests/test_timer_routes.py). The
// snake_case spelling is still accepted so either wire format keeps working.
const nlohmann::json* FindKey(const nlohmann::json& j, const char* pascal, const char* snake) {
  auto it = j.find(pascal);
  if (it != j.end() && !it->is_null()) return &*it;
  it = j.find(snake);
  if (it != j.end() && !it->is_null()) return &*it;
  return nullptr;
}

bool GetInt(const nlohmann::json& j, const char* pascal, const char* snake, int& out) {
  const nlohmann::json* v = FindKey(j, pascal, snake);
  if (!v || !v->is_number_integer()) return false;
  // Provider ids (e.g. station ids) can exceed 32 bit; never truncate silently.
  const long long raw = v->get<long long>();
  if (raw < std::numeric_limits<int>::min() || raw > std::numeric_limits<int>::max()) return false;
  out = static_cast<int>(raw);
  return true;
}

bool GetString(const nlohmann::json& j, const char* pascal, const char* snake, std::string& out) {
  const nlohmann::json* v = FindKey(j, pascal, snake);
  if (!v || !v->is_string()) return false;
  out = v->get<std::string>();
  return true;
}

bool GetBool(const nlohmann::json& j, const char* pascal, const char* snake, bool& out) {
  const nlohmann::json* v = FindKey(j, pascal, snake);
  if (!v || !v->is_boolean()) return false;
  out = v->get<bool>();
  return true;
}

bool GetTime(const nlohmann::json& j, const char* pascal, const char* snake, time_t& out) {
  std::string s;
  if (!GetString(j, pascal, snake, s)) return false;
  out = Utils::ParseISO8601(s);
  return true;
}

// Backend TimerState is a string ("SCHEDULED", ...); older payloads used ints.
int ParseTimerState(const nlohmann::json& j) {
  const nlohmann::json* v = FindKey(j, "State", "state");
  if (!v) return 1;
  if (v->is_number_integer()) return v->get<int>();
  if (!v->is_string()) return 1;
  static const std::map<std::string, int> kStates = {
    {"NEW", 0}, {"SCHEDULED", 1}, {"RECORDING", 2}, {"COMPLETED", 3},
    {"ABORTED", 4}, {"CANCELLED", 5}, {"CONFLICT", 6}, {"ERROR", 7}};
  auto it = kStates.find(v->get<std::string>());
  return it != kStates.end() ? it->second : 1;
}

// Backend TimerTypeAttribute bit -> Kodi PVR_TIMER_TYPE_* flag. Capabilities are
// declared per provider by the backend; the client only translates them.
unsigned int MapTimerTypeAttributes(unsigned int a) {
  struct Map { unsigned int backend; unsigned int kodi; };
  static const Map kMap[] = {
    {0x00000001, PVR_TIMER_TYPE_IS_MANUAL},
    {0x00000002, PVR_TIMER_TYPE_IS_REPEATING},
    {0x00000008, PVR_TIMER_TYPE_SUPPORTS_ENABLE_DISABLE},
    {0x00000010, PVR_TIMER_TYPE_SUPPORTS_CHANNELS},
    {0x00000020, PVR_TIMER_TYPE_SUPPORTS_START_TIME},
    {0x00000040, PVR_TIMER_TYPE_SUPPORTS_START_ANYTIME},
    {0x00000080, PVR_TIMER_TYPE_SUPPORTS_END_TIME},
    {0x00000100, PVR_TIMER_TYPE_SUPPORTS_END_ANYTIME},
    {0x00000200, PVR_TIMER_TYPE_SUPPORTS_FIRST_DAY},
    {0x00000400, PVR_TIMER_TYPE_SUPPORTS_WEEKDAYS},
    {0x00000800, PVR_TIMER_TYPE_SUPPORTS_TITLE_EPG_MATCH},
    {0x00001000, PVR_TIMER_TYPE_SUPPORTS_FULLTEXT_EPG_MATCH},
    {0x00002000, PVR_TIMER_TYPE_SUPPORTS_RECORDING_FOLDERS},
    {0x00004000, PVR_TIMER_TYPE_SUPPORTS_PRIORITY},
    {0x00008000, PVR_TIMER_TYPE_SUPPORTS_LIFETIME},
    {0x00010000, PVR_TIMER_TYPE_SUPPORTS_START_MARGIN | PVR_TIMER_TYPE_SUPPORTS_END_MARGIN},
    {0x00020000, PVR_TIMER_TYPE_SUPPORTS_RECORDING_GROUP},
    {0x00040000, PVR_TIMER_TYPE_SUPPORTS_MAX_RECORDINGS},
    {0x00080000, PVR_TIMER_TYPE_SUPPORTS_RECORD_ONLY_NEW_EPISODES},
    {0x00100000, PVR_TIMER_TYPE_REQUIRES_EPG_TAG_ON_CREATE},
    {0x00200000, PVR_TIMER_TYPE_FORBIDS_EPG_TAG_ON_CREATE},
  };
  unsigned int out = 0;
  for (const auto& m : kMap)
    if (a & m.backend) out |= m.kodi;
  return out;
}

// Used only when the backend reports no attributes (legacy / unknown provider).
constexpr unsigned int kLegacyTimerTypeAttributes =
    PVR_TIMER_TYPE_SUPPORTS_START_TIME | PVR_TIMER_TYPE_SUPPORTS_END_TIME |
    PVR_TIMER_TYPE_SUPPORTS_PRIORITY | PVR_TIMER_TYPE_SUPPORTS_LIFETIME |
    PVR_TIMER_TYPE_SUPPORTS_RECORDING_FOLDERS | PVR_TIMER_TYPE_SUPPORTS_START_ANYTIME |
    PVR_TIMER_TYPE_SUPPORTS_END_ANYTIME | PVR_TIMER_TYPE_SUPPORTS_FULLTEXT_EPG_MATCH |
    PVR_TIMER_TYPE_SUPPORTS_START_MARGIN | PVR_TIMER_TYPE_SUPPORTS_END_MARGIN |
    PVR_TIMER_TYPE_SUPPORTS_WEEKDAYS;

}  // namespace

bool TimerManager::LoadTimerTypes(const std::vector<UltimateProvider>& providers,
                                  const std::function<std::string(const std::string&)>& httpGet,
                                  const std::function<bool(const std::string&, nlohmann::json&)>& parseJson) {
  std::vector<UltimateTimerType> newTimerTypes;

  for (const auto& provider : providers) {
    if (provider.enabled) {
      LoadTimerTypesForProvider(provider.name, httpGet, parseJson, newTimerTypes);
    }
  }

  if (newTimerTypes.empty()) {
    UltimateTimerType manual;
    manual.id = 1;
    manual.description = "Manual";
    manual.priority = 50;
    newTimerTypes.push_back(manual);
  }

  std::unique_lock<std::shared_mutex> lock(m_dataMutex);
  m_timerTypes = std::move(newTimerTypes);

  return true;
}

void TimerManager::LoadTimerTypesForProvider(const std::string& provider,
                                             const std::function<std::string(const std::string&)>& httpGet,
                                             const std::function<bool(const std::string&, nlohmann::json&)>& parseJson,
                                             std::vector<UltimateTimerType>& outTimerTypes) {
  std::string response = httpGet("/api/providers/" + Utils::UrlPathEncode(provider) + "/timer-types");
  if (response.empty()) return;

  nlohmann::json document;
  if (!parseJson(response, document)) return;
  if (!document.contains("timer_types") || !document["timer_types"].is_array()) return;

  for (const auto& ttJson : document["timer_types"]) {
    UltimateTimerType timerType;
    if (!GetInt(ttJson, "TypeId", "id", timerType.id) || timerType.id <= 0) continue;
    if (!GetString(ttJson, "Description", "description", timerType.description))
      timerType.description = "Timer";
    GetInt(ttJson, "PriorityDefault", "priority", timerType.priority);
    int attributes = 0;
    if (GetInt(ttJson, "Attributes", "attributes", attributes) && attributes > 0)
      timerType.attributes = static_cast<unsigned int>(attributes);
    outTimerTypes.push_back(timerType);
  }
}

bool TimerManager::LoadTimers(const std::vector<UltimateProvider>& providers,
                              const std::function<std::string(const std::string&)>& httpGet,
                              const std::function<bool(const std::string&, nlohmann::json&)>& parseJson) {
  std::vector<UltimateTimer> newTimers;

  for (const auto& provider : providers) {
    if (provider.enabled) {
      LoadTimersForProvider(provider.name, httpGet, parseJson, newTimers);
    }
  }

  std::unique_lock<std::shared_mutex> lock(m_dataMutex);
  m_timers = std::move(newTimers);

  return true;
}

void TimerManager::LoadTimersForProvider(const std::string& provider,
                                         const std::function<std::string(const std::string&)>& httpGet,
                                         const std::function<bool(const std::string&, nlohmann::json&)>& parseJson,
                                         std::vector<UltimateTimer>& outTimers) {
  std::string response = httpGet("/api/providers/" + Utils::UrlPathEncode(provider) + "/timers?include_inactive=true");
  if (response.empty()) return;

  nlohmann::json document;
  if (!parseJson(response, document)) return;
  if (!document.contains("timers") || !document["timers"].is_array()) return;

  for (const auto& timerJson : document["timers"]) {
    UltimateTimer timer;
    timer.provider = provider;

    if (!GetInt(timerJson, "ClientIndex", "client_index", timer.clientIndex)) continue;

    GetInt(timerJson, "TimerTypeId", "timer_type_id", timer.timerTypeId);
    GetString(timerJson, "Title", "title", timer.title);
    GetInt(timerJson, "ParentClientIndex", "parent_client_index", timer.parentClientIndex);

    // -1 = "any channel" (PVR_TIMER_ANY_CHANNEL). Also the result when the
    // provider's native station id does not fit into 32 bit.
    timer.clientChannelUid = -1;
    GetInt(timerJson, "ClientChannelUid", "client_channel_uid", timer.clientChannelUid);
    if (const nlohmann::json* ch = FindKey(timerJson, "ClientChannelUid", "client_channel_uid")) {
      if (ch->is_number_integer()) timer.providerChannelId = std::to_string(ch->get<long long>());
    }
    GetString(timerJson, "ChannelName", "channel_name", timer.channelName);

    GetTime(timerJson, "StartTime", "start_time", timer.startTime);
    GetTime(timerJson, "EndTime", "end_time", timer.endTime);
    GetBool(timerJson, "StartAnyTime", "start_any_time", timer.startAnyTime);
    GetBool(timerJson, "EndAnyTime", "end_any_time", timer.endAnyTime);
    GetTime(timerJson, "FirstDay", "first_day", timer.firstDay);

    GetInt(timerJson, "MarginStart", "margin_start", timer.marginStart);
    GetInt(timerJson, "MarginEnd", "margin_end", timer.marginEnd);

    timer.state = ParseTimerState(timerJson);

    GetInt(timerJson, "Weekdays", "weekdays", timer.weekdays);
    GetInt(timerJson, "PreventDuplicateEpisodes", "prevent_duplicate_episodes",
           timer.preventDuplicateEpisodes);
    GetString(timerJson, "SeriesLink", "series_link", timer.seriesLink);

    GetString(timerJson, "Directory", "directory", timer.directory);
    GetInt(timerJson, "Priority", "priority", timer.priority);
    GetInt(timerJson, "Lifetime", "lifetime", timer.lifetime);
    GetInt(timerJson, "MaxRecordings", "max_recordings", timer.maxRecordings);
    GetInt(timerJson, "RecordingGroup", "recording_group", timer.recordingGroup);

    GetString(timerJson, "EpgSearchString", "epg_search_string", timer.epgSearchString);
    GetBool(timerJson, "FullTextEpgSearch", "full_text_epg_search", timer.fullTextEpgSearch);
    GetInt(timerJson, "EpgUid", "epg_uid", timer.epgUid);
    GetString(timerJson, "EpgEventId", "epg_event_id", timer.epgEventId);

    GetInt(timerJson, "GenreType", "genre_type", timer.genreType);
    GetInt(timerJson, "GenreSubType", "genre_sub_type", timer.genreSubType);
    GetString(timerJson, "Description", "description", timer.description);

    outTimers.push_back(timer);
  }
}

bool TimerManager::GetTimerTypes(std::vector<kodi::addon::PVRTimerType>& types) const {
  std::shared_lock<std::shared_mutex> lock(m_dataMutex);
  for (const auto& timerType : m_timerTypes) {
    kodi::addon::PVRTimerType type;
    type.SetId(timerType.id);
    type.SetDescription(timerType.description);
    type.SetPrioritiesDefault(timerType.priority);

    type.SetAttributes(timerType.attributes != 0 ? MapTimerTypeAttributes(timerType.attributes)
                                                 : kLegacyTimerTypeAttributes);
    types.push_back(type);
  }
  return true;
}

int TimerManager::GetTimersAmount() const {
  std::shared_lock<std::shared_mutex> lock(m_dataMutex);
  return static_cast<int>(m_timers.size());
}

std::string TimerManager::StateSignature() const {
  std::shared_lock<std::shared_mutex> lock(m_dataMutex);
  std::string sig;
  for (const auto& t : m_timers) sig += std::to_string(t.clientIndex) + ":" + std::to_string(t.state) + ";";
  return sig;
}

bool TimerManager::GetTimers(kodi::addon::PVRTimersResultSet& results,
                             const std::map<int, ChannelLookupInfo>& channelLookup) const {
  std::shared_lock<std::shared_mutex> lock(m_dataMutex);
  for (const auto& source : m_timers) {
    UltimateTimer timer = source;

    // The backend reports the provider's own channel id; Kodi knows the channel by
    // our channel number. Without this Kodi gets "any channel" for an EPG timer.
    if (!timer.providerChannelId.empty()) {
      for (const auto& [channelUid, info] : channelLookup) {
        if (info.provider == timer.provider && info.channelId == timer.providerChannelId) {
          timer.clientChannelUid = channelUid;
          break;
        }
      }
    }

    kodi::addon::PVRTimer kodiTimer;
    MapTimerToKodi(timer, kodiTimer);
    results.Add(kodiTimer);
    kodi::Log(ADDON_LOG_DEBUG, "GetTimers: '%s' idx=%d type=%d channelUid=%d (providerChannel='%s') state=%d start=%lld",
              timer.title.c_str(), timer.clientIndex, timer.timerTypeId, timer.clientChannelUid,
              timer.providerChannelId.c_str(), timer.state, static_cast<long long>(timer.startTime));
  }
  return true;
}

bool TimerManager::MapTimerToKodi(const UltimateTimer& timer, kodi::addon::PVRTimer& kodiTimer) {
  kodiTimer.SetClientIndex(timer.clientIndex);
  kodiTimer.SetTimerType(timer.timerTypeId);
  kodiTimer.SetTitle(timer.title);
  kodiTimer.SetState(MapTimerStateToKodi(timer.state));
  kodiTimer.SetStartTime(timer.startTime);
  kodiTimer.SetEndTime(timer.endTime);

  if (timer.parentClientIndex > 0) kodiTimer.SetParentClientIndex(timer.parentClientIndex);
  kodiTimer.SetClientChannelUid(timer.clientChannelUid);
  // Note: kodi::addon::PVRTimer has no channel-name field -- Kodi resolves
  // the displayed channel name itself from ClientChannelUid, so timer.channelName
  // (kept on UltimateTimer for our own bookkeeping) is intentionally not sent.

  kodiTimer.SetStartAnyTime(timer.startAnyTime);
  kodiTimer.SetEndAnyTime(timer.endAnyTime);

  kodiTimer.SetMarginStart(timer.marginStart);
  kodiTimer.SetMarginEnd(timer.marginEnd);
  kodiTimer.SetPriority(timer.priority);
  kodiTimer.SetLifetime(timer.lifetime);
  kodiTimer.SetWeekdays(timer.weekdays);
  if (timer.firstDay > 0) kodiTimer.SetFirstDay(timer.firstDay);

  kodiTimer.SetSeriesLink(timer.seriesLink);

  kodiTimer.SetFullTextEpgSearch(timer.fullTextEpgSearch);
  if (!timer.epgSearchString.empty()) kodiTimer.SetEPGSearchString(timer.epgSearchString);
  if (timer.epgUid > 0) kodiTimer.SetEPGUid(timer.epgUid);
  if (!timer.directory.empty()) kodiTimer.SetDirectory(timer.directory);
  if (timer.maxRecordings > 0) kodiTimer.SetMaxRecordings(timer.maxRecordings);
  if (timer.recordingGroup > 0) kodiTimer.SetRecordingGroup(timer.recordingGroup);
  if (timer.genreType > 0) kodiTimer.SetGenreType(timer.genreType);
  if (timer.genreSubType > 0) kodiTimer.SetGenreSubType(timer.genreSubType);
  if (!timer.description.empty()) kodiTimer.SetSummary(timer.description);

  kodiTimer.SetPreventDuplicateEpisodes(
    static_cast<unsigned int>(timer.preventDuplicateEpisodes));

  return true;
}

bool TimerManager::MapKodiTimerToUltimate(const kodi::addon::PVRTimer& kodiTimer,
                                          UltimateTimer& ultimateTimer) {
  ultimateTimer.clientIndex = kodiTimer.GetClientIndex();
  ultimateTimer.timerTypeId = kodiTimer.GetTimerType();
  ultimateTimer.title = kodiTimer.GetTitle();
  ultimateTimer.state = kodiTimer.GetState();
  ultimateTimer.startTime = kodiTimer.GetStartTime();
  ultimateTimer.endTime = kodiTimer.GetEndTime();
  ultimateTimer.parentClientIndex = kodiTimer.GetParentClientIndex();
  ultimateTimer.clientChannelUid = kodiTimer.GetClientChannelUid();
  // Note: no GetChannelName() on kodi::addon::PVRTimer -- if we need the
  // channel name here, it has to be resolved from clientChannelUid via the
  // channel lookup, not read back off the Kodi timer object.
  ultimateTimer.startAnyTime = kodiTimer.GetStartAnyTime();
  ultimateTimer.endAnyTime = kodiTimer.GetEndAnyTime();
  ultimateTimer.marginStart = kodiTimer.GetMarginStart();
  ultimateTimer.marginEnd = kodiTimer.GetMarginEnd();
  ultimateTimer.priority = kodiTimer.GetPriority();
  ultimateTimer.lifetime = kodiTimer.GetLifetime();
  ultimateTimer.weekdays = kodiTimer.GetWeekdays();
  ultimateTimer.firstDay = kodiTimer.GetFirstDay();
  ultimateTimer.seriesLink = kodiTimer.GetSeriesLink();
  ultimateTimer.fullTextEpgSearch = kodiTimer.GetFullTextEpgSearch();
  ultimateTimer.epgSearchString = kodiTimer.GetEPGSearchString();
  ultimateTimer.epgUid = kodiTimer.GetEPGUid();
  ultimateTimer.directory = kodiTimer.GetDirectory();
  ultimateTimer.maxRecordings = kodiTimer.GetMaxRecordings();
  ultimateTimer.recordingGroup = kodiTimer.GetRecordingGroup();
  ultimateTimer.genreType = kodiTimer.GetGenreType();
  ultimateTimer.genreSubType = kodiTimer.GetGenreSubType();
  ultimateTimer.description = kodiTimer.GetSummary();
  ultimateTimer.preventDuplicateEpisodes = kodiTimer.GetPreventDuplicateEpisodes();

  return true;
}

PVR_TIMER_STATE TimerManager::MapTimerStateToKodi(int state) {
  switch (state) {
    case 0: return PVR_TIMER_STATE_SCHEDULED;
    case 1: return PVR_TIMER_STATE_SCHEDULED;
    case 2: return PVR_TIMER_STATE_RECORDING;
    case 3: return PVR_TIMER_STATE_COMPLETED;
    case 4: return PVR_TIMER_STATE_ABORTED;
    case 5: return PVR_TIMER_STATE_CANCELLED;
    case 6: return PVR_TIMER_STATE_CONFLICT_NOK;
    case 7: return PVR_TIMER_STATE_ERROR;
    default: return PVR_TIMER_STATE_SCHEDULED;
  }
}

bool TimerManager::AddTimer(const kodi::addon::PVRTimer& timer,
                            const std::vector<UltimateProvider>& providers,
                            const std::map<int, ChannelLookupInfo>& channelLookup,
                            const std::function<std::string(const std::string&)>& buildApiUrl,
                            const std::function<bool(const std::string&, const std::string&)>& httpPost,
                            const std::function<void()>& loadTimers,
                            const std::function<std::string(const std::string&)>& httpGet,
                            const std::function<bool(const std::string&, nlohmann::json&)>& parseJson) {
  UltimateTimer ultimateTimer;
  MapKodiTimerToUltimate(timer, ultimateTimer);

  std::string provider;
  std::string channelId;

  if (timer.GetClientChannelUid() > 0) {
    auto it = channelLookup.find(timer.GetClientChannelUid());
    if (it != channelLookup.end()) {
      provider = it->second.provider;
      channelId = it->second.channelId;
    }
  }
  if (provider.empty() && !providers.empty()) {
    provider = providers[0].name;
  }

  if (provider.empty()) return false;

  nlohmann::json doc = nlohmann::json::object();
  doc["timer_type_id"] = ultimateTimer.timerTypeId;
  doc["title"] = ultimateTimer.title;
  doc["provider"] = provider;
  doc["client_channel_uid"] = ultimateTimer.clientChannelUid;

  if (ultimateTimer.startTime > 0)
    doc["start_time"] = Utils::ToISO8601(ultimateTimer.startTime);
  if (ultimateTimer.endTime > 0)
    doc["end_time"] = Utils::ToISO8601(ultimateTimer.endTime);
  if (ultimateTimer.priority > 0) doc["priority"] = ultimateTimer.priority;
  if (ultimateTimer.lifetime > 0) doc["lifetime"] = ultimateTimer.lifetime;
  if (ultimateTimer.marginStart > 0) doc["margin_start"] = ultimateTimer.marginStart;
  if (ultimateTimer.marginEnd > 0) doc["margin_end"] = ultimateTimer.marginEnd;
  if (ultimateTimer.weekdays > 0) doc["weekdays"] = ultimateTimer.weekdays;
  if (ultimateTimer.firstDay > 0)
    doc["first_day"] = Utils::ToISO8601(ultimateTimer.firstDay);
  if (!ultimateTimer.seriesLink.empty())
    doc["series_link"] = ultimateTimer.seriesLink;
  if (ultimateTimer.preventDuplicateEpisodes > 0)
    doc["prevent_duplicate_episodes"] = ultimateTimer.preventDuplicateEpisodes;
  if (!ultimateTimer.epgSearchString.empty())
    doc["epg_search_string"] = ultimateTimer.epgSearchString;
  doc["full_text_epg_search"] = ultimateTimer.fullTextEpgSearch;
  if (ultimateTimer.epgUid > 0) {
    doc["epg_uid"] = ultimateTimer.epgUid;
    std::string epgEventId = EPGManager::GetEpgEventId(static_cast<unsigned int>(ultimateTimer.epgUid));
    kodi::Log(ADDON_LOG_DEBUG,
              "AddTimer: provider='%s' channelId='%s' channelUid=%d epgUid=%d start=%lld cachedEventId='%s' canResolve=%d",
              provider.c_str(), channelId.c_str(), timer.GetClientChannelUid(), ultimateTimer.epgUid,
              static_cast<long long>(ultimateTimer.startTime), epgEventId.c_str(),
              (httpGet && parseJson) ? 1 : 0);
    // The client EPG source (e.g. webepg) may not carry the listing id; ask the
    // backend's own EPG for the broadcast starting at the same time instead.
    if (epgEventId.empty() && httpGet && parseJson && !channelId.empty()) {
      epgEventId = ResolveEpgEventId(provider, channelId, ultimateTimer.startTime,
                                     ultimateTimer.endTime, httpGet, parseJson);
    }
    kodi::Log(ADDON_LOG_DEBUG, "AddTimer: epg_event_id='%s'", epgEventId.c_str());
    if (!epgEventId.empty()) doc["epg_event_id"] = epgEventId;
  }

  if (!httpPost(buildApiUrl("/api/providers/" + Utils::UrlPathEncode(provider) + "/timers"), doc.dump())) {
    return false;
  }

  loadTimers();
  return true;
}

std::string TimerManager::ResolveEpgEventId(
    const std::string& provider, const std::string& channelId, time_t startTime, time_t endTime,
    const std::function<std::string(const std::string&)>& httpGet,
    const std::function<bool(const std::string&, nlohmann::json&)>& parseJson) {
  if (startTime <= 0) return "";

  // Widen the window so a start margin / rounding cannot exclude the broadcast.
  const time_t from = startTime - 3600;
  const time_t to = (endTime > startTime ? endTime : startTime) + 3600;
  std::string response = httpGet("/api/providers/" + Utils::UrlPathEncode(provider) + "/channels/" +
                                 Utils::UrlPathEncode(channelId) + "/epg?start_time=" +
                                 std::to_string(from) + "&end_time=" + std::to_string(to));
  if (response.empty()) return "";

  nlohmann::json doc;
  if (!parseJson(response, doc)) return "";

  const nlohmann::json* list = nullptr;
  if (doc.contains("programs") && doc["programs"].is_array()) list = &doc["programs"];
  else if (doc.contains("epg") && doc["epg"].is_array()) list = &doc["epg"];
  if (!list) return "";

  for (const auto& item : *list) {
    if (!item.is_object() || !item.contains("start") || !item["start"].is_number_integer()) continue;
    // Kodi's EPG start and the provider's start can differ by a few seconds.
    long long diff = item["start"].get<long long>() - static_cast<long long>(startTime);
    if (diff < -60 || diff > 60) continue;
    if (item.contains("epg_event_id") && item["epg_event_id"].is_string()) {
      std::string id = item["epg_event_id"].get<std::string>();
      if (!id.empty()) return id;
    }
  }
  return "";
}

bool TimerManager::DeleteTimer(int clientIndex, bool forceDelete,
                               const std::function<std::string(const std::string&)>& buildApiUrl,
                               const std::function<bool(const std::string&)>& httpDelete,
                               const std::function<void()>& loadTimers) {
  std::string provider;
  bool found = false;

  {
    std::shared_lock<std::shared_mutex> lock(m_dataMutex);
    UltimateTimer* ultimateTimer = FindTimer(clientIndex);
    if (!ultimateTimer) return false;
    provider = ultimateTimer->provider;
    found = true;
  }

  if (!found) return false;

  std::string url = buildApiUrl("/api/providers/" + Utils::UrlPathEncode(provider) + "/timers/" + std::to_string(clientIndex));
  if (forceDelete) url += "?force=true";

  if (!httpDelete(url)) return false;

  loadTimers();
  return true;
}

bool TimerManager::UpdateTimer(const kodi::addon::PVRTimer& timer,
                               const std::function<std::string(const std::string&)>& buildApiUrl,
                               const std::function<bool(const std::string&, const std::string&)>& httpPut,
                               const std::function<void()>& loadTimers) {
  int clientIndex = timer.GetClientIndex();

  std::shared_lock<std::shared_mutex> lock(m_dataMutex);
  UltimateTimer* existingTimer = FindTimer(clientIndex);
  if (!existingTimer) return false;

  UltimateTimer updatedTimer;
  MapKodiTimerToUltimate(timer, updatedTimer);
  updatedTimer.provider = existingTimer->provider;
  lock.unlock();

  nlohmann::json doc = nlohmann::json::object();
  doc["timer_type_id"] = updatedTimer.timerTypeId;
  doc["title"] = updatedTimer.title;
  doc["client_channel_uid"] = updatedTimer.clientChannelUid;

  if (updatedTimer.startTime > 0)
    doc["start_time"] = Utils::ToISO8601(updatedTimer.startTime);
  if (updatedTimer.endTime > 0)
    doc["end_time"] = Utils::ToISO8601(updatedTimer.endTime);
  if (updatedTimer.priority > 0) doc["priority"] = updatedTimer.priority;
  if (updatedTimer.lifetime > 0) doc["lifetime"] = updatedTimer.lifetime;
  if (updatedTimer.marginStart > 0) doc["margin_start"] = updatedTimer.marginStart;
  if (updatedTimer.marginEnd > 0) doc["margin_end"] = updatedTimer.marginEnd;
  if (updatedTimer.weekdays > 0) doc["weekdays"] = updatedTimer.weekdays;
  if (updatedTimer.firstDay > 0)
    doc["first_day"] = Utils::ToISO8601(updatedTimer.firstDay);
  if (!updatedTimer.seriesLink.empty())
    doc["series_link"] = updatedTimer.seriesLink;
  if (updatedTimer.preventDuplicateEpisodes > 0)
    doc["prevent_duplicate_episodes"] = updatedTimer.preventDuplicateEpisodes;
  if (!updatedTimer.epgSearchString.empty())
    doc["epg_search_string"] = updatedTimer.epgSearchString;
  doc["full_text_epg_search"] = updatedTimer.fullTextEpgSearch;
  if (updatedTimer.epgUid > 0) {
    doc["epg_uid"] = updatedTimer.epgUid;
    std::string epgEventId = EPGManager::GetEpgEventId(static_cast<unsigned int>(updatedTimer.epgUid));
    if (!epgEventId.empty()) doc["epg_event_id"] = epgEventId;
  }

  if (!httpPut(buildApiUrl("/api/providers/" + Utils::UrlPathEncode(updatedTimer.provider) + "/timers/" + std::to_string(clientIndex)),
               doc.dump())) {
    return false;
  }

  loadTimers();
  return true;
}

UltimateTimer* TimerManager::FindTimer(int clientIndex) {
  for (auto& timer : m_timers) {
    if (timer.clientIndex == clientIndex) return &timer;
  }
  return nullptr;
}