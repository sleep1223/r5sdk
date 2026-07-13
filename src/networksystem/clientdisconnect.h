#pragma once

#include <functional>
#include <mutex>
#include <unordered_set>
#include "ebisusdk/EbisuTypes.h"

class CClient;

struct ClientDisconnectKey_t
{
	int m_nUserID = -1;
	int m_nHandle = 0;
	NucleusID_t m_nNucleusID = 0;

	bool operator==(const ClientDisconnectKey_t& other) const
	{
		return m_nUserID == other.m_nUserID &&
			m_nHandle == other.m_nHandle &&
			m_nNucleusID == other.m_nNucleusID;
	}
};

struct ClientDisconnectKeyHash_t
{
	size_t operator()(const ClientDisconnectKey_t& key) const
	{
		const size_t userHash = std::hash<int>()(key.m_nUserID);
		const size_t handleHash = std::hash<int>()(key.m_nHandle);
		const size_t nucleusHash = std::hash<NucleusID_t>()(key.m_nNucleusID);
		size_t seed = userHash;
		seed ^= handleHash + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
		seed ^= nucleusHash + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
		return seed;
	}
};

class CClientDisconnectTracker
{
public:
	bool Begin(const ClientDisconnectKey_t& key);
	void End(const ClientDisconnectKey_t& key);

private:
	std::mutex m_Mutex;
	std::unordered_set<ClientDisconnectKey_t, ClientDisconnectKeyHash_t> m_Pending;
};

bool SV_IsValidClientDisconnectKey(const ClientDisconnectKey_t& key);
ClientDisconnectKey_t SV_MakeClientDisconnectKey(CClient* const pClient, const NucleusID_t nNucleusID);
CClient* SV_ResolveClientDisconnectClient(const ClientDisconnectKey_t& key);
const char* SV_GetClientDisconnectFallbackReason();
bool SV_IsClientSafeDisconnectReason(const char* const pszReason);
void SV_DisconnectClientNow(CClient* const pClient, const char* const pszReason);
