//=============================================================================//
//
// Purpose: Remote player access policy checks for dedicated servers.
//
//=============================================================================//

#include "core/stdafx.h"
#include <atomic>
#include <cstdint>
#include "core/logger.h"
#include "tier0/frametask.h"
#include "tier1/cvar.h"
#include "tier1/strtools.h"
#include "tier2/jsonutils.h"
#include "engine/client/client.h"
#include "engine/net.h"
#include "engine/server/server.h"
#include "game/server/player.h"
#include "game/server/util_server.h"
#include "networksystem/clientdisconnect.h"
#include "networksystem/disconnectmessage.h"
#include "networksystem/matchreport.h"
#include "networksystem/playeraccess.h"
#include "networksystem/remoteapi.h"

//-----------------------------------------------------------------------------
// Console variables
//-----------------------------------------------------------------------------
ConVar sv_player_access_enable("sv_player_access_enable", "0", FCVAR_RELEASE | FCVAR_ACCESSIBLE_FROM_THREADS, "Enables the remote player access policy check.", false, 0.f, true, 1.f, "0 = Disable, 1 = Enable.");
static ConVar sv_player_access_url("sv_player_access_url", "", FCVAR_RELEASE | FCVAR_ACCESSIBLE_FROM_THREADS, "Full HTTP(S) URL for the remote player access policy endpoint.");
static ConVar sv_player_access_fail_open("sv_player_access_fail_open", "1", FCVAR_RELEASE | FCVAR_ACCESSIBLE_FROM_THREADS, "Allows players when the remote player access policy endpoint fails.", false, 0.f, true, 1.f, "0 = Kick on check failure, 1 = Allow on check failure.");
static ConVar sv_player_access_default_reason("sv_player_access_default_reason", "Access denied by server policy", FCVAR_RELEASE | FCVAR_ACCESSIBLE_FROM_THREADS, "Fallback kick reason for the remote player access policy.");
static ConVar sv_player_access_ssl_verify_peer("sv_player_access_ssl_verify_peer", "1", FCVAR_RELEASE | FCVAR_ACCESSIBLE_FROM_THREADS, "Verifies HTTPS peer certificates for the remote player access policy endpoint.", false, 0.f, true, 1.f, "0 = Disable, 1 = Enable.");
static ConVar sv_player_access_debug("sv_player_access_debug", "0", FCVAR_RELEASE | FCVAR_ACCESSIBLE_FROM_THREADS, "Enables verbose curl logging and response logging for remote player access endpoints.", false, 0.f, true, 1.f, "0 = Disable, 1 = Enable.");
static ConVar sv_player_access_report_enable("sv_player_access_report_enable", "0", FCVAR_RELEASE | FCVAR_ACCESSIBLE_FROM_THREADS, "Enables periodic online player reporting to the remote player access endpoint.", false, 0.f, true, 1.f, "0 = Disable, 1 = Enable.");
static ConVar sv_player_access_report_url("sv_player_access_report_url", "", FCVAR_RELEASE | FCVAR_ACCESSIBLE_FROM_THREADS, "Full HTTP(S) URL for periodic online player reports.");
static ConVar sv_player_access_report_interval("sv_player_access_report_interval", "30", FCVAR_RELEASE | FCVAR_ACCESSIBLE_FROM_THREADS, "Online player report interval in seconds.", true, 5.f, false, 0.f, "seconds");

struct PlayerAccessDecision_t
{
	bool m_bAllow = true;
	string m_svReason;
	string m_svReasonEn;
	string m_svReasonCode;
	string m_svAction;
	string m_svIPAddr;
	string m_svProcessedAt;
	string m_svRuleId;
};

enum class PlayerAccessClientReasonSource_e
{
	LOCALIZED = 0,
	ENGLISH,
	GENERIC
};

struct PlayerAccessClientReason_t
{
	string m_svText;
	PlayerAccessClientReasonSource_e m_eSource = PlayerAccessClientReasonSource_e::GENERIC;
};

struct PlayerAccessOnlinePlayer_t
{
	int m_nUserID = 0;
	int m_nHandle = 0;
	int m_nSignonState = 0;
	int m_nPing = 0;
	int m_nPort = 0;
	NucleusID_t m_nNucleusID = 0;
	string m_svUID;
	string m_svPersonaName;
	string m_svIPAddr;
	string m_svInputDevice = "unknown";
};

struct PlayerAccessOnlineReport_t
{
	int m_nTick = 0;
	int m_nNumPlayers = 0;
	int m_nMaxPlayers = 0;
	string m_svServerName;
	string m_svServerIp;
	int m_nServerPort = 0;
	string m_svMap;
	vector<PlayerAccessOnlinePlayer_t> m_Players;
};

enum class PlayerAccessOnlineActionType_e
{
	KICK = 0,
	BAN
};

static const char* SV_GetOnlineActionName(const PlayerAccessOnlineActionType_e eType);

struct PlayerAccessOnlineAction_t
{
	PlayerAccessOnlineActionType_e m_eType = PlayerAccessOnlineActionType_e::KICK;
	NucleusID_t m_nNucleusID = 0;
	string m_svReason;
	string m_svReasonEn;
	string m_svReasonCode;
	string m_svIPAddr;
	string m_svProcessedAt;
	string m_svRuleId;
};

struct PlayerAccessOnlineReportResult_t
{
	bool m_bSuccess = false;
	vector<PlayerAccessOnlineAction_t> m_Actions;
	vector<string> m_Warnings;
	string m_svMessage;
	string m_svDebugMessage;
};

static std::atomic_bool s_bOnlineReportInFlight(false);
static double s_flNextOnlineReportTime = 0.0;
static double s_flNextOnlineReportHeartbeatTime = 0.0;
static bool s_bOnlineReportDisabledLogged = false;

static constexpr const char* PLAYER_ACCESS_CHECK_DEFAULT_PATH = "/api/r5/access/check";
static constexpr const char* PLAYER_ACCESS_ONLINE_DEFAULT_PATH = "/api/r5/access/online";
static constexpr const char* PLAYER_ACCESS_GENERIC_REASON = "Disconnected-from-server";
static constexpr double PLAYER_ACCESS_REPORT_HEARTBEAT_INTERVAL = 10.0;
static constexpr unsigned int PLAYER_ACCESS_DISCONNECT_RETRY_FRAMES = 1;
static constexpr unsigned int PLAYER_ACCESS_DISCONNECT_KEY_HOLD_FRAMES = 32;
// Online-report actions normally target fully connected players. If a matching
// client has fallen out of active signon, do not leave a half-open netchannel.
static constexpr unsigned int PLAYER_ACCESS_DISCONNECT_SIGNON_GRACE_FRAMES = 240;

static const char* SV_GetPlayerAccessClientReasonSourceName(const PlayerAccessClientReasonSource_e eSource)
{
	switch (eSource)
	{
	case PlayerAccessClientReasonSource_e::LOCALIZED:
		return "localized";
	case PlayerAccessClientReasonSource_e::ENGLISH:
		return "english";
	case PlayerAccessClientReasonSource_e::GENERIC:
	default:
		return "generic";
	}
}

static string SV_FormatPlayerAccessRejectTimestamp()
{
	SYSTEMTIME time;
	GetLocalTime(&time);

	char szTimestamp[64];
	V_snprintf(szTimestamp, sizeof(szTimestamp), "%04hu-%02hu-%02hu %02hu:%02hu:%02hu.%03hu",
		time.wYear, time.wMonth, time.wDay,
		time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);

	return szTimestamp;
}

static string SV_EscapePlayerAccessRejectLogValue(const string& svValue)
{
	string svEscaped;
	svEscaped.reserve(svValue.length());

	for (const unsigned char ch : svValue)
	{
		switch (ch)
		{
		case '\\':
			svEscaped.append("\\\\");
			break;
		case '\'':
			svEscaped.append("\\'");
			break;
		case '\n':
			svEscaped.append("\\n");
			break;
		case '\r':
			svEscaped.append("\\r");
			break;
		case '\t':
			svEscaped.append("\\t");
			break;
		default:
			if (ch < 0x20 || ch == 0x7f)
				svEscaped.append(Format("\\x%02X", ch));
			else
				svEscaped.push_back(static_cast<char>(ch));
			break;
		}
	}

	return svEscaped;
}

static void SV_WritePlayerAccessRejectLog(const PlayerAccessDecision_t& decision,
	const PlayerAccessClientReason_t& clientReason,
	const string& svIPAddr, const NucleusID_t nNucleusID,
	const string& svPersonaName, const int nPort)
{
	const string svLine = Format(
		"[%s] uid=%llu name='%s' addr='%s:%i' rule='%s' fallback=%s reason_source=%s reason='%s' reason_en='%s' client_reason='%s'\n",
		SV_FormatPlayerAccessRejectTimestamp().c_str(),
		static_cast<unsigned long long>(nNucleusID),
		SV_EscapePlayerAccessRejectLogValue(svPersonaName).c_str(),
		SV_EscapePlayerAccessRejectLogValue(svIPAddr).c_str(),
		nPort,
		SV_EscapePlayerAccessRejectLogValue(decision.m_svRuleId).c_str(),
		clientReason.m_eSource == PlayerAccessClientReasonSource_e::LOCALIZED ? "false" : "true",
		SV_GetPlayerAccessClientReasonSourceName(clientReason.m_eSource),
		SV_EscapePlayerAccessRejectLogValue(decision.m_svReason).c_str(),
		SV_EscapePlayerAccessRejectLogValue(decision.m_svReasonEn).c_str(),
		SV_EscapePlayerAccessRejectLogValue(clientReason.m_svText).c_str());

	SDK_AppendDiagnosticLogFile("player_access_reject.log", svLine.c_str());
}

static void SV_LogPlayerAccessDiagnostic(const char* const pszSource, const char* const pszDetail)
{
	if (!SV_IsRemoteApiDebugEnabled())
		return;

	Msg(eDLL_T::SERVER, "Player access diagnostic: %s%s%s\n",
		VALID_CHARSTAR(pszSource) ? pszSource : "unknown",
		VALID_CHARSTAR(pszDetail) ? " " : "",
		VALID_CHARSTAR(pszDetail) ? pszDetail : "");
	SDK_WriteRuntimeBreadcrumb(pszSource, pszDetail);
}

static string SV_FormatOnlineReportBreadcrumb(const PlayerAccessOnlineReport_t& report, const char* const pszExtra = nullptr)
{
	string svDetail = Format("map='%s' tick=%d players=%d/%d server='%s' addr='%s:%d'",
		report.m_svMap.c_str(), report.m_nTick, report.m_nNumPlayers, report.m_nMaxPlayers,
		report.m_svServerName.c_str(), report.m_svServerIp.c_str(), report.m_nServerPort);

	if (VALID_CHARSTAR(pszExtra))
		svDetail.append(Format(" %s", pszExtra));

	return svDetail;
}

static void SV_WriteOnlineReportBreadcrumb(const char* const pszSource,
	const PlayerAccessOnlineReport_t& report, const char* const pszExtra = nullptr)
{
	const string svDetail = SV_FormatOnlineReportBreadcrumb(report, pszExtra);
	SV_LogPlayerAccessDiagnostic(pszSource, svDetail.c_str());
}

static CClientDisconnectTracker s_PlayerAccessDisconnects;

static const char* SV_GetPlayerAccessEndpointMode(const char* const pszConfiguredUrl)
{
	if (VALID_CHARSTAR(pszConfiguredUrl))
		return "custom";

	return VALID_CHARSTAR(sv_r5_api_base_url.GetString()) ? "base" : "missing";
}

static void SV_AppendPlayerAccessConfigPart(string& outSummary, const string& svPart)
{
	if (!outSummary.empty())
		outSummary.append("; ");

	outSummary.append(svPart);
}

void SV_AppendPlayerAccessConfigSummary(string& outSummary)
{
	if (sv_player_access_enable.GetBool())
	{
		SV_AppendPlayerAccessConfigPart(outSummary, Format(
			"player_access(url=%s fail_open=%d ssl=%d debug=%d reason_mode=safe_utf8)",
			SV_GetPlayerAccessEndpointMode(sv_player_access_url.GetString()),
			sv_player_access_fail_open.GetBool() ? 1 : 0,
			sv_player_access_ssl_verify_peer.GetBool() ? 1 : 0,
			sv_player_access_debug.GetBool() ? 1 : 0));
	}

	if (sv_player_access_report_enable.GetBool())
	{
		SV_AppendPlayerAccessConfigPart(outSummary, Format(
			"online_report(url=%s interval=%.0fs ssl=%d debug=%d)",
			SV_GetPlayerAccessEndpointMode(sv_player_access_report_url.GetString()),
			sv_player_access_report_interval.GetFloat(),
			sv_player_access_ssl_verify_peer.GetBool() ? 1 : 0,
			sv_player_access_debug.GetBool() ? 1 : 0));
	}
}

static const char* SV_GetPlayerInputDeviceName(CPlayer* const pPlayer)
{
	if (!pPlayer || !pPlayer->IsConnected() || pPlayer->IsBot())
		return "unknown";

	return pPlayer->IsControllerModeActive() ? "controller" : "keyboard_mouse";
}

static const char* SV_GetPlayerAccessDefaultReason()
{
	const char* const pszReason = sv_player_access_default_reason.GetString();
	return VALID_CHARSTAR(pszReason) ? pszReason : "Access denied by server policy";
}

static bool SV_SendPlayerAccessJsonRequest(const RemoteApiRequest_t& request,
	const rapidjson::Document& requestJson, rapidjson::Document& responseJson,
	string& outMessage, string& outDebugMessage, const char* const pszContext)
{
	RemoteApiResponse_t response;
	const bool bSuccess = SV_SendRemoteApiJsonRequest(request, requestJson, response,
		outMessage, pszContext, true);
	outDebugMessage = response.m_svDebugMessage;

	if (!bSuccess)
		return false;

	responseJson.Swap(response.m_Json);
	return true;
}

static bool SV_QueryPlayerAccess(const PlayerAccessCheckRequest_t& config,
	const string& svIPAddr, const NucleusID_t nNucleusID,
	const string& svPersonaName, const int nPort, const string& svServerIp,
	const int nServerPort, PlayerAccessDecision_t& outDecision, string& outMessage,
	string& outDebugMessage)
{
	rapidjson::Document requestJson;
	requestJson.SetObject();

	rapidjson::Document::AllocatorType& allocator = requestJson.GetAllocator();
	const string svNucleusID = Format("%llu", nNucleusID);

	requestJson.AddMember("uid", rapidjson::Value(svNucleusID.c_str(), svNucleusID.length(), allocator), allocator);
	requestJson.AddMember("nucleusId", static_cast<uint64_t>(nNucleusID), allocator);
	requestJson.AddMember("playerName", rapidjson::Value(svPersonaName.c_str(), svPersonaName.length(), allocator), allocator);
	requestJson.AddMember("ip", rapidjson::Value(svIPAddr.c_str(), svIPAddr.length(), allocator), allocator);
	requestJson.AddMember("port", nPort, allocator);
	SV_AddRemoteServerName(requestJson, allocator, config.m_svServerName);
	SV_AddRemoteServerAddress(requestJson, allocator, svServerIp, nServerPort);

	rapidjson::Document responseJson;
	if (!SV_SendPlayerAccessJsonRequest(config.m_Http, requestJson, responseJson,
		outMessage, outDebugMessage, "player access"))
		return false;

	if (!JSON_GetValue(responseJson, "allow", outDecision.m_bAllow))
	{
		outMessage = "player access response is missing boolean field 'allow'";
		return false;
	}

	JSON_GetValue(responseJson, "reason", outDecision.m_svReason);
	JSON_GetValue(responseJson, "reasonEn", outDecision.m_svReasonEn);
	JSON_GetValue(responseJson, "reasonCode", outDecision.m_svReasonCode);
	JSON_GetValue(responseJson, "action", outDecision.m_svAction);
	JSON_GetValue(responseJson, "ip", outDecision.m_svIPAddr);
	JSON_GetValue(responseJson, "processedAt", outDecision.m_svProcessedAt);
	JSON_GetValue(responseJson, "ruleId", outDecision.m_svRuleId);

	return true;
}

static bool SV_BeginPlayerAccessDisconnect(const ClientDisconnectKey_t& key)
{
	return s_PlayerAccessDisconnects.Begin(key);
}

static void SV_EndPlayerAccessDisconnect(const ClientDisconnectKey_t& key)
{
	s_PlayerAccessDisconnects.End(key);
}

static void SV_EndPlayerAccessDisconnectDelayed(const ClientDisconnectKey_t& key)
{
	g_TaskQueue.Dispatch([key]
		{
			SV_EndPlayerAccessDisconnect(key);
		}, PLAYER_ACCESS_DISCONNECT_KEY_HOLD_FRAMES);
}

static PlayerAccessClientReason_t SV_BuildPlayerAccessClientReason(
	const string& svBackendReason, const string& svBackendReasonEn)
{
	string svSafeReason;
	if (DisconnectMessage_BuildSafeUtf8Reason(svBackendReason, svSafeReason))
		return { std::move(svSafeReason), PlayerAccessClientReasonSource_e::LOCALIZED };

	if (DisconnectMessage_BuildSafeUtf8Reason(svBackendReasonEn, svSafeReason))
		return { std::move(svSafeReason), PlayerAccessClientReasonSource_e::ENGLISH };

	return { PLAYER_ACCESS_GENERIC_REASON, PlayerAccessClientReasonSource_e::GENERIC };
}

static PlayerAccessClientReason_t SV_GetPlayerAccessClientReason(const PlayerAccessDecision_t& decision)
{
	return SV_BuildPlayerAccessClientReason(decision.m_svReason, decision.m_svReasonEn);
}

static void SV_DisconnectForPlayerAccess(const ClientDisconnectKey_t& key,
	const string& svReason, const string& svIPAddr, const int nPort,
	const string& svRuleId, const bool bOnlineReportAction = false,
	const PlayerAccessOnlineActionType_e eOnlineAction = PlayerAccessOnlineActionType_e::KICK,
	const unsigned int nAttempt = 0)
{
	const char* const pszSource = bOnlineReportAction ? "remote online player report" : "remote player access";

	if (SV_IsRemoteApiShutdownRequested())
	{
		SV_EndPlayerAccessDisconnect(key);
		return;
	}

	CClient* const pClient = SV_ResolveClientDisconnectClient(key);
	if (!pClient)
	{
		Warning(eDLL_T::SERVER, "Skipped %s disconnect for slot #%i handle=%i uid='%llu' because the client slot changed or client is gone\n",
			pszSource, key.m_nUserID, key.m_nHandle, key.m_nNucleusID);
		SV_EndPlayerAccessDisconnect(key);
		return;
	}

	CNetChan* const pChan = pClient->GetNetChan();
	if (!pChan)
	{
		Warning(eDLL_T::SERVER, "Skipped %s disconnect for '%llu' because the netchannel is gone\n",
			pszSource, key.m_nNucleusID);
		SV_EndPlayerAccessDisconnect(key);
		return;
	}

	const int nUserID = pClient->GetUserID();
	const int nHandle = pClient->GetHandle();
	const int nSignonState = static_cast<int>(pClient->GetSignonState());
	const string svRuleSuffix = svRuleId.empty() ? "" : Format(" rule='%s'", svRuleId.c_str());

	if (!pClient->IsActive())
	{
		if (nAttempt >= PLAYER_ACCESS_DISCONNECT_SIGNON_GRACE_FRAMES)
		{
			Warning(eDLL_T::SERVER, "Forcing %s half-open cleanup for '[%s]:%i' from slot #%i handle=%i signon=%i uid='%llu'%s reason='%s' because signon did not complete\n",
				pszSource, svIPAddr.c_str(), nPort, nUserID, nHandle, nSignonState, key.m_nNucleusID,
				svRuleSuffix.c_str(), svReason.c_str());
			SV_DisconnectClientNow(pClient, svReason.c_str());
			SV_EndPlayerAccessDisconnectDelayed(key);
			return;
		}

		if (nAttempt == 0)
		{
			Warning(eDLL_T::SERVER, "Delaying %s disconnect for '[%s]:%i' from slot #%i handle=%i signon=%i uid='%llu'%s until signon is complete\n",
				pszSource, svIPAddr.c_str(), nPort, nUserID, nHandle, nSignonState, key.m_nNucleusID,
				svRuleSuffix.c_str());
		}

		g_TaskQueue.Dispatch([key, svReason, svIPAddr, nPort, svRuleId, bOnlineReportAction, eOnlineAction, nAttempt]
			{
				SV_DisconnectForPlayerAccess(key, svReason, svIPAddr, nPort,
					svRuleId, bOnlineReportAction, eOnlineAction, nAttempt + 1);
			}, PLAYER_ACCESS_DISCONNECT_RETRY_FRAMES);
		return;
	}

	if (bOnlineReportAction)
	{
		Warning(eDLL_T::SERVER, "Remote online player report disconnecting '[%s]:%i' from slot #%i handle=%i signon=%i uid='%llu' action=%s%s reason='%s'\n",
			svIPAddr.c_str(), nPort, nUserID, nHandle, nSignonState, key.m_nNucleusID,
			SV_GetOnlineActionName(eOnlineAction), svRuleSuffix.c_str(), svReason.c_str());
	}
	else
	{
		Warning(eDLL_T::SERVER, "Remote player access disconnecting '[%s]:%i' from slot #%i handle=%i signon=%i uid='%llu'%s reason='%s'\n",
			svIPAddr.c_str(), nPort, nUserID, nHandle, nSignonState, key.m_nNucleusID,
			svRuleSuffix.c_str(), svReason.c_str());
	}

	SV_DisconnectClientNow(pClient, svReason.c_str());

	if (bOnlineReportAction)
	{
		if (svRuleId.empty())
		{
			Warning(eDLL_T::SERVER, "Remote online player report applied %s to '[%s]:%i' from slot #%i ('%llu': %s)\n",
				SV_GetOnlineActionName(eOnlineAction), svIPAddr.c_str(), nPort, nUserID, key.m_nNucleusID, svReason.c_str());
		}
		else
		{
			Warning(eDLL_T::SERVER, "Remote online player report applied %s to '[%s]:%i' from slot #%i ('%llu' by policy '%s': %s)\n",
				SV_GetOnlineActionName(eOnlineAction), svIPAddr.c_str(), nPort, nUserID,
				key.m_nNucleusID, svRuleId.c_str(), svReason.c_str());
		}
	}
	else if (svRuleId.empty())
	{
		Warning(eDLL_T::SERVER, "Removed client '[%s]:%i' from slot #%i ('%llu' denied by remote player access policy: %s)\n",
			svIPAddr.c_str(), nPort, nUserID, key.m_nNucleusID, svReason.c_str());
	}
	else
	{
		Warning(eDLL_T::SERVER, "Removed client '[%s]:%i' from slot #%i ('%llu' denied by remote player access policy '%s': %s)\n",
			svIPAddr.c_str(), nPort, nUserID, key.m_nNucleusID, svRuleId.c_str(), svReason.c_str());
	}

	SV_EndPlayerAccessDisconnectDelayed(key);
}

static void SV_QueuePlayerAccessDisconnect(const ClientDisconnectKey_t& key,
	const string& svReason, const string& svIPAddr, const int nPort,
	const string& svRuleId,
	const bool bOnlineReportAction = false,
	const PlayerAccessOnlineActionType_e eOnlineAction = PlayerAccessOnlineActionType_e::KICK)
{
	if (!SV_IsValidClientDisconnectKey(key))
	{
		Warning(eDLL_T::SERVER, "Skipped %s disconnect for '%llu' because the captured client identity is invalid\n",
			bOnlineReportAction ? "remote online player report" : "remote player access", key.m_nNucleusID);
		return;
	}

	if (!SV_BeginPlayerAccessDisconnect(key))
	{
		Warning(eDLL_T::SERVER, "Skipped duplicate %s disconnect for '%llu'\n",
			bOnlineReportAction ? "remote online player report" : "remote player access", key.m_nNucleusID);
		return;
	}

	SV_DisconnectForPlayerAccess(key, svReason, svIPAddr, nPort,
		svRuleId, bOnlineReportAction, eOnlineAction);
}

static void SV_QueuePlayerAccessDisconnect(CClient* const pClient,
	const string& svReason, const string& svIPAddr, const int nPort,
	const NucleusID_t nNucleusID, const string& svRuleId,
	const bool bOnlineReportAction = false,
	const PlayerAccessOnlineActionType_e eOnlineAction = PlayerAccessOnlineActionType_e::KICK)
{
	SV_QueuePlayerAccessDisconnect(SV_MakeClientDisconnectKey(pClient, nNucleusID),
		svReason, svIPAddr, nPort, svRuleId, bOnlineReportAction, eOnlineAction);
}

bool SV_CapturePlayerAccessCheckRequest(PlayerAccessCheckRequest_t& outRequest)
{
	outRequest.m_bValid = false;

	string svMessage;
	if (!SV_ResolveRemoteApiUrl(sv_player_access_url.GetString(), PLAYER_ACCESS_CHECK_DEFAULT_PATH,
		outRequest.m_Http.m_svUrl, svMessage, "sv_player_access_url"))
	{
		Warning(eDLL_T::SERVER, "Player access check skipped: %s\n", svMessage.c_str());
		return false;
	}

	SV_CaptureRemoteApiAuth(outRequest.m_Http);
	outRequest.m_Http.m_bVerifyPeer = sv_player_access_ssl_verify_peer.GetBool();
	outRequest.m_Http.m_bVerbose = SV_IsRemoteApiDebugEnabled() || sv_player_access_debug.GetBool();

	SV_GetRemoteServerName(outRequest.m_svServerName);
	outRequest.m_bFailOpen = sv_player_access_fail_open.GetBool();
	outRequest.m_svDefaultReason = SV_GetPlayerAccessDefaultReason();
	outRequest.m_bValid = true;
	return true;
}

static void SV_LogPlayerAccessDecision(const PlayerAccessDecision_t& decision,
	const PlayerAccessClientReason_t* pClientReason,
	const string& svIPAddr, const NucleusID_t nNucleusID,
	const string& svPersonaName, const int nPort);

static void SV_FinishPlayerAccessCheck(const int nClientUserID, const int nClientHandle,
	const string& svIPAddr, const NucleusID_t nNucleusID, const string& svPersonaName,
	const int nPort, const bool bFailOpen, const string& svDefaultReason,
	const bool bSuccess, PlayerAccessDecision_t decision, const string& svMessage,
	const string& svDebugMessage)
{
	if (SV_IsRemoteApiShutdownRequested())
		return;

	if (!svDebugMessage.empty())
		Msg(eDLL_T::SERVER, "%s\n", svDebugMessage.c_str());

	if (!bSuccess)
	{
		if (bFailOpen)
		{
			SV_TouchMatchReportPlayerIdentity(nNucleusID, svPersonaName.c_str());
			Warning(eDLL_T::SERVER, "Player access check failed for '[%s]:%i' ('%llu'); allowing due to fail-open: %s\n",
				svIPAddr.c_str(), nPort, nNucleusID, svMessage.c_str());
			return;
		}

		decision.m_bAllow = false;
		decision.m_svReason = svDefaultReason;

		Warning(eDLL_T::SERVER, "Player access check failed for '[%s]:%i' ('%llu'); denying due to fail-closed: %s\n",
			svIPAddr.c_str(), nPort, nNucleusID, svMessage.c_str());
	}

	if (decision.m_bAllow)
	{
		SV_LogPlayerAccessDecision(decision, nullptr, svIPAddr, nNucleusID, svPersonaName, nPort);
		SV_TouchMatchReportPlayerIdentity(nNucleusID, svPersonaName.c_str());
		return;
	}

	if (decision.m_svReason.empty())
		decision.m_svReason = svDefaultReason;

	const PlayerAccessClientReason_t clientReason = SV_GetPlayerAccessClientReason(decision);
	SV_LogPlayerAccessDecision(decision, &clientReason, svIPAddr, nNucleusID, svPersonaName, nPort);
	SV_QueuePlayerAccessDisconnect({ nClientUserID, nClientHandle, nNucleusID },
		clientReason.m_svText, svIPAddr, nPort, decision.m_svRuleId);
}

void SV_CheckPlayerAccessAndDisconnect(const int nClientUserID, const int nClientHandle,
	const string& svIPAddr, const NucleusID_t nNucleusID, const string& svPersonaName, const int nPort,
	const string& svServerIp, const int nServerPort,
	const PlayerAccessCheckRequest_t& request)
{
	if (SV_IsRemoteApiShutdownRequested() || !request.m_bValid)
		return;

	const string svDefaultReason = request.m_svDefaultReason.empty()
		? "Access denied by server policy"
		: request.m_svDefaultReason;

	PlayerAccessDecision_t decision;
	string svMessage;
	string svDebugMessage;

	const bool bSuccess = SV_QueryPlayerAccess(request, svIPAddr, nNucleusID, svPersonaName, nPort,
		svServerIp, nServerPort, decision, svMessage, svDebugMessage);

	g_TaskQueue.Dispatch([nClientUserID, nClientHandle, svIPAddr, nNucleusID, svPersonaName,
		nPort, bFailOpen = request.m_bFailOpen, svDefaultReason, bSuccess, decision,
		svMessage, svDebugMessage]
		{
			SV_FinishPlayerAccessCheck(nClientUserID, nClientHandle, svIPAddr, nNucleusID,
				svPersonaName, nPort, bFailOpen, svDefaultReason, bSuccess, decision,
				svMessage, svDebugMessage);
		}, 0);
}

static void SV_LogPlayerAccessDecision(const PlayerAccessDecision_t& decision,
	const PlayerAccessClientReason_t* const pClientReason,
	const string& svIPAddr, const NucleusID_t nNucleusID,
	const string& svPersonaName, const int nPort)
{
	if (decision.m_bAllow)
	{
		if (SV_IsRemoteApiDebugEnabled())
		{
			Msg(eDLL_T::SERVER, "Player access check: allow '%s' ('%llu') from '[%s]:%i'\n",
				svPersonaName.c_str(), nNucleusID, svIPAddr.c_str(), nPort);
		}
	}
	else
	{
		Assert(pClientReason);
		const PlayerAccessClientReason_t& clientReason = *pClientReason;

		Msg(eDLL_T::SERVER, "Player access check: deny '%s' ('%llu') from '[%s]:%i' rule='%s' fallback=%s reason_source=%s reason='%s' reason_en='%s' client_reason='%s'\n",
			svPersonaName.c_str(), nNucleusID, svIPAddr.c_str(), nPort,
			decision.m_svRuleId.c_str(),
			clientReason.m_eSource == PlayerAccessClientReasonSource_e::LOCALIZED ? "false" : "true",
			SV_GetPlayerAccessClientReasonSourceName(clientReason.m_eSource),
			decision.m_svReason.c_str(), decision.m_svReasonEn.c_str(), clientReason.m_svText.c_str());

		SV_WritePlayerAccessRejectLog(decision, clientReason, svIPAddr, nNucleusID, svPersonaName, nPort);
	}

	const char* const pszClientReason = pClientReason ? pClientReason->m_svText.c_str() : "";
	const bool bReasonFallback = pClientReason && pClientReason->m_eSource != PlayerAccessClientReasonSource_e::LOCALIZED;
	const char* const pszReasonSource = pClientReason
		? SV_GetPlayerAccessClientReasonSourceName(pClientReason->m_eSource)
		: "none";
	const string svDetail = Format("decision=%s uid=%llu name='%s' addr='%s:%i' rule='%s' fallback=%s reason_source=%s reason='%s' reason_en='%s' client_reason='%s'",
		decision.m_bAllow ? "allow" : "deny",
		static_cast<unsigned long long>(nNucleusID),
		svPersonaName.c_str(), svIPAddr.c_str(), nPort,
		decision.m_svRuleId.c_str(), bReasonFallback ? "true" : "false", pszReasonSource,
		decision.m_svReason.c_str(), decision.m_svReasonEn.c_str(), pszClientReason);
	SV_LogPlayerAccessDiagnostic("player_access_check", svDetail.c_str());
}

static bool SV_GetOnlineActionTargetId(const rapidjson::Value& value, NucleusID_t& outNucleusID)
{
	if (value.IsUint64())
	{
		outNucleusID = value.GetUint64();
		return outNucleusID != 0;
	}

	if (value.IsString())
		return JSON_StringToNumber(value.GetString(), value.GetStringLength(), outNucleusID) && outNucleusID != 0;

	if (!value.IsObject())
		return false;

	if (JSON_ParseNumber(value, "nucleusId", outNucleusID) && outNucleusID != 0)
		return true;

	if (JSON_ParseNumber(value, "uid", outNucleusID) && outNucleusID != 0)
		return true;

	if (JSON_ParseNumber(value, "id", outNucleusID) && outNucleusID != 0)
		return true;

	return false;
}

static bool SV_GetOnlineActionType(const string& svAction, PlayerAccessOnlineActionType_e& outType)
{
	if (_stricmp(svAction.c_str(), "kick") == 0)
	{
		outType = PlayerAccessOnlineActionType_e::KICK;
		return true;
	}

	if (_stricmp(svAction.c_str(), "ban") == 0)
	{
		outType = PlayerAccessOnlineActionType_e::BAN;
		return true;
	}

	return false;
}

static const char* SV_GetOnlineActionName(const PlayerAccessOnlineActionType_e eType)
{
	switch (eType)
	{
	case PlayerAccessOnlineActionType_e::BAN:
		return "ban";
	case PlayerAccessOnlineActionType_e::KICK:
	default:
		return "kick";
	}
}

static PlayerAccessClientReason_t SV_GetOnlineActionReason(const PlayerAccessOnlineAction_t& action)
{
	return SV_BuildPlayerAccessClientReason(action.m_svReason, action.m_svReasonEn);
}

static bool SV_AddOnlineActionFromValue(const rapidjson::Value& value,
	const PlayerAccessOnlineActionType_e eType, vector<PlayerAccessOnlineAction_t>& outActions)
{
	PlayerAccessOnlineAction_t action;
	action.m_eType = eType;

	if (!SV_GetOnlineActionTargetId(value, action.m_nNucleusID))
		return false;

	if (value.IsObject())
	{
		JSON_GetValue(value, "reason", action.m_svReason);
		JSON_GetValue(value, "reasonEn", action.m_svReasonEn);
		JSON_GetValue(value, "reasonCode", action.m_svReasonCode);
		JSON_GetValue(value, "ip", action.m_svIPAddr);
		JSON_GetValue(value, "processedAt", action.m_svProcessedAt);
		JSON_GetValue(value, "ruleId", action.m_svRuleId);
	}

	outActions.emplace_back(std::move(action));
	return true;
}

static bool SV_ParseTypedOnlineActionArray(const rapidjson::Document& responseJson, const char* const pszField,
	const PlayerAccessOnlineActionType_e eType, vector<PlayerAccessOnlineAction_t>& outActions,
	string& outMessage, vector<string>& outWarnings)
{
	rapidjson::Document::ConstMemberIterator actionsIt = responseJson.FindMember(rapidjson::StringRef(pszField));
	if (actionsIt == responseJson.MemberEnd())
		return true;

	if (!actionsIt->value.IsArray())
	{
		outMessage = Format("online player report response field '%s' was not an array", pszField);
		return false;
	}

	const rapidjson::Value::ConstArray actions = actionsIt->value.GetArray();
	for (const rapidjson::Value& value : actions)
	{
		if (!SV_AddOnlineActionFromValue(value, eType, outActions))
		{
			outWarnings.emplace_back(Format("Skipping invalid remote online player %s action entry",
				SV_GetOnlineActionName(eType)));
		}
	}

	return true;
}

static bool SV_ParseOnlinePlayerActions(const rapidjson::Document& responseJson,
	vector<PlayerAccessOnlineAction_t>& outActions, string& outMessage,
	vector<string>& outWarnings)
{
	rapidjson::Document::ConstMemberIterator actionsIt;
	if (JSON_GetIterator(responseJson, "actions", actionsIt))
	{
		if (!actionsIt->value.IsArray())
		{
			outMessage = "online player report response field 'actions' was not an array";
			return false;
		}

		const rapidjson::Value::ConstArray actions = actionsIt->value.GetArray();
		for (const rapidjson::Value& value : actions)
		{
			if (!value.IsObject())
			{
				outWarnings.emplace_back("Skipping invalid remote online player action entry: expected object");
				continue;
			}

			string svAction;
			PlayerAccessOnlineActionType_e eType = PlayerAccessOnlineActionType_e::KICK;

			if (!JSON_GetValue(value, "action", svAction) ||
				!SV_GetOnlineActionType(svAction, eType) ||
				!SV_AddOnlineActionFromValue(value, eType, outActions))
			{
				outWarnings.emplace_back("Skipping invalid remote online player action entry");
			}
		}
	}

	if (!SV_ParseTypedOnlineActionArray(responseJson, "kick", PlayerAccessOnlineActionType_e::KICK,
		outActions, outMessage, outWarnings))
		return false;

	if (!SV_ParseTypedOnlineActionArray(responseJson, "ban", PlayerAccessOnlineActionType_e::BAN,
		outActions, outMessage, outWarnings))
		return false;

	return true;
}

static void SV_BuildOnlinePlayersRequest(const PlayerAccessOnlineReport_t& report, rapidjson::Document& requestJson)
{
	requestJson.SetObject();

	rapidjson::Document::AllocatorType& allocator = requestJson.GetAllocator();
	rapidjson::Value playersArray(rapidjson::kArrayType);

	for (const PlayerAccessOnlinePlayer_t& reportPlayer : report.m_Players)
	{
		rapidjson::Value player(rapidjson::kObjectType);

		player.AddMember("uid", rapidjson::Value(reportPlayer.m_svUID.c_str(), reportPlayer.m_svUID.length(), allocator), allocator);
		player.AddMember("nucleusId", static_cast<uint64_t>(reportPlayer.m_nNucleusID), allocator);
		player.AddMember("playerName", rapidjson::Value(reportPlayer.m_svPersonaName.c_str(), reportPlayer.m_svPersonaName.length(), allocator), allocator);
		player.AddMember("ip", rapidjson::Value(reportPlayer.m_svIPAddr.c_str(), reportPlayer.m_svIPAddr.length(), allocator), allocator);
		player.AddMember("port", reportPlayer.m_nPort, allocator);
		player.AddMember("userId", reportPlayer.m_nUserID, allocator);
		player.AddMember("handle", reportPlayer.m_nHandle, allocator);
		player.AddMember("signonState", reportPlayer.m_nSignonState, allocator);
		player.AddMember("ping", reportPlayer.m_nPing, allocator);
		player.AddMember("inputDevice", rapidjson::Value(reportPlayer.m_svInputDevice.c_str(), reportPlayer.m_svInputDevice.length(), allocator), allocator);

		playersArray.PushBack(player, allocator);
	}

	SV_AddRemoteServerName(requestJson, allocator, report.m_svServerName);
	SV_AddRemoteServerAddress(requestJson, allocator, report.m_svServerIp, report.m_nServerPort);
	requestJson.AddMember("map", rapidjson::Value(report.m_svMap.c_str(), report.m_svMap.length(), allocator), allocator);
	requestJson.AddMember("tick", report.m_nTick, allocator);
	requestJson.AddMember("numPlayers", report.m_nNumPlayers, allocator);
	requestJson.AddMember("maxPlayers", report.m_nMaxPlayers, allocator);
	requestJson.AddMember("players", playersArray, allocator);
}

static bool SV_QueryOnlinePlayerActions(const RemoteApiRequest_t& request,
	const PlayerAccessOnlineReport_t& report,
	vector<PlayerAccessOnlineAction_t>& outActions, string& outMessage,
	string& outDebugMessage, vector<string>& outWarnings)
{
	rapidjson::Document requestJson;
	SV_BuildOnlinePlayersRequest(report, requestJson);

	rapidjson::Document responseJson;
	if (!SV_SendPlayerAccessJsonRequest(request, requestJson, responseJson,
		outMessage, outDebugMessage, "online player report"))
		return false;

	return SV_ParseOnlinePlayerActions(responseJson, outActions, outMessage, outWarnings);
}

static bool SV_GetValidOnlinePlayerState(CClient* const pClient, const NucleusID_t nNucleusID,
	const CNetChan*& outNetChan, CPlayer** const outPlayer = nullptr)
{
	outNetChan = nullptr;
	if (outPlayer)
		*outPlayer = nullptr;

	if (!pClient || !pClient->IsConnected() || !pClient->IsActive() ||
		pClient->IsFakeClient() || pClient->GetNucleusID() != nNucleusID)
		return false;

	const CNetChan* const pNetChan = pClient->GetNetChan();
	if (!pNetChan || pNetChan->GetRemoteAddress().IsLoopback())
		return false;

	const edict_t nHandle = pClient->GetHandle();
	if (!g_pServer || nHandle <= 0 || nHandle > g_pServer->GetMaxClients())
		return false;

	CPlayer* const pPlayer = UTIL_PlayerByIndex(nHandle);
	if (!pPlayer || !pPlayer->IsConnected() || pPlayer->IsBot())
		return false;

	if (pPlayer->GetEdict() != nHandle)
		return false;

	const NucleusID_t nPlayerNucleusID = pPlayer->GetPlatformUserId();
	if (nPlayerNucleusID != 0 && nPlayerNucleusID != nNucleusID)
		return false;

	outNetChan = pNetChan;
	if (outPlayer)
		*outPlayer = pPlayer;

	return true;
}

static bool SV_BuildOnlinePlayerReport(CServer* const pServer, PlayerAccessOnlineReport_t& outReport)
{
	if (!pServer || !pServer->IsActive())
		return false;

	const char* pszMapName = pServer->GetMapName();
	if (!pszMapName)
		pszMapName = "";

	outReport.m_nTick = pServer->GetTick();
	outReport.m_nMaxPlayers = SV_GetRemoteServerMaxPlayers();
	SV_GetRemoteServerName(outReport.m_svServerName);
	SV_GetRemoteServerAddress(outReport.m_svServerIp, outReport.m_nServerPort);
	outReport.m_svMap = pszMapName;

	for (int i = 0; i < pServer->GetMaxClients(); i++)
	{
		CClient* const pClient = pServer->GetClient(i);
		if (!pClient)
			continue;

		const NucleusID_t nNucleusID = pClient->GetNucleusID();
		if (nNucleusID == 0)
			continue;

		const CNetChan* pNetChan = nullptr;
		CPlayer* pPlayer = nullptr;
		if (!SV_GetValidOnlinePlayerState(pClient, nNucleusID, pNetChan, &pPlayer))
			continue;

		SV_TouchMatchReportPlayer(pPlayer);

		PlayerAccessOnlinePlayer_t reportPlayer;
		reportPlayer.m_nUserID = pClient->GetUserID();
		reportPlayer.m_nHandle = pClient->GetHandle();
		reportPlayer.m_nSignonState = static_cast<int>(pClient->GetSignonState());
		reportPlayer.m_nPing = static_cast<int>(1000.0f * Max(0.0f, pNetChan->GetAvgLatency(FLOW_OUTGOING)));
		reportPlayer.m_nPort = pNetChan->GetPort();
		reportPlayer.m_nNucleusID = nNucleusID;
		reportPlayer.m_svUID = Format("%llu", reportPlayer.m_nNucleusID);
		reportPlayer.m_svIPAddr = pNetChan->GetAddress(true);
		reportPlayer.m_svInputDevice = SV_GetPlayerInputDeviceName(pPlayer);

		const char* pszClientName = pClient->GetClientName();
		if (!VALID_CHARSTAR(pszClientName))
			pszClientName = pNetChan->GetName();

		reportPlayer.m_svPersonaName = VALID_CHARSTAR(pszClientName) ? pszClientName : "";

		outReport.m_Players.emplace_back(std::move(reportPlayer));
	}

	outReport.m_nNumPlayers = static_cast<int>(outReport.m_Players.size());
	return true;
}

static bool SV_FindOnlinePlayerByNucleusID(const NucleusID_t nNucleusID, CClient*& outClient, const CNetChan*& outNetChan)
{
	if (!g_pServer)
		return false;

	for (int i = 0; i < g_pServer->GetMaxClients(); i++)
	{
		CClient* const pClient = g_pServer->GetClient(i);
		const CNetChan* pNetChan = nullptr;
		if (!SV_GetValidOnlinePlayerState(pClient, nNucleusID, pNetChan))
			continue;

		outClient = pClient;
		outNetChan = pNetChan;
		return true;
	}

	return false;
}

static void SV_ApplyOnlinePlayerActions(const vector<PlayerAccessOnlineAction_t>& actions)
{
	for (const PlayerAccessOnlineAction_t& action : actions)
	{
		if (action.m_nNucleusID == 0)
			continue;

		CClient* pClient = nullptr;
		const CNetChan* pNetChan = nullptr;
		const bool bOnline = SV_FindOnlinePlayerByNucleusID(action.m_nNucleusID, pClient, pNetChan);

		if (!bOnline)
		{
			Warning(eDLL_T::SERVER, "Remote online player report could not find player '%llu' for %s%s%s\n",
				action.m_nNucleusID, SV_GetOnlineActionName(action.m_eType),
				action.m_svRuleId.empty() ? "" : " by policy ",
				action.m_svRuleId.empty() ? "" : action.m_svRuleId.c_str());
			continue;
		}

		const string svIPAddr(pNetChan->GetAddress(true));
		const int nPort = pNetChan->GetPort();
		const PlayerAccessClientReason_t clientReason = SV_GetOnlineActionReason(action);
		const string svActionDetail = Format("uid=%llu action=%s rule='%s' fallback=%s reason_source=%s reason='%s' reason_en='%s'",
			static_cast<unsigned long long>(action.m_nNucleusID),
			SV_GetOnlineActionName(action.m_eType),
			action.m_svRuleId.c_str(),
			clientReason.m_eSource == PlayerAccessClientReasonSource_e::LOCALIZED ? "false" : "true",
			SV_GetPlayerAccessClientReasonSourceName(clientReason.m_eSource),
			clientReason.m_svText.c_str(), action.m_svReasonEn.c_str());
		SV_LogPlayerAccessDiagnostic("online_report_action_lookup", svActionDetail.c_str());
		const string svQueueDetail = Format("%s addr='%s:%i'",
			svActionDetail.c_str(), svIPAddr.c_str(), nPort);
		SV_LogPlayerAccessDiagnostic("online_report_action_queue", svQueueDetail.c_str());
		SV_QueuePlayerAccessDisconnect(pClient, clientReason.m_svText, svIPAddr, nPort,
			action.m_nNucleusID, action.m_svRuleId, true, action.m_eType);
	}
}

// RAII guard ensuring the in-flight flag is cleared after the main-thread
// completion task logs and applies the report result.
struct OnlineReportInFlightGuard_t
{
	~OnlineReportInFlightGuard_t()
	{
		s_bOnlineReportInFlight.store(false);
	}
};

static void SV_FinishOnlinePlayerReport(PlayerAccessOnlineReport_t report,
	PlayerAccessOnlineReportResult_t result)
{
	OnlineReportInFlightGuard_t inFlightGuard;
	const string svResultDetail = Format("success=%d actions=%llu warnings=%llu",
		result.m_bSuccess ? 1 : 0,
		static_cast<unsigned long long>(result.m_Actions.size()),
		static_cast<unsigned long long>(result.m_Warnings.size()));
	SV_WriteOnlineReportBreadcrumb("online_report_finish_enter", report, svResultDetail.c_str());

	if (SV_IsRemoteApiShutdownRequested())
	{
		SV_WriteOnlineReportBreadcrumb("online_report_finish_shutdown", report, svResultDetail.c_str());
		return;
	}

	if (!result.m_svDebugMessage.empty())
		Msg(eDLL_T::SERVER, "%s\n", result.m_svDebugMessage.c_str());

	for (const string& svWarning : result.m_Warnings)
		Warning(eDLL_T::SERVER, "%s\n", svWarning.c_str());

	if (!result.m_bSuccess)
	{
		Warning(eDLL_T::SERVER, "Online player report failed: %s\n", result.m_svMessage.c_str());
		SV_WriteOnlineReportBreadcrumb("online_report_finish_failed", report, result.m_svMessage.c_str());
		return;
	}

	if (SV_IsRemoteApiDebugEnabled() || !result.m_Actions.empty())
	{
		Msg(eDLL_T::SERVER, "Online player report submitted for '%s' with %d/%d player(s), %llu action(s)\n",
			report.m_svMap.c_str(), report.m_nNumPlayers, report.m_nMaxPlayers,
			static_cast<unsigned long long>(result.m_Actions.size()));
	}
	SV_WriteOnlineReportBreadcrumb("online_report_finish_success", report, svResultDetail.c_str());

	if (result.m_Actions.empty())
		return;

	if (SV_IsRemoteApiShutdownRequested())
	{
		SV_WriteOnlineReportBreadcrumb("online_report_actions_shutdown", report, svResultDetail.c_str());
		return;
	}

	SV_WriteOnlineReportBreadcrumb("online_report_actions_apply", report, svResultDetail.c_str());
	SV_ApplyOnlinePlayerActions(result.m_Actions);
	SV_WriteOnlineReportBreadcrumb("online_report_actions_done", report, svResultDetail.c_str());
}

static void SV_SubmitOnlinePlayerReport(RemoteApiRequest_t request, PlayerAccessOnlineReport_t report)
{
	PlayerAccessOnlineReportResult_t result;
	SV_WriteOnlineReportBreadcrumb("online_report_worker_query", report);

	result.m_bSuccess = SV_QueryOnlinePlayerActions(request, report, result.m_Actions,
		result.m_svMessage, result.m_svDebugMessage, result.m_Warnings);
	const string svResultDetail = Format("success=%d actions=%llu warnings=%llu",
		result.m_bSuccess ? 1 : 0,
		static_cast<unsigned long long>(result.m_Actions.size()),
		static_cast<unsigned long long>(result.m_Warnings.size()));
	SV_WriteOnlineReportBreadcrumb("online_report_worker_done", report, svResultDetail.c_str());

	if (SV_IsRemoteApiShutdownRequested())
	{
		SV_WriteOnlineReportBreadcrumb("online_report_worker_shutdown", report, svResultDetail.c_str());
		s_bOnlineReportInFlight.store(false);
		return;
	}

	SV_WriteOnlineReportBreadcrumb("online_report_dispatch_finish", report, svResultDetail.c_str());
	g_TaskQueue.Dispatch([report, result]
		{
			SV_FinishOnlinePlayerReport(report, result);
		}, 0);
}

void SV_RunPlayerAccessOnlineReportFrame(CServer* const pServer)
{
	if (SV_IsRemoteApiShutdownRequested())
		return;

	if (!sv_player_access_report_enable.GetBool())
	{
		if (!s_bOnlineReportDisabledLogged)
		{
			Msg(eDLL_T::SERVER, "Online player access report disabled (sv_player_access_report_enable=0)\n");
			s_bOnlineReportDisabledLogged = true;
		}
		return;
	}

	s_bOnlineReportDisabledLogged = false;

	const double flNow = Plat_FloatTime();
	if (flNow >= s_flNextOnlineReportHeartbeatTime)
	{
		const double flNextReportIn = s_flNextOnlineReportTime > flNow
			? s_flNextOnlineReportTime - flNow
			: 0.0;
		const string svHeartbeatDetail = Format("enabled=1 in_flight=%d next_report_in=%.3f",
			s_bOnlineReportInFlight.load() ? 1 : 0, flNextReportIn);
		SV_LogPlayerAccessDiagnostic("online_report_frame", svHeartbeatDetail.c_str());
		s_flNextOnlineReportHeartbeatTime = flNow + PLAYER_ACCESS_REPORT_HEARTBEAT_INTERVAL;
	}

	if (flNow < s_flNextOnlineReportTime)
		return;

	if (s_bOnlineReportInFlight.load())
		return;

	s_flNextOnlineReportTime = flNow + sv_player_access_report_interval.GetFloat();

	// Capture all HTTP settings on the main thread; the worker must never read
	// ConVar string storage (see RemoteApiRequest_t).
	RemoteApiRequest_t request;
	string svMessage;
	if (!SV_ResolveRemoteApiUrl(sv_player_access_report_url.GetString(), PLAYER_ACCESS_ONLINE_DEFAULT_PATH,
		request.m_svUrl, svMessage, "sv_player_access_report_url"))
	{
		Warning(eDLL_T::SERVER, "Online player report skipped: %s\n", svMessage.c_str());
		return;
	}

	SV_CaptureRemoteApiAuth(request);
	request.m_bVerifyPeer = sv_player_access_ssl_verify_peer.GetBool();
	request.m_bVerbose = SV_IsRemoteApiDebugEnabled() || sv_player_access_debug.GetBool();

	PlayerAccessOnlineReport_t report;
	if (!SV_BuildOnlinePlayerReport(pServer, report))
	{
		Warning(eDLL_T::SERVER, "Online player report build failed; skipping report frame\n");
		SV_LogPlayerAccessDiagnostic("online_report_build_failed", "SV_BuildOnlinePlayerReport returned false");
		return;
	}

	s_bOnlineReportInFlight.store(true);
	const string svReportDetail = SV_FormatOnlineReportBreadcrumb(report);
	SV_WriteOnlineReportBreadcrumb("online_report_start", report);

	if (!SV_StartRemoteApiWorker("online-player-report",
		[request = std::move(request), report = std::move(report)]() mutable
		{
			SV_SubmitOnlinePlayerReport(std::move(request), std::move(report));
		}))
	{
		s_bOnlineReportInFlight.store(false);
		Warning(eDLL_T::SERVER, "Online player report worker failed to start: %s\n", svReportDetail.c_str());
		SV_LogPlayerAccessDiagnostic("online_report_worker_start_failed", svReportDetail.c_str());
	}
}
