#pragma once

#include "ebisusdk/EbisuTypes.h"
#include "networksystem/remoteapi.h"

class CClient;
class CServer;
class ConVar;

extern ConVar sv_player_access_enable;

//-----------------------------------------------------------------------------
// Snapshot of the player-access check configuration, captured on the main
// thread so the worker thread never touches ConVar string storage. See
// RemoteApiRequest_t for the rationale.
//-----------------------------------------------------------------------------
struct PlayerAccessCheckRequest_t
{
	RemoteApiRequest_t m_Http;
	string m_svServerName;
	string m_svDefaultReason;
	bool   m_bFailOpen = true;
	bool   m_bValid = false; // false = config could not be resolved; skip the check.
};

void SV_AppendPlayerAccessConfigSummary(string& outSummary);

// Captures the player-access check configuration on the main thread. Returns
// false (and sets m_bValid=false) when the endpoint cannot be resolved.
bool SV_CapturePlayerAccessCheckRequest(PlayerAccessCheckRequest_t& outRequest);

void SV_CheckPlayerAccessAndDisconnect(const int nClientUserID, const int nClientHandle,
	const string& svIPAddr, const NucleusID_t nNucleusID, const string& svPersonaName, const int nPort,
	const string& svServerIp, const int nServerPort,
	const PlayerAccessCheckRequest_t& request);
void SV_RunPlayerAccessOnlineReportFrame(CServer* const pServer);
