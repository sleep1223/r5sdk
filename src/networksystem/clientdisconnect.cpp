//=============================================================================//
//
// Purpose: Shared helpers for safely disconnecting a captured server client.
//
//=============================================================================//

#include "core/stdafx.h"
#include "tier1/strtools.h"
#include "engine/client/client.h"
#include "engine/net.h"
#include "engine/server/server.h"
#include "networksystem/clientdisconnect.h"

static constexpr const char* CLIENT_DISCONNECT_FALLBACK_REASON = "Disconnected from server";
static constexpr size_t CLIENT_DISCONNECT_REASON_MAX_LENGTH = 192;

static bool SV_IsLogSafeClientDisconnectReason(const char* const pszReason)
{
	if (!VALID_CHARSTAR(pszReason))
		return false;

	for (const unsigned char* p = reinterpret_cast<const unsigned char*>(pszReason); *p; ++p)
	{
		if (*p < 0x20 || *p == 0x7f)
			return false;
	}

	return V_IsValidUTF8(pszReason);
}

const char* SV_GetClientDisconnectFallbackReason()
{
	return CLIENT_DISCONNECT_FALLBACK_REASON;
}

bool SV_IsClientSafeDisconnectReason(const char* const pszReason)
{
	if (!VALID_CHARSTAR(pszReason))
		return false;

	size_t nLength = 0;
	for (const unsigned char* p = reinterpret_cast<const unsigned char*>(pszReason); *p; ++p)
	{
		if (++nLength > CLIENT_DISCONNECT_REASON_MAX_LENGTH)
			return false;

		if (*p < 0x20 || *p == 0x7f)
			return false;
	}

	return V_IsValidUTF8(pszReason);
}

bool CClientDisconnectTracker::Begin(const ClientDisconnectKey_t& key)
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	return m_Pending.insert(key).second;
}

void CClientDisconnectTracker::End(const ClientDisconnectKey_t& key)
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	m_Pending.erase(key);
}

bool SV_IsValidClientDisconnectKey(const ClientDisconnectKey_t& key)
{
	return key.m_nUserID >= 0 && key.m_nHandle > 0 && key.m_nNucleusID != 0;
}

ClientDisconnectKey_t SV_MakeClientDisconnectKey(CClient* const pClient,
	const NucleusID_t nNucleusID)
{
	ClientDisconnectKey_t key;
	if (!pClient)
		return key;

	key.m_nUserID = pClient->GetUserID();
	key.m_nHandle = static_cast<int>(pClient->GetHandle());
	key.m_nNucleusID = nNucleusID;
	return key;
}

CClient* SV_ResolveClientDisconnectClient(const ClientDisconnectKey_t& key)
{
	if (!SV_IsValidClientDisconnectKey(key) || !g_pServer)
		return nullptr;

	if (key.m_nUserID >= g_pServer->GetMaxClients())
		return nullptr;

	CClient* const pClient = g_pServer->GetClient(key.m_nUserID);
	if (!pClient || !pClient->IsConnected())
		return nullptr;

	if (pClient->GetUserID() != key.m_nUserID ||
		static_cast<int>(pClient->GetHandle()) != key.m_nHandle ||
		pClient->GetNucleusID() != key.m_nNucleusID)
	{
		return nullptr;
	}

	return pClient;
}

void SV_DisconnectClientNow(CClient* const pClient, const char* const pszReason)
{
	if (!pClient)
		return;

	const bool bClientSafeReason = SV_IsClientSafeDisconnectReason(pszReason);
	const char* const pszSafeReason = bClientSafeReason
		? pszReason
		: SV_GetClientDisconnectFallbackReason();

	if (!bClientSafeReason)
	{
		if (SV_IsLogSafeClientDisconnectReason(pszReason))
		{
			Warning(eDLL_T::SERVER, "Downgrading client disconnect reason for '%llu' from '%s' to '%s'\n",
				pClient->GetNucleusID(), pszReason, pszSafeReason);
		}
		else
		{
			Warning(eDLL_T::SERVER, "Downgrading invalid client disconnect reason for '%llu' to '%s'\n",
				pClient->GetNucleusID(), pszSafeReason);
		}
	}

	if (pClient->IsActive())
	{
		pClient->Disconnect(Reputation_t::REP_MARK_BAD, "%s", pszSafeReason);
		return;
	}

	NET_RemoveChannel(pClient, pClient->GetUserID(), pszSafeReason, 1, true);
}
