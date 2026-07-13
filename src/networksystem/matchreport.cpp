//=============================================================================//
//
// Purpose: Reports match-end player metrics to a remote service.
//
//=============================================================================//

#include "core/stdafx.h"
#include "core/logdef.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <ctime>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include "tier1/cvar.h"
#include "tier2/jsonutils.h"
#include "engine/client/client.h"
#include "engine/server/server.h"
#include "game/server/player.h"
#include "game/server/util_server.h"
#include "networksystem/matchreport.h"
#include "networksystem/remoteapi.h"
#include "rtech/playlists/playlists.h"

//-----------------------------------------------------------------------------
// Console variables
//-----------------------------------------------------------------------------
static ConVar sv_match_report_enable("sv_match_report_enable", "0", FCVAR_RELEASE | FCVAR_ACCESSIBLE_FROM_THREADS, "Enables match-end player metric reporting.", false, 0.f, true, 1.f, "0 = Disable, 1 = Enable.");
static ConVar sv_match_report_url("sv_match_report_url", "", FCVAR_RELEASE | FCVAR_ACCESSIBLE_FROM_THREADS, "Full HTTP(S) URL for the match-end player metric endpoint.");
static ConVar sv_match_report_ssl_verify_peer("sv_match_report_ssl_verify_peer", "1", FCVAR_RELEASE | FCVAR_ACCESSIBLE_FROM_THREADS, "Verifies HTTPS peer certificates for the match-end player metric endpoint.", false, 0.f, true, 1.f, "0 = Disable, 1 = Enable.");
static ConVar sv_match_report_debug("sv_match_report_debug", "0", FCVAR_RELEASE | FCVAR_ACCESSIBLE_FROM_THREADS, "Enables verbose curl logging and response logging for match-end reports.", false, 0.f, true, 1.f, "0 = Disable, 1 = Enable.");

struct MatchReportWeaponStat_t
{
	string m_svWeapon;
	int m_nShots = 0;
	int m_nHits = 0;
	float m_flBulletsHit = 0.0f;
	float m_flDamage = 0.0f;
	int m_nHeadshots = 0;
	int m_nKills = 0;
};

struct MatchReportKillEvent_t
{
	int m_nTick = 0;
	int64_t m_nRecordedAt = 0;
	int m_nDamageSourceId = 0;
	NucleusID_t m_nAttackerNucleusID = 0;
	NucleusID_t m_nVictimNucleusID = 0;
	string m_svAttackerUid;
	string m_svAttackerName;
	string m_svVictimUid;
	string m_svVictimName;
	string m_svWeapon;
};

struct MatchReportPlayer_t
{
	int m_nUserID = 0;
	int m_nHandle = 0;
	int m_nSignonState = 0;
	int m_nTeam = 0;
	int m_nLifeState = 0;
	NucleusID_t m_nNucleusID = 0;
	bool m_bConnected = false;
	bool m_bBot = false;
	string m_svUID;
	string m_svPersonaName;
	string m_svInputDevice = "unknown";
	MatchMetrics m_Metrics = {};
	vector<pair<string, int>> m_WeaponKills;
	vector<MatchReportWeaponStat_t> m_WeaponStats;
};

struct MatchReportPlayerSnapshot_t
{
	int m_nUserID = 0;
	int m_nHandle = 0;
	int m_nSignonState = 0;
	int m_nTeam = 0;
	int m_nLifeState = 0;
	NucleusID_t m_nNucleusID = 0;
	bool m_bConnected = false;
	bool m_bBot = false;
	bool m_bHasMetrics = false;
	string m_svUID;
	string m_svPersonaName;
	string m_svInputDevice = "unknown";
	MatchMetrics m_Metrics = {};
};

struct MatchReport_t
{
	int m_nTick = 0;
	int m_nSpawnCount = 0;
	int m_nNumPlayers = 0;
	int m_nMaxPlayers = 0;
	int64_t m_nEndedAt = 0;
	string m_svServerName;
	string m_svServerIp;
	int m_nServerPort = 0;
	string m_svMap;
	string m_svPlaylist;
	vector<MatchReportPlayer_t> m_Players;
	vector<MatchReportKillEvent_t> m_KillEvents;
};

static constexpr const char* MATCH_REPORT_DEFAULT_PATH = "/api/r5/matches/end";

static std::atomic_bool s_bMatchReportInFlight(false);
static int s_nLastReportedSpawnCount = -1;
static string s_svLastReportedMap;
static std::recursive_mutex s_MatchReportStatsMutex;
static std::unique_ptr<MatchReport_t> s_PendingMatchReport;
static std::unordered_map<NucleusID_t, MatchReportPlayerSnapshot_t> s_PlayerSnapshots;
static std::unordered_map<NucleusID_t, std::unordered_map<string, int>> s_PlayerWeaponKills;
static std::unordered_map<NucleusID_t, std::unordered_map<string, MatchReportWeaponStat_t>> s_PlayerWeaponStats;
static vector<MatchReportKillEvent_t> s_KillEvents;
static std::atomic_uint64_t s_nDebugWeaponKillRecords(0);
static std::atomic_uint64_t s_nDebugPlayerKillRecords(0);
static std::atomic_uint64_t s_nDebugWeaponShotRecords(0);
static std::atomic_uint64_t s_nDebugWeaponHitRecords(0);

static const char* SV_GetMatchReportEndpointMode(const char* const pszConfiguredUrl)
{
	if (VALID_CHARSTAR(pszConfiguredUrl))
		return "custom";

	return VALID_CHARSTAR(sv_r5_api_base_url.GetString()) ? "base" : "missing";
}

static bool SV_ShouldCollectMatchReportStats()
{
	return sv_match_report_enable.GetBool() && !SV_IsRemoteApiShutdownRequested();
}

void SV_AppendMatchReportConfigSummary(string& outSummary)
{
	if (!sv_match_report_enable.GetBool())
		return;

	if (!outSummary.empty())
		outSummary.append("; ");

	outSummary.append(Format("match_report(url=%s ssl=%d debug=%d)",
		SV_GetMatchReportEndpointMode(sv_match_report_url.GetString()),
		sv_match_report_ssl_verify_peer.GetBool() ? 1 : 0,
		sv_match_report_debug.GetBool() ? 1 : 0));
}

static std::shared_ptr<spdlog::logger> SV_GetMatchReportDebugLogger()
{
	static std::mutex s_LoggerMutex;
	std::lock_guard<std::mutex> lock(s_LoggerMutex);

	std::shared_ptr<spdlog::logger> logger = spdlog::get("match_report_debug");
	if (logger)
		return logger;

	const string svLogDirectory = g_LogSessionDirectory.empty() ? "platform/logs" : g_LogSessionDirectory;
	try
	{
		logger = spdlog::rotating_logger_mt<spdlog::synchronous_factory>(
			"match_report_debug",
			fmt::format("{:s}/{:s}", svLogDirectory, "match_report_debug.log"),
			SPDLOG_MAX_SIZE,
			SPDLOG_NUM_FILE);
		logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %v");
		return logger;
	}
	catch (const spdlog::spdlog_ex& ex)
	{
		Warning(eDLL_T::SERVER, "Failed to create match report debug logger: %s\n", ex.what());
		return nullptr;
	}
}

template <typename... Args>
static void SV_MatchReportDebugLogEnabled(const bool bEnabled, const char* const pszFormat, Args&&... args)
{
	if (!bEnabled)
		return;

	std::shared_ptr<spdlog::logger> logger = SV_GetMatchReportDebugLogger();
	if (!logger)
		return;

	logger->info(pszFormat, std::forward<Args>(args)...);
	logger->flush();
}

template <typename... Args>
static void SV_MatchReportDebugLog(const char* const pszFormat, Args&&... args)
{
	SV_MatchReportDebugLogEnabled(SV_IsRemoteApiDebugEnabled() || sv_match_report_debug.GetBool(),
		pszFormat, std::forward<Args>(args)...);
}

void SV_MatchReportDebugNote(const char* const pszMessage)
{
	if (!VALID_CHARSTAR(pszMessage) || SV_IsRemoteApiShutdownRequested())
		return;

	SV_MatchReportDebugLog("[script] {}", pszMessage);
}

static size_t SV_CountMatchReportWeaponKillRows(const MatchReport_t& report)
{
	size_t nRows = 0;
	for (const MatchReportPlayer_t& player : report.m_Players)
		nRows += player.m_WeaponKills.size();

	return nRows;
}

static size_t SV_CountMatchReportWeaponStatRows(const MatchReport_t& report)
{
	size_t nRows = 0;
	for (const MatchReportPlayer_t& player : report.m_Players)
		nRows += player.m_WeaponStats.size();

	return nRows;
}

static void SV_LogMatchReportSummary(const char* const pszStage, const MatchReport_t& report, const bool bDebugEnabled)
{
	if (!bDebugEnabled)
		return;

	std::lock_guard<std::recursive_mutex> lock(s_MatchReportStatsMutex);

	SV_MatchReportDebugLogEnabled(
		bDebugEnabled,
		"[{}] map={} playlist={} spawn={} tick={} players={} max={} killEvents={} weaponKillRows={} weaponStatRows={} recordCounters={{weaponKill={}, playerKill={}, weaponShot={}, weaponHit={}}} cachedMaps={{snapshots={}, weaponKills={}, weaponStats={}, killEvents={}}}",
		pszStage ? pszStage : "match-report",
		report.m_svMap,
		report.m_svPlaylist,
		report.m_nSpawnCount,
		report.m_nTick,
		report.m_nNumPlayers,
		report.m_nMaxPlayers,
		report.m_KillEvents.size(),
		SV_CountMatchReportWeaponKillRows(report),
		SV_CountMatchReportWeaponStatRows(report),
		s_nDebugWeaponKillRecords.load(),
		s_nDebugPlayerKillRecords.load(),
		s_nDebugWeaponShotRecords.load(),
		s_nDebugWeaponHitRecords.load(),
		s_PlayerSnapshots.size(),
		s_PlayerWeaponKills.size(),
		s_PlayerWeaponStats.size(),
		s_KillEvents.size());

	for (const MatchReportPlayer_t& player : report.m_Players)
	{
		SV_MatchReportDebugLogEnabled(
			bDebugEnabled,
			"[{}] player uid={} name={} connected={} bot={} inputDevice={} kills={} damage={} shotsFired={} shotsHit={} weaponKills={} weaponStats={}",
			pszStage ? pszStage : "match-report",
			player.m_svUID,
			player.m_svPersonaName,
			player.m_bConnected ? 1 : 0,
			player.m_bBot ? 1 : 0,
			player.m_svInputDevice,
			player.m_Metrics.kills,
			player.m_Metrics.damage,
			player.m_Metrics.shotsFired,
			player.m_Metrics.shotsHit,
			player.m_WeaponKills.size(),
			player.m_WeaponStats.size());
	}
}

static string SV_FixedCharArrayToString(const char* const pszString, const size_t nMaxLength)
{
	if (!pszString)
		return "";

	size_t nLength = 0;
	while (nLength < nMaxLength && pszString[nLength] != '\0')
		nLength++;

	return string(pszString, nLength);
}

static void SV_AddJsonString(rapidjson::Value& object, const char* const pszName,
	const string& svValue, rapidjson::Document::AllocatorType& allocator)
{
	object.AddMember(rapidjson::StringRef(pszName),
		rapidjson::Value(svValue.c_str(), svValue.length(), allocator), allocator);
}

static void SV_AddJsonCString(rapidjson::Value& object, const char* const pszName,
	const char* pszValue, rapidjson::Document::AllocatorType& allocator)
{
	if (!pszValue)
		pszValue = "";

	object.AddMember(rapidjson::StringRef(pszName),
		rapidjson::Value(pszValue, strlen(pszValue), allocator), allocator);
}

static void SV_AddMatchMetricsObject(rapidjson::Value& player, const MatchMetrics& metrics,
	rapidjson::Document::AllocatorType& allocator)
{
	rapidjson::Value metricsObject(rapidjson::kObjectType);
	const string svCharacterName = SV_FixedCharArrayToString(metrics.characterName, sizeof(metrics.characterName));
	const string svRankedPeriodName = SV_FixedCharArrayToString(metrics.rankedPeriodName, sizeof(metrics.rankedPeriodName));
	const float flAccuracy = metrics.shotsFired > 0 ? (static_cast<float>(metrics.shotsHit) / static_cast<float>(metrics.shotsFired)) : 0.0f;

	metricsObject.AddMember("hotDropped", metrics.hotDropped, allocator);
	metricsObject.AddMember("relinquished", metrics.relinquished, allocator);
	metricsObject.AddMember("allPings", metrics.allPings, allocator);
	metricsObject.AddMember("locationPings", metrics.locationPings, allocator);
	metricsObject.AddMember("enemyPings", metrics.enemyPings, allocator);
	metricsObject.AddMember("shotsFired", metrics.shotsFired, allocator);
	metricsObject.AddMember("shotsHit", metrics.shotsHit, allocator);
	metricsObject.AddMember("accuracy", flAccuracy, allocator);
	metricsObject.AddMember("accuracyPercent", flAccuracy * 100.0f, allocator);
	metricsObject.AddMember("level", metrics.level, allocator);
	metricsObject.AddMember("matches", metrics.matches, allocator);
	metricsObject.AddMember("wins", metrics.wins, allocator);
	metricsObject.AddMember("winsWithFriends", metrics.winsWithFriends, allocator);
	metricsObject.AddMember("timesJumpmaster", metrics.timesJumpmaster, allocator);
	metricsObject.AddMember("winsAsJumpmaster", metrics.winsAsJumpmaster, allocator);
	metricsObject.AddMember("damage", metrics.damage, allocator);
	metricsObject.AddMember("damageTaken", metrics.damageTaken, allocator);
	metricsObject.AddMember("kills", metrics.kills, allocator);
	metricsObject.AddMember("teamworkKills", metrics.teamworkKills, allocator);
	metricsObject.AddMember("timesRevivedAlly", metrics.timesRevivedAlly, allocator);
	metricsObject.AddMember("characterPickOrder", metrics.characterPickOrder, allocator);
	SV_AddJsonString(metricsObject, "characterName", svCharacterName, allocator);
	SV_AddJsonString(metricsObject, "rankedPeriodName", svRankedPeriodName, allocator);
	metricsObject.AddMember("rankedScore", metrics.rankedScore, allocator);

	player.AddMember("metrics", metricsObject, allocator);
}

static void SV_AddTrackerCompatObject(rapidjson::Value& player, const MatchMetrics& metrics,
	rapidjson::Document::AllocatorType& allocator)
{
	rapidjson::Value trackerObject(rapidjson::kObjectType);

	trackerObject.AddMember("kills", metrics.kills, allocator);
	trackerObject.AddMember("damage", metrics.damage, allocator);
	trackerObject.AddMember("score", metrics.rankedScore, allocator);
	trackerObject.AddMember("total_matches", metrics.matches, allocator);
	trackerObject.AddMember("previous_kills", metrics.kills, allocator);
	trackerObject.AddMember("previous_damage", metrics.damage, allocator);
	trackerObject.AddMember("previous_score", metrics.rankedScore, allocator);

	player.AddMember("tracker", trackerObject, allocator);
}

static void SV_AddWeaponKillsObject(rapidjson::Value& player, const vector<pair<string, int>>& weaponKills,
	rapidjson::Document::AllocatorType& allocator)
{
	rapidjson::Value weaponKillsArray(rapidjson::kArrayType);

	for (const pair<string, int>& weaponKill : weaponKills)
	{
		rapidjson::Value weaponKillObject(rapidjson::kObjectType);
		SV_AddJsonString(weaponKillObject, "weapon", weaponKill.first, allocator);
		weaponKillObject.AddMember("kills", weaponKill.second, allocator);

		weaponKillsArray.PushBack(weaponKillObject, allocator);
	}

	player.AddMember("weaponKills", weaponKillsArray, allocator);
}

static void SV_AddWeaponStatsObject(rapidjson::Value& player, const vector<MatchReportWeaponStat_t>& weaponStats,
	rapidjson::Document::AllocatorType& allocator)
{
	rapidjson::Value weaponStatsArray(rapidjson::kArrayType);

	for (const MatchReportWeaponStat_t& weaponStat : weaponStats)
	{
		rapidjson::Value weaponStatObject(rapidjson::kObjectType);
		const float flAccuracy = weaponStat.m_nShots > 0
			? (static_cast<float>(weaponStat.m_nHits) / static_cast<float>(weaponStat.m_nShots))
			: 0.0f;

		SV_AddJsonString(weaponStatObject, "weapon", weaponStat.m_svWeapon, allocator);
		weaponStatObject.AddMember("shots", weaponStat.m_nShots, allocator);
		weaponStatObject.AddMember("hits", weaponStat.m_nHits, allocator);
		weaponStatObject.AddMember("bulletsHit", weaponStat.m_flBulletsHit, allocator);
		weaponStatObject.AddMember("damage", weaponStat.m_flDamage, allocator);
		weaponStatObject.AddMember("headshots", weaponStat.m_nHeadshots, allocator);
		weaponStatObject.AddMember("kills", weaponStat.m_nKills, allocator);
		weaponStatObject.AddMember("accuracy", flAccuracy, allocator);
		weaponStatObject.AddMember("accuracyPercent", flAccuracy * 100.0f, allocator);

		weaponStatsArray.PushBack(weaponStatObject, allocator);
	}

	player.AddMember("weaponStats", weaponStatsArray, allocator);
}

static void SV_AddKillEventsObject(rapidjson::Value& requestJson, const vector<MatchReportKillEvent_t>& killEvents,
	rapidjson::Document::AllocatorType& allocator)
{
	rapidjson::Value killEventsArray(rapidjson::kArrayType);

	for (const MatchReportKillEvent_t& killEvent : killEvents)
	{
		rapidjson::Value killEventObject(rapidjson::kObjectType);

		killEventObject.AddMember("tick", killEvent.m_nTick, allocator);
		killEventObject.AddMember("recordedAt", killEvent.m_nRecordedAt, allocator);
		killEventObject.AddMember("damageSourceId", killEvent.m_nDamageSourceId, allocator);
		SV_AddJsonString(killEventObject, "attackerUid", killEvent.m_svAttackerUid, allocator);
		killEventObject.AddMember("attackerNucleusId", static_cast<uint64_t>(killEvent.m_nAttackerNucleusID), allocator);
		SV_AddJsonString(killEventObject, "attackerName", killEvent.m_svAttackerName, allocator);
		SV_AddJsonString(killEventObject, "victimUid", killEvent.m_svVictimUid, allocator);
		killEventObject.AddMember("victimNucleusId", static_cast<uint64_t>(killEvent.m_nVictimNucleusID), allocator);
		SV_AddJsonString(killEventObject, "victimName", killEvent.m_svVictimName, allocator);
		SV_AddJsonString(killEventObject, "weapon", killEvent.m_svWeapon, allocator);

		killEventsArray.PushBack(killEventObject, allocator);
	}

	requestJson.AddMember("killEvents", killEventsArray, allocator);
}

static void SV_BuildMatchReportRequest(const MatchReport_t& report, rapidjson::Document& requestJson)
{
	requestJson.SetObject();

	rapidjson::Document::AllocatorType& allocator = requestJson.GetAllocator();
	rapidjson::Value playersArray(rapidjson::kArrayType);

	for (const MatchReportPlayer_t& reportPlayer : report.m_Players)
	{
		rapidjson::Value player(rapidjson::kObjectType);

		SV_AddJsonString(player, "uid", reportPlayer.m_svUID, allocator);
		player.AddMember("nucleusId", static_cast<uint64_t>(reportPlayer.m_nNucleusID), allocator);
		SV_AddJsonString(player, "playerName", reportPlayer.m_svPersonaName, allocator);
		SV_AddJsonString(player, "inputDevice", reportPlayer.m_svInputDevice, allocator);
		player.AddMember("userId", reportPlayer.m_nUserID, allocator);
		player.AddMember("handle", reportPlayer.m_nHandle, allocator);
		player.AddMember("signonState", reportPlayer.m_nSignonState, allocator);
		player.AddMember("team", reportPlayer.m_nTeam, allocator);
		player.AddMember("lifeState", reportPlayer.m_nLifeState, allocator);
		player.AddMember("connected", reportPlayer.m_bConnected, allocator);
		player.AddMember("bot", reportPlayer.m_bBot, allocator);
		player.AddMember("eliminated", reportPlayer.m_nLifeState != 0, allocator);

		SV_AddMatchMetricsObject(player, reportPlayer.m_Metrics, allocator);
		SV_AddTrackerCompatObject(player, reportPlayer.m_Metrics, allocator);
		SV_AddWeaponKillsObject(player, reportPlayer.m_WeaponKills, allocator);
		SV_AddWeaponStatsObject(player, reportPlayer.m_WeaponStats, allocator);

		playersArray.PushBack(player, allocator);
	}

	SV_AddRemoteServerName(requestJson, allocator, report.m_svServerName);
	SV_AddRemoteServerAddress(requestJson, allocator, report.m_svServerIp, report.m_nServerPort);
	SV_AddJsonString(requestJson, "map", report.m_svMap, allocator);
	SV_AddJsonString(requestJson, "playlist", report.m_svPlaylist, allocator);
	SV_AddJsonCString(requestJson, "sdkVersion", SDK_VERSION, allocator);
	requestJson.AddMember("tick", report.m_nTick, allocator);
	requestJson.AddMember("spawnCount", report.m_nSpawnCount, allocator);
	requestJson.AddMember("endedAt", report.m_nEndedAt, allocator);
	requestJson.AddMember("numPlayers", report.m_nNumPlayers, allocator);
	requestJson.AddMember("maxPlayers", report.m_nMaxPlayers, allocator);
	requestJson.AddMember("players", playersArray, allocator);
	SV_AddKillEventsObject(requestJson, report.m_KillEvents, allocator);
}

static bool SV_SendMatchReportJson(const RemoteApiRequest_t& request,
	const rapidjson::Document& requestJson, string& outMessage)
{
	rapidjson::StringBuffer stringBuffer;
	JSON_DocumentToBufferDeserialize(requestJson, stringBuffer);
	SV_MatchReportDebugLogEnabled(request.m_bVerbose, "[send] url={} requestBytes={}", request.m_svUrl, stringBuffer.GetSize());
	SV_MatchReportDebugLogEnabled(request.m_bVerbose, "[send] payload={}", stringBuffer.GetString());

	RemoteApiResponse_t response;
	const bool bSuccess = SV_SendRemoteApiJsonRequest(request, requestJson, response,
		outMessage, "match report", false);

	if (!response.m_svDebugMessage.empty())
	{
		SV_MatchReportDebugLogEnabled(request.m_bVerbose, "[send] {}", response.m_svDebugMessage);
		Msg(eDLL_T::SERVER, "%s\n", response.m_svDebugMessage.c_str());
	}

	if (!bSuccess)
	{
		SV_MatchReportDebugLogEnabled(request.m_bVerbose, "[send] failed: {}", outMessage);
		return false;
	}

	return true;
}

static void SV_CopyMatchReportWeaponKills(const NucleusID_t nNucleusID, vector<pair<string, int>>& outWeaponKills)
{
	std::lock_guard<std::recursive_mutex> lock(s_MatchReportStatsMutex);

	const auto playerIt = s_PlayerWeaponKills.find(nNucleusID);
	if (playerIt == s_PlayerWeaponKills.end())
		return;

	for (const auto& weaponIt : playerIt->second)
	{
		if (weaponIt.second <= 0)
			continue;

		outWeaponKills.emplace_back(weaponIt.first, weaponIt.second);
	}

	std::sort(outWeaponKills.begin(), outWeaponKills.end(),
		[](const pair<string, int>& left, const pair<string, int>& right)
		{
			if (left.second != right.second)
				return left.second > right.second;

			return left.first < right.first;
		});
}

static void SV_CopyMatchReportWeaponStats(const NucleusID_t nNucleusID, vector<MatchReportWeaponStat_t>& outWeaponStats)
{
	std::lock_guard<std::recursive_mutex> lock(s_MatchReportStatsMutex);

	const auto playerIt = s_PlayerWeaponStats.find(nNucleusID);
	if (playerIt == s_PlayerWeaponStats.end())
		return;

	for (const auto& weaponIt : playerIt->second)
	{
		const MatchReportWeaponStat_t& stat = weaponIt.second;
		if (stat.m_nShots <= 0 && stat.m_nHits <= 0 && stat.m_flDamage <= 0.0f && stat.m_nKills <= 0)
			continue;

		outWeaponStats.emplace_back(stat);
	}

	std::sort(outWeaponStats.begin(), outWeaponStats.end(),
		[](const MatchReportWeaponStat_t& left, const MatchReportWeaponStat_t& right)
		{
			if (left.m_nKills != right.m_nKills)
				return left.m_nKills > right.m_nKills;

			if (left.m_flDamage != right.m_flDamage)
				return left.m_flDamage > right.m_flDamage;

			return left.m_svWeapon < right.m_svWeapon;
		});
}

static void SV_ClearMatchReportScriptStats()
{
	std::lock_guard<std::recursive_mutex> lock(s_MatchReportStatsMutex);

	s_PlayerSnapshots.clear();
	s_PlayerWeaponKills.clear();
	s_PlayerWeaponStats.clear();
	s_KillEvents.clear();
	s_nDebugWeaponKillRecords.store(0);
	s_nDebugPlayerKillRecords.store(0);
	s_nDebugWeaponShotRecords.store(0);
	s_nDebugWeaponHitRecords.store(0);
}

static NucleusID_t SV_ParseMatchReportNucleusID(const char* const pszUid)
{
	if (!VALID_CHARSTAR(pszUid))
		return 0;

	for (const unsigned char* p = reinterpret_cast<const unsigned char*>(pszUid); *p; ++p)
	{
		if (*p < '0' || *p > '9')
			return 0;
	}

	char* pEnd = nullptr;
	errno = 0;
	const uint64_t nUid = strtoull(pszUid, &pEnd, 10);
	if (pEnd == pszUid || *pEnd != '\0' || errno == ERANGE || nUid == 0 ||
		nUid > static_cast<uint64_t>((std::numeric_limits<NucleusID_t>::max)()))
		return 0;

	return static_cast<NucleusID_t>(nUid);
}

static bool SV_IsDuplicateMatchReportKillEvent(const int nTick, const NucleusID_t nAttackerNucleusID,
	const NucleusID_t nVictimNucleusID, const char* const pszVictimUid,
	const char* const pszWeaponName, const int nDamageSourceId)
{
	std::lock_guard<std::recursive_mutex> lock(s_MatchReportStatsMutex);

	const string svVictimUid = VALID_CHARSTAR(pszVictimUid) ? pszVictimUid : "";
	const string svWeaponName = VALID_CHARSTAR(pszWeaponName) ? pszWeaponName : "";

	for (auto it = s_KillEvents.rbegin(); it != s_KillEvents.rend(); ++it)
	{
		if (it->m_nTick != nTick)
			continue;

		if (it->m_nAttackerNucleusID != nAttackerNucleusID)
			continue;

		const bool bSameVictim = nVictimNucleusID != 0
			? it->m_nVictimNucleusID == nVictimNucleusID
			: it->m_svVictimUid == svVictimUid;
		if (!bSameVictim)
			continue;

		if (it->m_nDamageSourceId != nDamageSourceId || it->m_svWeapon != svWeaponName)
			continue;

		return true;
	}

	return false;
}

static int SV_GetMatchReportClientIndex(CPlayer* const pPlayer)
{
	if (!pPlayer)
		return -1;

	return pPlayer->GetEdict() - 1;
}

static CClient* SV_GetMatchReportClient(CPlayer* const pPlayer)
{
	if (!g_pServer)
		return nullptr;

	const int nClientIndex = SV_GetMatchReportClientIndex(pPlayer);
	if (nClientIndex < 0 || nClientIndex >= g_pServer->GetMaxClients())
		return nullptr;

	CClient* const pClient = g_pServer->GetClient(nClientIndex);
	if (!pClient || !pClient->IsConnected())
		return nullptr;

	if (pClient->GetHandle() != pPlayer->GetEdict())
		return nullptr;

	if (UTIL_PlayerByIndex(pClient->GetHandle()) != pPlayer)
		return nullptr;

	return pClient;
}

static NucleusID_t SV_GetMatchReportNucleusID(CPlayer* const pPlayer)
{
	if (!pPlayer)
		return 0;

	CClient* const pClient = SV_GetMatchReportClient(pPlayer);
	return pClient ? pClient->GetNucleusID() : 0;
}

static string SV_GetMatchReportPlayerName(CPlayer* const pPlayer)
{
	if (!pPlayer)
		return "";

	CClient* const pClient = SV_GetMatchReportClient(pPlayer);
	const char* pszName = pClient ? pClient->GetClientName() : nullptr;
	return VALID_CHARSTAR(pszName) ? pszName : "";
}

static const char* SV_GetMatchReportInputDeviceName(CPlayer* const pPlayer)
{
	if (!pPlayer || !pPlayer->IsConnected() || pPlayer->IsBot() || !SV_GetMatchReportClient(pPlayer))
		return "unknown";

	return pPlayer->IsControllerModeActive() ? "controller" : "keyboard_mouse";
}

static void SV_UpdateMatchReportPlayerSnapshotForId(const NucleusID_t nNucleusID, const char* const pszPlayerName)
{
	if (nNucleusID == 0)
		return;

	std::lock_guard<std::recursive_mutex> lock(s_MatchReportStatsMutex);

	MatchReportPlayerSnapshot_t& snapshot = s_PlayerSnapshots[nNucleusID];
	snapshot.m_nNucleusID = nNucleusID;
	snapshot.m_svUID = Format("%llu", nNucleusID);

	if (VALID_CHARSTAR(pszPlayerName))
		snapshot.m_svPersonaName = pszPlayerName;
}

void SV_TouchMatchReportPlayerIdentity(const NucleusID_t nNucleusID, const char* const pszPlayerName)
{
	if (!SV_ShouldCollectMatchReportStats())
		return;

	SV_UpdateMatchReportPlayerSnapshotForId(nNucleusID, pszPlayerName);
}

static void SV_UpdateMatchReportPlayerSnapshot(CPlayer* const pPlayer)
{
	if (!pPlayer || !pPlayer->IsConnected())
		return;

	CClient* const pClient = SV_GetMatchReportClient(pPlayer);
	if (!pClient)
		return;

	const NucleusID_t nNucleusID = pClient->GetNucleusID();
	if (nNucleusID == 0)
		return;

	std::lock_guard<std::recursive_mutex> lock(s_MatchReportStatsMutex);

	MatchReportPlayerSnapshot_t& snapshot = s_PlayerSnapshots[nNucleusID];

	snapshot.m_nUserID = pClient->GetUserID();
	snapshot.m_nHandle = pClient->GetHandle();
	snapshot.m_nSignonState = static_cast<int>(pClient->GetSignonState());
	snapshot.m_nTeam = pPlayer->GetTeamNum();
	snapshot.m_nLifeState = pPlayer->GetLifeState();
	snapshot.m_nNucleusID = nNucleusID;
	snapshot.m_bConnected = pPlayer->IsConnected();
	snapshot.m_bBot = pPlayer->IsBot();
	snapshot.m_bHasMetrics = true;
	snapshot.m_svUID = Format("%llu", nNucleusID);
	snapshot.m_Metrics = pPlayer->GetMatchMetrics();
	snapshot.m_svInputDevice = SV_GetMatchReportInputDeviceName(pPlayer);

	const string svPlayerName = SV_GetMatchReportPlayerName(pPlayer);
	if (!svPlayerName.empty())
		snapshot.m_svPersonaName = svPlayerName;
}

void SV_TouchMatchReportPlayer(CPlayer* const pPlayer)
{
	if (!SV_ShouldCollectMatchReportStats())
		return;

	SV_UpdateMatchReportPlayerSnapshot(pPlayer);
}

static void SV_CopyMatchReportPlayerSnapshot(const NucleusID_t nNucleusID, MatchReportPlayer_t& outPlayer)
{
	std::lock_guard<std::recursive_mutex> lock(s_MatchReportStatsMutex);

	outPlayer.m_nNucleusID = nNucleusID;
	outPlayer.m_svUID = Format("%llu", nNucleusID);

	const auto snapshotIt = s_PlayerSnapshots.find(nNucleusID);
	if (snapshotIt != s_PlayerSnapshots.end())
	{
		const MatchReportPlayerSnapshot_t& snapshot = snapshotIt->second;
		outPlayer.m_nUserID = snapshot.m_nUserID;
		outPlayer.m_nHandle = snapshot.m_nHandle;
		outPlayer.m_nSignonState = snapshot.m_nSignonState;
		outPlayer.m_nTeam = snapshot.m_nTeam;
		outPlayer.m_nLifeState = snapshot.m_nLifeState;
		outPlayer.m_bConnected = snapshot.m_bConnected;
		outPlayer.m_bBot = snapshot.m_bBot;
		outPlayer.m_svUID = snapshot.m_svUID.empty() ? outPlayer.m_svUID : snapshot.m_svUID;
		outPlayer.m_svPersonaName = snapshot.m_svPersonaName;
		outPlayer.m_svInputDevice = snapshot.m_svInputDevice.empty() ? "unknown" : snapshot.m_svInputDevice;
		if (snapshot.m_bHasMetrics)
			outPlayer.m_Metrics = snapshot.m_Metrics;
	}
}

static MatchReportWeaponStat_t& SV_GetMatchReportWeaponStatForId(const NucleusID_t nNucleusID, const char* const pszWeaponName)
{
	const char* const pszSafeWeaponName = VALID_CHARSTAR(pszWeaponName) ? pszWeaponName : "unknown";
	MatchReportWeaponStat_t& stat = s_PlayerWeaponStats[nNucleusID][pszSafeWeaponName];
	if (stat.m_svWeapon.empty())
		stat.m_svWeapon = pszSafeWeaponName;

	return stat;
}

static void SV_RecordMatchReportWeaponKillForId(const NucleusID_t nNucleusID, const char* const pszWeaponName)
{
	if (nNucleusID == 0 || !VALID_CHARSTAR(pszWeaponName))
		return;

	std::lock_guard<std::recursive_mutex> lock(s_MatchReportStatsMutex);

	s_PlayerWeaponKills[nNucleusID][pszWeaponName]++;
	SV_GetMatchReportWeaponStatForId(nNucleusID, pszWeaponName).m_nKills++;
}

static void SV_RecordMatchReportWeaponShotForId(const NucleusID_t nNucleusID, const char* const pszWeaponName, const int nShots)
{
	if (nNucleusID == 0 || !VALID_CHARSTAR(pszWeaponName) || nShots <= 0)
		return;

	std::lock_guard<std::recursive_mutex> lock(s_MatchReportStatsMutex);

	SV_GetMatchReportWeaponStatForId(nNucleusID, pszWeaponName).m_nShots += nShots;
}

static void SV_RecordMatchReportWeaponHitForId(const NucleusID_t nNucleusID, const char* const pszWeaponName,
	const float flDamage, const int nHits, const float flBulletsHit, const int nHeadshots)
{
	if (nNucleusID == 0 || !VALID_CHARSTAR(pszWeaponName))
		return;

	std::lock_guard<std::recursive_mutex> lock(s_MatchReportStatsMutex);

	MatchReportWeaponStat_t& stat = SV_GetMatchReportWeaponStatForId(nNucleusID, pszWeaponName);
	if (flDamage > 0.0f)
		stat.m_flDamage += flDamage;
	if (nHits > 0)
		stat.m_nHits += nHits;
	if (flBulletsHit > 0.0f)
		stat.m_flBulletsHit += flBulletsHit;
	if (nHeadshots > 0)
		stat.m_nHeadshots += nHeadshots;
}

static bool SV_BuildMatchReport(CServer* const pServer, MatchReport_t& outReport)
{
	if (!pServer || !pServer->IsActive())
		return false;

	std::lock_guard<std::recursive_mutex> lock(s_MatchReportStatsMutex);

	const char* pszMapName = pServer->GetMapName();
	if (!pszMapName)
		pszMapName = "";

	const char* pszPlaylist = v_Playlists_GetCurrent ? v_Playlists_GetCurrent() : "";
	if (!pszPlaylist)
		pszPlaylist = "";

	outReport.m_nTick = pServer->GetTick();
	outReport.m_nSpawnCount = pServer->GetSpawnCount();
	outReport.m_nMaxPlayers = SV_GetRemoteServerMaxPlayers();
	outReport.m_nEndedAt = static_cast<int64_t>(time(nullptr));
	SV_GetRemoteServerName(outReport.m_svServerName);
	SV_GetRemoteServerAddress(outReport.m_svServerIp, outReport.m_nServerPort);
	outReport.m_svMap = pszMapName;
	outReport.m_svPlaylist = pszPlaylist;

	std::unordered_map<NucleusID_t, bool> includedPlayers;
	auto appendReportPlayer = [&](const NucleusID_t nNucleusID)
	{
		if (nNucleusID == 0 || includedPlayers[nNucleusID])
			return;

		MatchReportPlayer_t reportPlayer;
		SV_CopyMatchReportPlayerSnapshot(nNucleusID, reportPlayer);
		SV_CopyMatchReportWeaponKills(nNucleusID, reportPlayer.m_WeaponKills);
		SV_CopyMatchReportWeaponStats(nNucleusID, reportPlayer.m_WeaponStats);

		outReport.m_Players.emplace_back(std::move(reportPlayer));
		includedPlayers[nNucleusID] = true;
	};

	for (int i = 0; i < pServer->GetMaxClients(); i++)
	{
		CClient* const pClient = pServer->GetClient(i);
		if (!pClient || !pClient->IsConnected() || pClient->IsFakeClient())
			continue;

		CPlayer* const pPlayer = UTIL_PlayerByIndex(pClient->GetHandle());
		if (!pPlayer || !pPlayer->IsConnected())
			continue;

		SV_UpdateMatchReportPlayerSnapshot(pPlayer);
		appendReportPlayer(SV_GetMatchReportNucleusID(pPlayer));
	}

	for (const auto& snapshotIt : s_PlayerSnapshots)
		appendReportPlayer(snapshotIt.first);

	for (const auto& playerIt : s_PlayerWeaponKills)
		appendReportPlayer(playerIt.first);

	for (const auto& playerIt : s_PlayerWeaponStats)
		appendReportPlayer(playerIt.first);

	for (const MatchReportKillEvent_t& killEvent : s_KillEvents)
	{
		appendReportPlayer(killEvent.m_nAttackerNucleusID);
		appendReportPlayer(killEvent.m_nVictimNucleusID);
	}

	outReport.m_nNumPlayers = static_cast<int>(outReport.m_Players.size());
	outReport.m_KillEvents = s_KillEvents;
	return true;
}

static void SV_SubmitMatchReport(RemoteApiRequest_t request, MatchReport_t report)
{
	struct MatchReportInFlightGuard_t
	{
		~MatchReportInFlightGuard_t()
		{
			s_bMatchReportInFlight.store(false);
		}
	} inFlightGuard;

	rapidjson::Document requestJson;
	SV_BuildMatchReportRequest(report, requestJson);
	SV_LogMatchReportSummary("submit", report, request.m_bVerbose);

	string svMessage;
	const bool bSuccess = SV_SendMatchReportJson(request, requestJson, svMessage);

	if (!bSuccess)
	{
		Warning(eDLL_T::SERVER, "Match-end report failed: %s\n", svMessage.c_str());
		SV_MatchReportDebugLogEnabled(request.m_bVerbose, "[submit] failed: {}", svMessage);
		return;
	}

	{
		std::lock_guard<std::recursive_mutex> lock(s_MatchReportStatsMutex);
		if (s_PendingMatchReport &&
			s_PendingMatchReport->m_nSpawnCount == report.m_nSpawnCount &&
			s_PendingMatchReport->m_svMap == report.m_svMap)
		{
			s_nLastReportedSpawnCount = report.m_nSpawnCount;
			s_svLastReportedMap = report.m_svMap;
			s_PendingMatchReport.reset();
		}
	}

	size_t nWeaponStatRows = 0;
	for (const MatchReportPlayer_t& player : report.m_Players)
		nWeaponStatRows += player.m_WeaponStats.size();

	Msg(eDLL_T::SERVER, "Match-end report submitted for '%s' with %d player(s), %llu kill event(s), %llu weapon stat row(s)\n",
		report.m_svMap.c_str(), report.m_nNumPlayers,
		static_cast<unsigned long long>(report.m_KillEvents.size()),
		static_cast<unsigned long long>(nWeaponStatRows));
	SV_MatchReportDebugLogEnabled(request.m_bVerbose, "[submit] success weaponStatRows={}", nWeaponStatRows);
}

void SV_ReportMatchEndData(CServer* const pServer)
{
	if (!sv_match_report_enable.GetBool())
	{
		std::lock_guard<std::recursive_mutex> lock(s_MatchReportStatsMutex);
		s_PendingMatchReport.reset();
		SV_ClearMatchReportScriptStats();
		return;
	}

	if (SV_IsRemoteApiShutdownRequested())
		return;

	// Capture all HTTP settings on the main thread; the worker must never read
	// ConVar string storage (see RemoteApiRequest_t).
	RemoteApiRequest_t request;
	string svMessage;
	if (!SV_ResolveRemoteApiUrl(sv_match_report_url.GetString(), MATCH_REPORT_DEFAULT_PATH,
		request.m_svUrl, svMessage, "sv_match_report_url"))
	{
		Warning(eDLL_T::SERVER, "Match-end report skipped: %s\n", svMessage.c_str());
		return;
	}

	SV_CaptureRemoteApiAuth(request);
	request.m_bVerifyPeer = sv_match_report_ssl_verify_peer.GetBool();
	request.m_bVerbose = SV_IsRemoteApiDebugEnabled() || sv_match_report_debug.GetBool();

	if (s_bMatchReportInFlight.load())
	{
		Warning(eDLL_T::SERVER, "Match-end report skipped: previous report is still in flight\n");
		return;
	}

	std::lock_guard<std::recursive_mutex> lock(s_MatchReportStatsMutex);

	MatchReport_t report;
	if (s_PendingMatchReport)
	{
		report = *s_PendingMatchReport;
		SV_MatchReportDebugLogEnabled(request.m_bVerbose,
			"[built] retrying pending spawn/map spawn={} map={}",
			report.m_nSpawnCount, report.m_svMap);
	}
	else
	{
		if (!SV_BuildMatchReport(pServer, report))
			return;
		SV_LogMatchReportSummary("built", report, request.m_bVerbose);

		if (s_nLastReportedSpawnCount == report.m_nSpawnCount && s_svLastReportedMap == report.m_svMap)
		{
			SV_MatchReportDebugLogEnabled(request.m_bVerbose, "[built] skipped duplicate spawn/map spawn={} map={}", report.m_nSpawnCount, report.m_svMap);
			return;
		}

		s_PendingMatchReport = std::make_unique<MatchReport_t>(report);
		SV_ClearMatchReportScriptStats();
	}

	s_bMatchReportInFlight.store(true);

	if (!SV_StartRemoteApiWorker("match-report",
		[request = std::move(request), report = std::move(report)]() mutable
		{
			SV_SubmitMatchReport(std::move(request), std::move(report));
		}))
	{
		s_bMatchReportInFlight.store(false);
	}
}

void SV_RecordMatchReportWeaponKill(CPlayer* const pPlayer, const char* const pszWeaponName)
{
	if (!SV_ShouldCollectMatchReportStats())
		return;

	if (!pPlayer || !pPlayer->IsConnected() || !VALID_CHARSTAR(pszWeaponName))
		return;

	s_nDebugWeaponKillRecords.fetch_add(1);
	SV_UpdateMatchReportPlayerSnapshot(pPlayer);
	const NucleusID_t nNucleusID = SV_GetMatchReportNucleusID(pPlayer);
	SV_RecordMatchReportWeaponKillForId(nNucleusID, pszWeaponName);

	CClient* const pClient = SV_GetMatchReportClient(pPlayer);
	const NucleusID_t nClientNucleusID = pClient ? pClient->GetNucleusID() : 0;
	if (nClientNucleusID != nNucleusID)
		SV_RecordMatchReportWeaponKillForId(nClientNucleusID, pszWeaponName);
}

void SV_RecordMatchReportWeaponKillByUid(const char* const pszPlayerUid, const char* const pszPlayerName,
	const char* const pszWeaponName)
{
	if (!SV_ShouldCollectMatchReportStats())
		return;

	if (!VALID_CHARSTAR(pszPlayerUid) || !VALID_CHARSTAR(pszWeaponName))
		return;

	const NucleusID_t nNucleusID = SV_ParseMatchReportNucleusID(pszPlayerUid);
	if (nNucleusID == 0)
		return;

	s_nDebugWeaponKillRecords.fetch_add(1);
	SV_UpdateMatchReportPlayerSnapshotForId(nNucleusID, pszPlayerName);
	SV_RecordMatchReportWeaponKillForId(nNucleusID, pszWeaponName);
}

void SV_RecordMatchReportPlayerKill(CPlayer* const pAttacker, const char* const pszVictimUid, const char* const pszVictimName,
	const char* const pszWeaponName, const int nDamageSourceId)
{
	if (!SV_ShouldCollectMatchReportStats())
		return;

	if (!pAttacker || !pAttacker->IsConnected() || !VALID_CHARSTAR(pszWeaponName) || !VALID_CHARSTAR(pszVictimUid))
		return;

	s_nDebugPlayerKillRecords.fetch_add(1);
	SV_UpdateMatchReportPlayerSnapshot(pAttacker);
	const NucleusID_t nAttackerNucleusID = SV_GetMatchReportNucleusID(pAttacker);
	if (nAttackerNucleusID == 0)
		return;

	const int nTick = g_pServer ? g_pServer->GetTick() : 0;
	const NucleusID_t nVictimNucleusID = SV_ParseMatchReportNucleusID(pszVictimUid);
	if (SV_IsDuplicateMatchReportKillEvent(nTick, nAttackerNucleusID, nVictimNucleusID,
		pszVictimUid, pszWeaponName, nDamageSourceId))
		return;

	SV_RecordMatchReportWeaponKill(pAttacker, pszWeaponName);

	MatchReportKillEvent_t event;
	event.m_nTick = nTick;
	event.m_nRecordedAt = static_cast<int64_t>(time(nullptr));
	event.m_nDamageSourceId = nDamageSourceId;
	event.m_nAttackerNucleusID = nAttackerNucleusID;
	event.m_nVictimNucleusID = nVictimNucleusID;
	event.m_svAttackerUid = Format("%llu", nAttackerNucleusID);
	event.m_svAttackerName = SV_GetMatchReportPlayerName(pAttacker);
	event.m_svVictimUid = pszVictimUid;
	event.m_svVictimName = VALID_CHARSTAR(pszVictimName) ? pszVictimName : "";
	event.m_svWeapon = pszWeaponName;

	SV_UpdateMatchReportPlayerSnapshotForId(event.m_nVictimNucleusID, event.m_svVictimName.c_str());
	std::lock_guard<std::recursive_mutex> lock(s_MatchReportStatsMutex);
	s_KillEvents.emplace_back(std::move(event));
}

void SV_RecordMatchReportPlayerKillByUid(const char* const pszAttackerUid, const char* const pszAttackerName,
	const char* const pszVictimUid, const char* const pszVictimName, const char* const pszWeaponName,
	const int nDamageSourceId)
{
	if (!SV_ShouldCollectMatchReportStats())
		return;

	if (!VALID_CHARSTAR(pszAttackerUid) || !VALID_CHARSTAR(pszWeaponName))
		return;

	const NucleusID_t nAttackerNucleusID = SV_ParseMatchReportNucleusID(pszAttackerUid);
	if (nAttackerNucleusID == 0)
		return;

	if (!VALID_CHARSTAR(pszVictimUid))
	{
		SV_RecordMatchReportWeaponKillByUid(pszAttackerUid, pszAttackerName, pszWeaponName);
		return;
	}

	s_nDebugPlayerKillRecords.fetch_add(1);
	SV_UpdateMatchReportPlayerSnapshotForId(nAttackerNucleusID, pszAttackerName);

	const int nTick = g_pServer ? g_pServer->GetTick() : 0;
	const NucleusID_t nVictimNucleusID = SV_ParseMatchReportNucleusID(pszVictimUid);
	if (SV_IsDuplicateMatchReportKillEvent(nTick, nAttackerNucleusID, nVictimNucleusID,
		pszVictimUid, pszWeaponName, nDamageSourceId))
		return;

	SV_RecordMatchReportWeaponKillByUid(pszAttackerUid, pszAttackerName, pszWeaponName);

	MatchReportKillEvent_t event;
	event.m_nTick = nTick;
	event.m_nRecordedAt = static_cast<int64_t>(time(nullptr));
	event.m_nDamageSourceId = nDamageSourceId;
	event.m_nAttackerNucleusID = nAttackerNucleusID;
	event.m_nVictimNucleusID = nVictimNucleusID;
	event.m_svAttackerUid = pszAttackerUid;
	event.m_svAttackerName = VALID_CHARSTAR(pszAttackerName) ? pszAttackerName : "";
	event.m_svVictimUid = pszVictimUid;
	event.m_svVictimName = VALID_CHARSTAR(pszVictimName) ? pszVictimName : "";
	event.m_svWeapon = pszWeaponName;

	SV_UpdateMatchReportPlayerSnapshotForId(event.m_nVictimNucleusID, event.m_svVictimName.c_str());
	std::lock_guard<std::recursive_mutex> lock(s_MatchReportStatsMutex);
	s_KillEvents.emplace_back(std::move(event));
}

void SV_RecordMatchReportWeaponShot(CPlayer* const pPlayer, const char* const pszWeaponName, const int nShots)
{
	if (!SV_ShouldCollectMatchReportStats())
		return;

	if (!pPlayer || !pPlayer->IsConnected() || !VALID_CHARSTAR(pszWeaponName) || nShots <= 0)
		return;

	s_nDebugWeaponShotRecords.fetch_add(1);
	SV_UpdateMatchReportPlayerSnapshot(pPlayer);
	const NucleusID_t nNucleusID = SV_GetMatchReportNucleusID(pPlayer);
	SV_RecordMatchReportWeaponShotForId(nNucleusID, pszWeaponName, nShots);

	CClient* const pClient = SV_GetMatchReportClient(pPlayer);
	const NucleusID_t nClientNucleusID = pClient ? pClient->GetNucleusID() : 0;
	if (nClientNucleusID != nNucleusID)
		SV_RecordMatchReportWeaponShotForId(nClientNucleusID, pszWeaponName, nShots);
}

void SV_RecordMatchReportWeaponShotByUid(const char* const pszPlayerUid, const char* const pszPlayerName,
	const char* const pszWeaponName, const int nShots)
{
	if (!SV_ShouldCollectMatchReportStats())
		return;

	if (!VALID_CHARSTAR(pszPlayerUid) || !VALID_CHARSTAR(pszWeaponName) || nShots <= 0)
		return;

	const NucleusID_t nNucleusID = SV_ParseMatchReportNucleusID(pszPlayerUid);
	if (nNucleusID == 0)
		return;

	s_nDebugWeaponShotRecords.fetch_add(1);
	SV_UpdateMatchReportPlayerSnapshotForId(nNucleusID, pszPlayerName);
	SV_RecordMatchReportWeaponShotForId(nNucleusID, pszWeaponName, nShots);
}

void SV_RecordMatchReportWeaponHit(CPlayer* const pPlayer, const char* const pszWeaponName, const float flDamage,
	const int nHits, const float flBulletsHit, const int nHeadshots)
{
	if (!SV_ShouldCollectMatchReportStats())
		return;

	if (!pPlayer || !pPlayer->IsConnected() || !VALID_CHARSTAR(pszWeaponName))
		return;

	s_nDebugWeaponHitRecords.fetch_add(1);
	SV_UpdateMatchReportPlayerSnapshot(pPlayer);
	const NucleusID_t nNucleusID = SV_GetMatchReportNucleusID(pPlayer);
	SV_RecordMatchReportWeaponHitForId(nNucleusID, pszWeaponName, flDamage, nHits, flBulletsHit, nHeadshots);

	CClient* const pClient = SV_GetMatchReportClient(pPlayer);
	const NucleusID_t nClientNucleusID = pClient ? pClient->GetNucleusID() : 0;
	if (nClientNucleusID != nNucleusID)
		SV_RecordMatchReportWeaponHitForId(nClientNucleusID, pszWeaponName, flDamage, nHits, flBulletsHit, nHeadshots);
}

void SV_RecordMatchReportWeaponHitByUid(const char* const pszPlayerUid, const char* const pszPlayerName,
	const char* const pszWeaponName, const float flDamage, const int nHits, const float flBulletsHit,
	const int nHeadshots)
{
	if (!SV_ShouldCollectMatchReportStats())
		return;

	if (!VALID_CHARSTAR(pszPlayerUid) || !VALID_CHARSTAR(pszWeaponName))
		return;

	const NucleusID_t nNucleusID = SV_ParseMatchReportNucleusID(pszPlayerUid);
	if (nNucleusID == 0)
		return;

	s_nDebugWeaponHitRecords.fetch_add(1);
	SV_UpdateMatchReportPlayerSnapshotForId(nNucleusID, pszPlayerName);
	SV_RecordMatchReportWeaponHitForId(nNucleusID, pszWeaponName, flDamage, nHits, flBulletsHit, nHeadshots);
}
