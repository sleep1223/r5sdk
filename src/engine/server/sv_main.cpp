//===========================================================================//
//
// Purpose:
//
//===========================================================================//
#include "core/stdafx.h"
#include <cstdint>
#include <memory>
#include "tier0/threadtools.h"
#include "tier0/frametask.h"
#include "tier1/cvar.h"
#include "engine/server/sv_main.h"
#include "engine/client/client.h"
#include "engine/net.h"
#include "networksystem/pylon.h"
#include "networksystem/bansystem.h"
#include "networksystem/clientdisconnect.h"
#include "networksystem/remoteapi.h"
#include "engine/client/client.h"
#include "server.h"
#include "game/server/gameinterface.h"
#include "game/server/util_server.h"

static ConVar sv_applyGlobalCommsBans("sv_applyGlobalCommsBans", "3", FCVAR_RELEASE, "Determines whether or not to use the global chat ban list, 0 = None, 1 = Text, 2 = Voice, 3 = Both.", false, 0.f, true, 3.f);
static ConVar sv_commsBansAreGameBans("sv_commsBansAreGameBans", "0", FCVAR_RELEASE, "If set chat bans will be applied as game bans", false, 0.f, true, 1.f);

static constexpr unsigned int GLOBAL_BAN_DISCONNECT_RETRY_FRAMES = 1;
static constexpr unsigned int GLOBAL_BAN_CONNECT_DENY_CLEANUP_FRAMES = 4;
static constexpr unsigned int GLOBAL_BAN_DISCONNECT_MAX_RETRIES = 600;
static constexpr unsigned int GLOBAL_BAN_DISCONNECT_KEY_HOLD_FRAMES = 32;

static CClientDisconnectTracker s_GlobalBanDisconnects;

bool SV_ShouldApplyTextChatGlobalMutes()
{
	const int globalBanSetting = sv_applyGlobalCommsBans.GetInt();
	if (globalBanSetting == 1 || globalBanSetting == 3)
		return true;
	return false;
}

bool SV_ShouldApplyVoiceChatGlobalMutes()
{
	const int globalBanSetting = sv_applyGlobalCommsBans.GetInt();
	if (globalBanSetting >= 2)
		return true;
	return false;
}

static bool SV_BeginGlobalBanDisconnect(const ClientDisconnectKey_t& key)
{
	return s_GlobalBanDisconnects.Begin(key);
}

static void SV_EndGlobalBanDisconnect(const ClientDisconnectKey_t& key)
{
	s_GlobalBanDisconnects.End(key);
}

static void SV_EndGlobalBanDisconnectDelayed(const ClientDisconnectKey_t& key)
{
	g_TaskQueue.Dispatch([key]
		{
			SV_EndGlobalBanDisconnect(key);
		}, GLOBAL_BAN_DISCONNECT_KEY_HOLD_FRAMES);
}

static void SV_DisconnectForGlobalBan(const ClientDisconnectKey_t& key, const string& svReason,
	const string& svIPAddr, const int nPort, const NucleusID_t nNucleusID,
	const string& svDescription, const unsigned int nAttempt = 0)
{
	if (SV_IsRemoteApiShutdownRequested())
	{
		SV_EndGlobalBanDisconnect(key);
		return;
	}

	CClient* const pClient = SV_ResolveClientDisconnectClient(key);
	if (!pClient)
	{
		Warning(eDLL_T::SERVER, "Skipped global ban disconnect for slot #%i handle=%i uid='%llu' because the client slot changed or client is gone\n",
			key.m_nUserID, key.m_nHandle, key.m_nNucleusID);
		SV_EndGlobalBanDisconnect(key);
		return;
	}

	const CNetChan* const pChan = pClient->GetNetChan();
	if (!pChan)
	{
		Warning(eDLL_T::SERVER, "Skipped global ban disconnect for '%llu' because the netchannel is gone\n",
			nNucleusID);
		SV_EndGlobalBanDisconnect(key);
		return;
	}

	const int nUserID = pClient->GetUserID();
	const int nHandle = pClient->GetHandle();
	const int nSignonState = static_cast<int>(pClient->GetSignonState());

	if (!pClient->IsActive())
	{
		if (nAttempt >= GLOBAL_BAN_CONNECT_DENY_CLEANUP_FRAMES)
		{
			Warning(eDLL_T::SERVER, "Forcing global ban pre-active cleanup for '[%s]:%i' from slot #%i handle=%i signon=%i uid='%llu' reason='%s'\n",
				svIPAddr.c_str(), nPort, nUserID, nHandle, nSignonState, nNucleusID, svReason.c_str());
			SV_DisconnectClientNow(pClient, svReason.c_str());
			SV_EndGlobalBanDisconnectDelayed(key);
			return;
		}

		if (nAttempt >= GLOBAL_BAN_DISCONNECT_MAX_RETRIES)
		{
			Warning(eDLL_T::SERVER, "Skipped global ban disconnect for '[%s]:%i' from slot #%i handle=%i signon=%i uid='%llu' because signon did not complete\n",
				svIPAddr.c_str(), nPort, nUserID, nHandle, nSignonState, nNucleusID);
			SV_EndGlobalBanDisconnect(key);
			return;
		}

		if (nAttempt == 0)
		{
			Warning(eDLL_T::SERVER, "Delaying global ban disconnect for '[%s]:%i' from slot #%i handle=%i signon=%i uid='%llu' until signon is complete\n",
				svIPAddr.c_str(), nPort, nUserID, nHandle, nSignonState, nNucleusID);
		}

		g_TaskQueue.Dispatch([key, svReason, svIPAddr, nPort, nNucleusID, svDescription, nAttempt]
			{
				SV_DisconnectForGlobalBan(key, svReason, svIPAddr, nPort,
					nNucleusID, svDescription, nAttempt + 1);
			}, GLOBAL_BAN_DISCONNECT_RETRY_FRAMES);
		return;
	}

	Warning(eDLL_T::SERVER, "Global ban disconnecting '[%s]:%i' from slot #%i handle=%i signon=%i uid='%llu' reason='%s'\n",
		svIPAddr.c_str(), nPort, nUserID, nHandle, nSignonState, nNucleusID, svReason.c_str());

	SV_DisconnectClientNow(pClient, svReason.c_str());

	Warning(eDLL_T::SERVER, "Removed client '[%s]:%i' from slot #%i ('%llu' %s)\n",
		svIPAddr.c_str(), nPort, nUserID, nNucleusID, svDescription.c_str());

	SV_EndGlobalBanDisconnectDelayed(key);
}

static void SV_QueueGlobalBanDisconnect(CClient* const pClient, const char* const pszReason,
	const char* const pszIpStr, const int nPort, const NucleusID_t nNucleusID,
	const char* const pszDescription)
{
	const ClientDisconnectKey_t key = SV_MakeClientDisconnectKey(pClient, nNucleusID);
	if (!SV_IsValidClientDisconnectKey(key))
	{
		Warning(eDLL_T::SERVER, "Skipped global ban disconnect for '%llu' because the captured client identity is invalid\n",
			nNucleusID);
		return;
	}

	if (!SV_BeginGlobalBanDisconnect(key))
	{
		Warning(eDLL_T::SERVER, "Skipped duplicate global ban disconnect for '%llu'\n",
			nNucleusID);
		return;
	}

	const string svReason(VALID_CHARSTAR(pszReason) ? pszReason : "Banned from server");
	const string svIPAddr(VALID_CHARSTAR(pszIpStr) ? pszIpStr : "unknown");
	const string svDescription(VALID_CHARSTAR(pszDescription) ? pszDescription : "is banned");

	g_TaskQueue.Dispatch([key, svReason, svIPAddr, nPort, nNucleusID, svDescription]
		{
			SV_DisconnectForGlobalBan(key, svReason, svIPAddr, nPort,
				nNucleusID, svDescription);
		}, 0);
}

static bool SV_GlobalCommsBansEnabled()
{
	if (sv_applyGlobalCommsBans.GetInt() != 0)
		return true;
	return false;
}

static void SV_HandleConnectBan(CClient* const pClient, const char* const pszReason, const char* const pszIpStr, const int nPort, const NucleusID_t nNucleusID)
{
	SV_QueueGlobalBanDisconnect(pClient, pszReason, pszIpStr, nPort, nNucleusID, "is banned globally!");
}

static void SV_HandleCommunicationBan(CClient* const pClient, const char* const pszReason, const char* const pszExpiry, const char* const pszIpStr, const int nPort, const NucleusID_t nNucleusID)
{
	const int nUserId = pClient->GetUserID();
	CClientExtended* const pClientExtended = pClient->GetClientExtended();
	if (!pClientExtended)
		return;

	if (sv_commsBansAreGameBans.GetBool())
	{
		SV_QueueGlobalBanDisconnect(pClient, pszReason, pszIpStr, nPort, nNucleusID, "is communication banned and communication bans are treated as game bans!");
	}
	else
	{
		DevMsg(eDLL_T::SERVER, "Muting client '[%s]:%i' from slot #%i ('%llu' is communication banned!)\n",
			pszIpStr, nPort, nUserId, nNucleusID);
	}

	pClientExtended->SetClientIsCommsBanned(true);
	pClientExtended->SetCommsBanInfo(pszReason, pszExpiry);
}

//-----------------------------------------------------------------------------
// Purpose: checks if particular client is banned on the comp server
//-----------------------------------------------------------------------------
void SV_CheckForBanAndDisconnect(const int nClientUserID, const int nClientHandle,
	const string& svIPAddr,
	const NucleusID_t nNucleusID, const string& svPersonaName, const int nPort,
	const PylonRequestConfig_t& requestConfig)
{
	if (SV_IsRemoteApiShutdownRequested())
		return;

	string svError;
	string expiry;
	CBanSystem::Banned_t::BanType_e banType = CBanSystem::Banned_t::CONNECT;
	
	const bool bCompBanned = g_MasterServer.CheckForBan(requestConfig, svIPAddr, nNucleusID, svPersonaName, svError, banType, expiry);

	if (!bCompBanned || SV_IsRemoteApiShutdownRequested())
		return;

	ClientDisconnectKey_t key;
	key.m_nUserID = nClientUserID;
	key.m_nHandle = nClientHandle;
	key.m_nNucleusID = nNucleusID;

	g_TaskQueue.Dispatch([key, svError, svIPAddr, nNucleusID, nPort, banType, expiry]
		{
			if (SV_IsRemoteApiShutdownRequested())
				return;

			// Make sure client isn't already disconnected,
			// and that if there is a valid netchannel, that
			// it hasn't been taken by a different client by
			// the time this task is getting executed.
			CClient* const pClient = SV_ResolveClientDisconnectClient(key);
			if (!pClient)
				return;

			const CNetChan* const pChan = pClient->GetNetChan();
			if (pChan && pClient->GetNucleusID() == nNucleusID)
			{
				switch (banType)
				{
				case CBanSystem::Banned_t::CONNECT:
				{
					SV_HandleConnectBan(pClient, svError.c_str(), svIPAddr.c_str(), nPort, nNucleusID);
					break;
				}
				case CBanSystem::Banned_t::COMMUNICATION:
				{
					if(SV_GlobalCommsBansEnabled())
						SV_HandleCommunicationBan(pClient, svError.c_str(), expiry.c_str(), svIPAddr.c_str(), nPort, nNucleusID);
					break;
				}
				default:
					break;
				}
			}
		}, 0);
}

//-----------------------------------------------------------------------------
// Purpose: checks if particular client is banned on the master server
//-----------------------------------------------------------------------------
void SV_ProcessBulkCheck(const CBanSystem::BannedList_t* const pBannedVec, const PylonRequestConfig_t& requestConfig)
{
	if (SV_IsRemoteApiShutdownRequested())
		return;

	CBanSystem::BannedList_t* outBannedVec = nullptr;

	if (!g_MasterServer.GetBannedList(requestConfig, *pBannedVec, &outBannedVec))
		return;

	// Own the result in a shared_ptr so it is freed even if the dispatched task
	// is never executed (e.g. the task queue is torn down at shutdown before the
	// task runs). std::function requires copyable captures, so shared_ptr (not
	// unique_ptr) is used here.
	std::shared_ptr<CBanSystem::BannedList_t> spBannedVec(outBannedVec);

	if (SV_IsRemoteApiShutdownRequested())
		return;

	g_TaskQueue.Dispatch([spBannedVec]
		{
			if (SV_IsRemoteApiShutdownRequested())
				return;

			SV_CheckClientsForBan(spBannedVec.get());
		}, 0);
}

//-----------------------------------------------------------------------------
// Purpose: creates a snapshot of the currently connected clients
// Input  : *pBannedVec - if passed, will check for bans and kick the clients
//-----------------------------------------------------------------------------
void SV_CheckClientsForBan(const CBanSystem::BannedList_t* const pBannedVec /*= nullptr*/)
{
	Assert(ThreadInMainThread());

	if (SV_IsRemoteApiShutdownRequested())
		return;

	CBanSystem::BannedList_t* bannedVec = !pBannedVec 
		? new CBanSystem::BannedList_t 
		: nullptr;

	for (int c = 0; c < gpGlobals->maxClients; c++) // Loop through all possible client instances.
	{
		CClient* const pClient = g_pServer->GetClient(c);

		if (!pClient || !pClient->IsConnected())
			continue;

		const CNetChan* const pNetChan = pClient->GetNetChan();
		if (!pNetChan)
			continue;

		if (pNetChan->GetRemoteAddress().IsLoopback())
			continue;

		CPlayer* const pPlayer = UTIL_PlayerByIndex(pClient->GetHandle());

		// Bots shouldn't be checked for bans because these are added by the
		// server host.
		if (pPlayer && pPlayer->IsBot())
			continue;

		const char* const szIPAddr = pNetChan->GetAddress(true);
		const NucleusID_t nNucleusID = pClient->GetNucleusID();

		// If no banned list was provided, build one with all clients
		// on the server. This will be used for bulk checking so live
		// bans could be performed, as this function is called periodically.
		if (bannedVec)
			bannedVec->AddToTail(CBanSystem::Banned_t(szIPAddr, nNucleusID));
		else
		{
			// Check if current client is within provided banned list, and
			// prune if so...
			FOR_EACH_VEC(*pBannedVec, i)
			{
				const CBanSystem::Banned_t& banned = (*pBannedVec)[i];

				//If this ban isnt for this client then we check the next
				if (banned.m_NucleusID != pClient->GetNucleusID())
					continue;

				const int nPort = pNetChan->GetPort();

				//What ban type do we have for this client
				switch (banned.m_BanType)
				{
				case CBanSystem::Banned_t::CONNECT:
				{
					SV_HandleConnectBan(pClient, banned.m_Address.String(), szIPAddr, nPort, nNucleusID);
					break;
				}
				case CBanSystem::Banned_t::COMMUNICATION:
				{
					//Does the host have the comms ban system on and is our client already banned, no point rebanning them if they are
					CClientExtended* const pClientExtended = pClient->GetClientExtended();
					if (pClientExtended && SV_GlobalCommsBansEnabled() && (!pClientExtended->IsClientCommsBanned() || sv_commsBansAreGameBans.GetBool()))
						SV_HandleCommunicationBan(pClient, banned.m_Address.String(), banned.m_BanExpiry.Get(), szIPAddr, nPort, nNucleusID);
					break;
				}
				//Unknown ban type
				default:
					break;
				}

				//Since we have handled this client we can move onto the next one
				break;
			}
		}
	}

	if (bannedVec && !bannedVec->IsEmpty())
	{
		PylonRequestConfig_t pylonRequestConfig;
		g_MasterServer.CaptureRequestConfig(pylonRequestConfig);

		if (!SV_StartRemoteApiWorker("pylon-bulk-ban-check", [bannedVec, pylonRequestConfig]
			{
				if (!SV_IsRemoteApiShutdownRequested())
					SV_ProcessBulkCheck(bannedVec, pylonRequestConfig);

				delete bannedVec;
			}))
		{
			delete bannedVec;
		}
	}
	else if (bannedVec)
	{
		delete bannedVec;
		bannedVec = nullptr;
	}
}

//-----------------------------------------------------------------------------
// Purpose: loads the game .dll
//-----------------------------------------------------------------------------
void SV_InitGameDLL()
{
	v_SV_InitGameDLL();
}

//-----------------------------------------------------------------------------
// Purpose: release resources associated with extension DLLs.
//-----------------------------------------------------------------------------
void SV_ShutdownGameDLL()
{
	v_SV_ShutdownGameDLL();
}

//-----------------------------------------------------------------------------
// Purpose: activates the server
// Output : true on success, false on failure
//-----------------------------------------------------------------------------
bool SV_ActivateServer()
{
	return v_SV_ActivateServer();
}

//-----------------------------------------------------------------------------
// Purpose: returns whether voice data can be broadcasted from the server
//-----------------------------------------------------------------------------
bool SV_CanBroadcastVoice()
{
	if (IsPartyDedi())
		return false;

	if (IsTrainingDedi())
		return false;

	if (!sv_voiceenable->GetBool())
		return false;

	if (gpGlobals->maxClients <= 0)
		return false;

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: relays voice data to other clients
//-----------------------------------------------------------------------------
void SV_BroadcastVoiceData(CClient* const cl, const int nBytes, char* const data)
{
	if (!SV_CanBroadcastVoice())
		return;

	const bool bShouldApplyGlobalMutes = SV_ShouldApplyVoiceChatGlobalMutes();
	const bool bBannedClientsCanHearOtherClients = sv_commsBannedClientsCanReceiveComms.GetBool();

	SVC_VoiceData voiceData(cl->GetUserID(), nBytes, data);

	for (int i = 0; i < gpGlobals->maxClients; i++)
	{
		CClient* const pClient = g_pServer->GetClient(i);

		// is this client fully connected
		if (pClient->GetSignonState() != SIGNONSTATE::SIGNONSTATE_FULL)
			continue;

		CClientExtended* const pClientExtended = pClient->GetClientExtended();
		if (!pClientExtended)
			continue;

		//If the client is communication banned and the server has decidecd that players who are comms banned cant hear other players, skip broadcasting to them
		if (!bBannedClientsCanHearOtherClients && bShouldApplyGlobalMutes && pClientExtended->IsClientCommsBanned())
			continue;

		// is this client the sender
		if (pClient == cl && !sv_voiceEcho->GetBool())
			continue;

		// is this client on the sender's team
		if (pClient->GetTeamNum() != cl->GetTeamNum() && !sv_alltalk->GetBool())
			continue;

		//if (voice_noxplat->GetBool() && cl->GetXPlatID() != pClient->GetXPlatID())
		//{
		//	if ((cl->GetXPlatID() -1) > 1 || (pClient->GetXPlatID() -1) > 1)
		//		continue;
		//}

		CNetChan* const pNetChan = pClient->GetNetChan();

		if (!pNetChan)
			continue;

		// if voice stream has enough space for new data
		if (pNetChan->GetStreamVoice().GetNumBitsLeft() >= 8 * nBytes + 96)
			pClient->SendNetMsgEx(&voiceData, false, false, true);
	}
}

//-----------------------------------------------------------------------------
// Purpose: relays durango voice data to other clients
//-----------------------------------------------------------------------------
void SV_BroadcastDurangoVoiceData(CClient* const cl, const int nBytes, char* const data,
	const int nXid, const int unknown, const bool useVoiceStream, const bool skipXidCheck)
{
	if (!SV_CanBroadcastVoice())
		return;

	const bool bShouldApplyGlobalMutes = SV_ShouldApplyVoiceChatGlobalMutes();
	const bool bBannedClientsCanHearOtherClients = sv_commsBannedClientsCanReceiveComms.GetBool();

	SVC_DurangoVoiceData voiceData(cl->GetUserID(), nBytes, data, unknown, useVoiceStream);

	for (int i = 0; i < gpGlobals->maxClients; i++)
	{
		CClient* const pClient = g_pServer->GetClient(i);

		// is this client fully connected
		if (pClient->GetSignonState() != SIGNONSTATE::SIGNONSTATE_FULL)
			continue;

		CClientExtended* const pClientExtended = pClient->GetClientExtended();
		if (!pClientExtended)
			continue;

		//If the client is communication banned and the server has decidecd that players who are comms banned cant other players, skip broadcasting to them
		if (!bBannedClientsCanHearOtherClients && bShouldApplyGlobalMutes && pClientExtended->IsClientCommsBanned())
			continue;

		// is this client the sender
		if (pClient == cl && !sv_voiceEcho->GetBool())
			continue;

		if (!skipXidCheck && i != nXid)
			continue;

		// is this client on the sender's team
		if (pClient->GetTeamNum() != cl->GetTeamNum() && !sv_alltalk->GetBool())
		{
			// NOTE: on Durango packets, the game appears to bypass the team
			// check if 'useVoiceStream' is false, thus forcing the usage
			// of the reliable stream. Omitted the check as it appears that
			// could be exploited to transmit voice to other teams while cvar
			// 'sv_alltalk' is unset.
			continue;
		}

		// NOTE: xplat code checks disabled; CClient::GetXPlatID() seems to be
		// an enumeration of platforms, but the enum hasn't been reversed yet.
		//if (voice_noxplat->GetBool() && cl->GetXPlatID() != pClient->GetXPlatID())
		//{
		//	if ((cl->GetXPlatID() - 1) > 1 || (pClient->GetXPlatID() - 1) > 1)
		//		continue;
		//}

		CNetChan* const pNetChan = pClient->GetNetChan();

		if (!pNetChan)
			continue;

		// NOTE: the game appears to have the ability to use the unreliable
		// stream as well, but the condition to hit that code path can never
		// evaluate to true - appears to be a compile time option that hasn't
		// been fully optimized away? For now only switch between voice and
		// reliable streams as that is what the original code does.
		const bf_write& stream = useVoiceStream ? pNetChan->GetStreamVoice() : pNetChan->GetStreamReliable();

		// if stream has enough space for new data
		if (stream.GetNumBitsLeft() >= 8 * nBytes + 34)
			pClient->SendNetMsgEx(&voiceData, false, !useVoiceStream, useVoiceStream);
	}
}
