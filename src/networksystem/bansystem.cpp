//=====================================================================================//
//
// Purpose: Implementation of the CBanSystem class.
//
// $NoKeywords: $
//=====================================================================================//

#include "core/stdafx.h"
#include "tier1/strtools.h"
#include "tier2/jsonutils.h"
#include "engine/net.h"
#include "engine/server/server.h"
#include "engine/client/client.h"
#include "filesystem/filesystem.h"
#include "networksystem/bansystem.h"
#include "networksystem/clientdisconnect.h"
#include "game/server/gameinterface.h"
#include "game/server/logger_websocket.h"

//-----------------------------------------------------------------------------
// Purpose: converts IPv6 address to string
//-----------------------------------------------------------------------------
std::string CBanSystem::ConvertIpToString(const in6_addr* const adr) const
{
	if (!adr)
		return "";

	char adrBuf[INET6_ADDRSTRLEN];
	if (inet_ntop(AF_INET6, adr, adrBuf, sizeof(adrBuf)))
		return std::string(adrBuf);

	return "";
}

static void BanSystem_DisconnectClient(CClient* const pClient, const char* const reason)
{
	SV_DisconnectClientNow(pClient, reason);
}

//-----------------------------------------------------------------------------
// Purpose: loads and parses the banned list
//-----------------------------------------------------------------------------
void CBanSystem::LoadList(void)
{
	FileHandle_t pFile = FileSystem()->Open("banlist.json", "rb", "PLATFORM");
	if (!pFile)
		return;

	const ssize_t nFileSize = FileSystem()->Size(pFile);

	if (nFileSize <= 0)
	{
		Error(eDLL_T::SERVER, 0, "%s: Banned list file is empty\n", __FUNCTION__);
		FileSystem()->Close(pFile);

		return;
	}

	const u64 nBufSize = FileSystem()->GetOptimalReadSize(pFile, nFileSize + 2);
	char* const pBuf = (char*)FileSystem()->AllocOptimalReadBuffer(pFile, nBufSize, 0);
	if (!pBuf)
	{
		Error(eDLL_T::SERVER, 0, "%s: Failed to allocate banned list read buffer\n", __FUNCTION__);
		FileSystem()->Close(pFile);
		return;
	}

	const ssize_t nRead = FileSystem()->ReadEx(pBuf, nBufSize, nFileSize, pFile);
	FileSystem()->Close(pFile);

	if (nRead <= 0)
	{
		Error(eDLL_T::SERVER, 0, "%s: Banned list file read failure\n", __FUNCTION__);
		FileSystem()->FreeOptimalReadBuffer(pBuf);

		return;
	}

	pBuf[nFileSize] = '\0'; // Null terminate the string buffer containing our banned list.
	pBuf[nFileSize+1] = '\0'; // Double null terminating in case this is an unicode file.

	rapidjson::Document document;
	if (document.Parse(pBuf, nRead).HasParseError())
	{
		Error(eDLL_T::SERVER, 0, "%s: JSON parse error at position %zu: %s\n",
			__FUNCTION__, document.GetErrorOffset(), rapidjson::GetParseError_En(document.GetParseError()));
		FileSystem()->FreeOptimalReadBuffer(pBuf);

		return;
	}

	// Buffer is no longer needed.
	FileSystem()->FreeOptimalReadBuffer(pBuf);

	// ===== Check if this is v2 format (has "version" and "entries") =====
	if (document.IsObject() && document.HasMember("version"))
	{
		// This is v2 format - extract entries
		if (document.HasMember("entries") && document["entries"].IsArray())
		{
			for (const rapidjson::Value& entry : document["entries"].GetArray())
			{
				if (!entry.IsObject())
					continue;

				NucleusID_t nuc = 0;
				if (entry.HasMember("nucleusId") && entry["nucleusId"].IsUint64())
					nuc = entry["nucleusId"].GetUint64();

				std::string ipStr;
				if (entry.HasMember("ipAddress") && entry["ipAddress"].IsString())
				{
					const char* ip = entry["ipAddress"].GetString();
					if (ip && ip[0] != '\0')
						ipStr = ip;
				}

				std::string playerName;
				if (entry.HasMember("playerName") && entry["playerName"].IsString())
				{
					const char* name = entry["playerName"].GetString();
					if (name)
						playerName = name;
				}

				std::string banReason;
				if (entry.HasMember("banReason") && entry["banReason"].IsString())
				{
					const char* reason = entry["banReason"].GetString();
					if (reason)
						banReason = reason;
				}

				int64_t banTimestamp = 0;
				if (entry.HasMember("banTimestamp") && entry["banTimestamp"].IsInt64())
					banTimestamp = entry["banTimestamp"].GetInt64();

				std::string bannedByID;
				if (entry.HasMember("bannedByID") && entry["bannedByID"].IsString())
				{
					const char* banner = entry["bannedByID"].GetString();
					if (banner)
						bannedByID = banner;
				}

				BanMetadata_t metadata(nuc, playerName.c_str(), banReason.c_str(), bannedByID.c_str(), ipStr.c_str(), banTimestamp);

				if (nuc != 0)
				{
					m_bannedIdList.insert(nuc);
					m_banMetadataById[nuc] = metadata;
				}

				if (!ipStr.empty())
				{
					netadr_t adr;
					if (adr.SetFromString(ipStr.c_str(), true))
					{
						m_bannedIpList.insert(adr.GetIP());
						m_banMetadataByIp[ipStr] = metadata;
					}
				}
			}
		}

		Msg(eDLL_T::SERVER, "%s: Loaded v2 format banlist with %zu nucleus bans and %zu ip bans\n", __FUNCTION__, m_bannedIdList.size(), m_bannedIpList.size());
		return;
	}

	// ===== Legacy v1 format (array of IDs and IPs) =====
	if (!document.IsArray())
	{
		Error(eDLL_T::SERVER, 0, "%s: JSON root was not an array or object\n", __FUNCTION__);
		return;
	}

	ssize_t currIdx = -1;

	for (const rapidjson::Value& entry : document.GetArray())
	{
		currIdx++;

		if (entry.IsUint64())
		{
			const NucleusID_t nuc = entry.GetUint64();

			if (nuc == 0)
			{
				Warning(eDLL_T::SERVER, "%s: Nucleus ID (%llu) at index #%zd is zero!\n", __FUNCTION__, nuc, currIdx); //order was wrong on sdk
				continue;
			}

			m_bannedIdList.insert(entry.GetUint64());
			continue;
		}

		if (entry.IsString())
		{
			netadr_t adr;
			const char* const adrStr = entry.GetString();

			if (!adr.SetFromString(adrStr, true))
			{
				Warning(eDLL_T::SERVER, "%s: IP Address (%s) at index #%zd is invalid!\n", __FUNCTION__, adrStr, currIdx);
				continue;
			}

			m_bannedIpList.insert(adr.GetIP());
			continue;
		}

		Error(eDLL_T::SERVER, 0, "%s: Entry #%zd is of type %s, but code expects type %s or %s\n", __FUNCTION__, currIdx,
			JSON_TypeToString(JSON_ExtractType(entry)), JSON_TypeToString(JSONFieldType_e::kUint64), JSON_TypeToString(JSONFieldType_e::kString));
	}
}

//-----------------------------------------------------------------------------
// Purpose: saves the banned list ( with v2 format )
//-----------------------------------------------------------------------------
void CBanSystem::SaveList(void) const
{
	FileHandle_t pFile = FileSystem()->Open("banlist.json", "wb", "PLATFORM");
	if (!pFile)
	{
		Error(eDLL_T::SERVER, NO_ERROR, "%s - Unable to write to '%s' (read-only?)\n", __FUNCTION__, "banlist.json");
		return;
	}

	rapidjson::Document document;
	document.SetObject();

	rapidjson::Document::AllocatorType& allocator = document.GetAllocator();

	// ===== V2 Format with metadata =====
	document.AddMember("version", 2, allocator);
	document.AddMember("lastUpdated", static_cast<int64_t>(time(nullptr)), allocator);

	rapidjson::Value entries(rapidjson::kArrayType);

	std::unordered_map<NucleusID_t, std::string> nucToIp;
	nucToIp.reserve(m_banMetadataByIp.size());

	for (const auto& kv : m_banMetadataByIp)
	{
		const std::string& ip = kv.first;
		const BanMetadata_t& meta = kv.second;

		if (meta.m_NucleusID != 0 && !ip.empty())
		{
			nucToIp[meta.m_NucleusID] = ip;
		}
	}

	// ===============================
	// Write nucleus ID bans
	// ===============================
	for (const NucleusID_t id : m_bannedIdList)
	{
		const BanMetadata_t* pMeta = nullptr;

		auto it = m_banMetadataById.find(id);
		if (it != m_banMetadataById.end())
			pMeta = &it->second;

		rapidjson::Value entry(rapidjson::kObjectType);

		entry.AddMember("nucleusId", id, allocator);

		const char* ipOut = "";
		std::string ipFallback;

		if (pMeta && pMeta->m_IpAddress.Length() > 0)
		{
			ipOut = pMeta->m_IpAddress.Get();
		}
		else
		{
			auto ipIt = nucToIp.find(id);
			if (ipIt != nucToIp.end())
			{
				ipFallback = ipIt->second;
				ipOut = ipFallback.c_str();
			}
		}

		entry.AddMember(
			"ipAddress",
			rapidjson::Value(ipOut, allocator),
			allocator
		);

		entry.AddMember(
			"playerName",
			rapidjson::Value(
				pMeta ? pMeta->m_PlayerName.Get() : "Unknown",
				allocator
			),
			allocator
		);

		entry.AddMember(
			"banReason",
			rapidjson::Value(
				pMeta ? pMeta->m_BanReason.Get() : "",
				allocator
			),
			allocator
		);

		entry.AddMember(
			"banTimestamp",
			pMeta
			? pMeta->m_BanTimestamp
			: static_cast<int64_t>(time(nullptr)),
			allocator
		);

		entry.AddMember(
			"bannedByID",
			rapidjson::Value(
				pMeta ? pMeta->m_BannedByID.Get() : "",
				allocator
			),
			allocator
		);

		entry.AddMember("banExpiryTimestamp", 0, allocator); //todo
		entry.AddMember("banType", 0, allocator); //todo

		entries.PushBack(entry, allocator);
	}

	// ===============================
	// Write IP-only bans (ONLY when they do not already have a nucleus record)
	// ===============================
	for (const auto& ipWrapper : m_bannedIpList)
	{
		std::string ipStr = ConvertIpToString(&ipWrapper.adr);
		if (ipStr.empty())
			continue;

		auto it = m_banMetadataByIp.find(ipStr);
		const BanMetadata_t fallbackMeta(0, "Unknown", "", "00000000", ipStr.c_str(), time(nullptr));
		const BanMetadata_t& meta = it != m_banMetadataByIp.end() ? it->second : fallbackMeta;

		if (meta.m_NucleusID != 0 && m_bannedIdList.find(meta.m_NucleusID) != m_bannedIdList.end())
			continue;

		rapidjson::Value entry(rapidjson::kObjectType);

		entry.AddMember("nucleusId", meta.m_NucleusID, allocator);

		entry.AddMember(
			"ipAddress",
			rapidjson::Value(ipStr.c_str(), allocator),
			allocator
		);

		entry.AddMember(
			"playerName",
			rapidjson::Value(meta.m_PlayerName.Get(), allocator),
			allocator
		);

		entry.AddMember(
			"banReason",
			rapidjson::Value(meta.m_BanReason.Get(), allocator),
			allocator
		);

		entry.AddMember(
			"banTimestamp",
			meta.m_BanTimestamp,
			allocator
		);

		entry.AddMember(
			"bannedByID",
			rapidjson::Value(meta.m_BannedByID.Get(), allocator),
			allocator
		);

		entry.AddMember("banExpiryTimestamp", 0, allocator);
		entry.AddMember("banType", 0, allocator);

		entries.PushBack(entry, allocator);
	}

	document.AddMember("entries", entries, allocator);

	rapidjson::StringBuffer buffer;
	rapidjson::PrettyWriter< rapidjson::StringBuffer > writer(buffer);
	writer.SetIndent(' ', 2);
	document.Accept(writer);

	FileSystem()->Write(
		buffer.GetString(),
		buffer.GetSize(),
		pFile
	);

	FileSystem()->Close(pFile);
}


void CBanSystem::Clear()
{
	m_bannedIdList.clear();
	m_bannedIpList.clear();
	m_banMetadataById.clear();
	m_banMetadataByIp.clear();
}

//-----------------------------------------------------------------------------
// Purpose: adds a banned player entry to the banned list
// Input  : *ipAddress - 
//			nucleusId - 
//			playerName -
//			banReason -
//-----------------------------------------------------------------------------
bool CBanSystem::AddEntry(const netadr_t* const adr, const NucleusID_t nuc, const char* playerName, const char* bannedByID, const char* banReason)
{
	return AddEntry( adr ? adr->GetIP() : nullptr, nuc, playerName, bannedByID, banReason);
}

bool CBanSystem::AddEntry(const NucleusID_t nuc)
{
	return AddEntry(static_cast<const in6_addr*>(nullptr), nuc, "", "00000000", "");
}

bool CBanSystem::AddEntry(const in6_addr* const adr, const NucleusID_t nuc, const char* playerName, const char* bannedByID, const char* banReason)
{
	bool nucAdded = false;
	bool adrAdded = false;

	std::string ipStr;

	if (adr)
		ipStr = ConvertIpToString(adr);

	BanMetadata_t metadata
	(
		nuc,
		playerName ? playerName : "Unknown",
		banReason,
		bannedByID ? bannedByID : "Unknown",
		ipStr.c_str(),
		time(nullptr)
	);

	if (nuc)
	{
		nucAdded = m_bannedIdList.insert(nuc).second;
		m_banMetadataById[nuc] = metadata;
	}

	if (adr && !ipStr.empty())
	{
		adrAdded = m_bannedIpList.insert(adr).second;
		m_banMetadataByIp[ipStr] = metadata;
	}

	return nucAdded || adrAdded;
}


//-----------------------------------------------------------------------------
// Purpose: deletes an entry in the banned list
// Input  : *ipAddress - 
//			nucleusId - 
//-----------------------------------------------------------------------------
bool CBanSystem::DeleteEntry(const netadr_t* const adr, const NucleusID_t nuc)
{
	return DeleteEntry(adr ? adr->GetIP() : nullptr, nuc);
}

//-----------------------------------------------------------------------------
// Purpose: bans a player by nucleus id only, without banning their current IP
// Input  : *playerHandle -
//			*reason -
//-----------------------------------------------------------------------------
void CBanSystem::BanPlayerByNId(const char* playerHandle, const char* reason)
{
	if (!VALID_CHARSTAR(playerHandle))
		return;

	AuthorPlayerById(playerHandle, true, nullptr, reason, false, nullptr, true);
}

static bool BanSystem_ConvertAddress(const char* const address, in6_addr* const addr)
{
	const int ret = inet_pton(AF_INET6, address, addr);

	if (ret != 1)
	{
		Warning(eDLL_T::SERVER, "%s: Failed to convert provided network address \"%s\" (%s)\n",
			__FUNCTION__, address, ret == -1 ? NET_ErrorString(WSAGetLastError()) : "invalid format");
		return false;
	}

	return true;
}

bool CBanSystem::DeleteEntry(const in6_addr* const adr, const NucleusID_t nuc)
{
	bool bRemoved = false;

	if (nuc)
	{
		if (m_bannedIdList.erase(nuc) > 0)
			bRemoved = true;
	}

	std::string ipStr;

	if (adr)
	{
		ipStr = ConvertIpToString(adr);

		if (!ipStr.empty())
		{
			if (m_bannedIpList.erase(adr) > 0)
				bRemoved = true;
		}
	}

	BanMetadata_t metadata;
	bool hasMetadata = false;

	if (nuc)
	{
		auto it = m_banMetadataById.find(nuc);
		if (it != m_banMetadataById.end())
		{
			metadata = it->second;
			hasMetadata = true;
		}
	}

	if (!hasMetadata && !ipStr.empty())
	{
		auto it = m_banMetadataByIp.find(ipStr);
		if (it != m_banMetadataByIp.end())
		{
			metadata = it->second;
			hasMetadata = true;
		}
	}

	//pairs
	if (hasMetadata)
	{
		// paired nucleus
		if (metadata.m_NucleusID != 0)
		{
			m_bannedIdList.erase(metadata.m_NucleusID);
			m_banMetadataById.erase(metadata.m_NucleusID);
		}

		// paired ip
		if (metadata.m_IpAddress.Length() > 0)
		{
			in6_addr pairedAdr;

			if (BanSystem_ConvertAddress(
				metadata.m_IpAddress.Get(),
				&pairedAdr
			))
			{
				m_bannedIpList.erase(&pairedAdr);
				m_banMetadataByIp.erase(metadata.m_IpAddress.Get());
			}
		}
	}

	//meta
	if (nuc)
		m_banMetadataById.erase(nuc);

	if (!ipStr.empty())
		m_banMetadataByIp.erase(ipStr);

	return bRemoved;
}

//-----------------------------------------------------------------------------
// Purpose: checks if specified ip address or nucleus id is banned
// Input  : *ipAddress - 
//			nucleusId - 
// Output : true if banned, false if not banned
//-----------------------------------------------------------------------------
bool CBanSystem::IsBanned(const netadr_t* const adr, const NucleusID_t nuc) const
{
	return IsBanned(adr ? adr->GetIP() : nullptr, nuc);
}

bool CBanSystem::IsBanned(const in6_addr* const adr, const NucleusID_t nuc) const
{
	if (nuc && m_bannedIdList.find(nuc) != m_bannedIdList.end())
		return true;

	if (adr && m_bannedIpList.find(adr) != m_bannedIpList.end())
		return true;

	return false;
}

//-----------------------------------------------------------------------------
// Purpose: kicks a player by given name
// Input  : *playerName - 
//			*reason - 
//-----------------------------------------------------------------------------
void CBanSystem::KickPlayerByName(const char* playerName, const char* reason)
{
	if (!VALID_CHARSTAR(playerName))
		return;

	AuthorPlayerByName(playerName, false, nullptr, reason);
}

//-----------------------------------------------------------------------------
// Purpose: kicks a player by given handle or id
// Input  : *playerHandle - 
//			*reason - 
//-----------------------------------------------------------------------------
void CBanSystem::KickPlayerById(const char* playerHandle, const char* reason)
{
	if (!VALID_CHARSTAR(playerHandle))
		return;

	AuthorPlayerById(playerHandle, false, nullptr, reason);
}

//-----------------------------------------------------------------------------
// Purpose: bans a player by given name
// Input  : *playerName - 
//			*reason - 
//-----------------------------------------------------------------------------
void CBanSystem::BanPlayerByName(const char* playerName, const char* reason)
{
	BanPlayerByName(playerName, "00000000", reason);
}

void CBanSystem::BanPlayerByName(const char* playerName, const char* bannedByID, const char* reason)
{
	if (!VALID_CHARSTAR(playerName))
		return;

	AuthorPlayerByName(playerName, true, bannedByID, reason);
}

//-----------------------------------------------------------------------------
// Purpose: bans a player by given handle or id
// Input  : *playerHandle - 
//			*reason - 
//-----------------------------------------------------------------------------
void CBanSystem::BanPlayerById(const char* playerHandle, const char* reason)
{
	BanPlayerById(playerHandle, "00000000", reason, false);
}

void CBanSystem::BanPlayerById(const char* playerHandle, const char* bannedByID, const char* reason, bool bIdOnly )
{
	if (!VALID_CHARSTAR(playerHandle))
		return;

	AuthorPlayerById(playerHandle, true, bannedByID, reason, false, nullptr, bIdOnly);
}

//-----------------------------------------------------------------------------
// Purpose: unbans a player by given nucleus id or ip address
// Input  : *criteria - 
//-----------------------------------------------------------------------------
void CBanSystem::UnbanPlayer(const char* criteria)
{
	if (!VALID_CHARSTAR(criteria))
		return;

	NucleusID_t nTargetID = 0;
	in6_addr targetAddress = {};
	bool hasTargetAddress = false;

	if (V_IsAllDigit(criteria))
	{
		Bansystem_ValidateInputID(criteria, nTargetID);
	}
	else
	{
		netadr_t parsedAddress;
		if (parsedAddress.SetFromString(criteria, false))
		{
			targetAddress = *parsedAddress.GetIP();
			hasTargetAddress = true;
		}
		else
		{
			BanMetadata_t metadata;
			bool foundMetadata = false;

			for (const auto& entry : m_banMetadataById)
			{
				if (V_stricmp(entry.second.m_PlayerName.Get(), criteria) == 0)
				{
					metadata = entry.second;
					foundMetadata = true;
					break;
				}
			}

			if (!foundMetadata)
			{
				for (const auto& entry : m_banMetadataByIp)
				{
					if (V_stricmp(entry.second.m_PlayerName.Get(), criteria) == 0)
					{
						metadata = entry.second;
						foundMetadata = true;
						break;
					}
				}
			}

			if (foundMetadata)
			{
				nTargetID = metadata.m_NucleusID;

				if (metadata.m_IpAddress.Length() > 0 &&
					parsedAddress.SetFromString(metadata.m_IpAddress.Get(), false))
				{
					targetAddress = *parsedAddress.GetIP();
					hasTargetAddress = true;
				}
			}
		}
	}

	if (!nTargetID && !hasTargetAddress)
	{
		Warning(eDLL_T::SERVER, "No ban entry found matching '%s'\n", criteria);
		return;
	}

	if (DeleteEntry(hasTargetAddress ? &targetAddress : nullptr, nTargetID))
	{
		SaveList();
		Msg(eDLL_T::SERVER, "Removed '%s' from banned list\n", criteria);
		return;
	}

	Warning(eDLL_T::SERVER, "No ban entry found matching '%s'\n", criteria);
}

void CBanSystem::AddIdToBanlist(const char* playerHandle, const char* bannedByID, const char* reason, const netadr_t* const address  )
{
	AuthorPlayerById(playerHandle, true, bannedByID, reason, true, address);
}


//-----------------------------------------------------------------------------
// Purpose: authors player by given name
// Input  : *playerName - 
//			shouldBan   - (only kicks if false)
//			*reason     - 
//-----------------------------------------------------------------------------
void CBanSystem::AuthorPlayerByName(const char* playerName, const bool shouldBan, const char* bannedByID, const char* reason)
{
	Assert(VALID_CHARSTAR(playerName));
	bool bDisconnect = false;
	bool bSave = false;

	if (!reason)
		reason = shouldBan ? "Banned from server" : "Kicked from server";

	if (!bannedByID)
		bannedByID = "00000000";

	for (int i = 0; i < gpGlobals->maxClients; i++)
	{
		CClient* const pClient = g_pServer->GetClient(i);

		if (!pClient || !pClient->IsConnected())
			continue;

		const CNetChan* const pNetChan = pClient->GetNetChan();

		if (!pNetChan)
			continue;

		if (strlen(pNetChan->GetName()) > 0)
		{
			if (strcmp(playerName, pNetChan->GetName()) == NULL) // Our wanted name?
			{
				if (shouldBan && AddEntry(&pNetChan->GetRemoteAddress(), pClient->GetNucleusID(), playerName, bannedByID, reason) && !bSave)
					bSave = true;

				Warning(eDLL_T::SERVER, "Ban system disconnecting player name='%s' slot #%i handle=%i signon=%i uid='%llu' action=%s reason='%s'\n",
					pNetChan->GetName(), pClient->GetUserID(), pClient->GetHandle(), static_cast<int>(pClient->GetSignonState()),
					pClient->GetNucleusID(), shouldBan ? "ban" : "kick", reason);

				BanSystem_DisconnectClient(pClient, reason);
				bDisconnect = true;
			}
		}
	}

	if (bSave)
	{
		SaveList();
		Msg(eDLL_T::SERVER, "Added '%s' to banned list\n", playerName);
	}
	else if (!bDisconnect && shouldBan)
	{
		Msg(eDLL_T::SERVER, "Could not find player '%s' to ban\n", playerName);
	}
	else if (bDisconnect)
	{
		Msg(eDLL_T::SERVER, "Kicked '%s' from server\n", playerName);
	}
	else if(!shouldBan)
	{
		Msg(eDLL_T::SERVER, "Could not find player '%s' to kick\n", playerName);
	}
}

static bool BanSystem_CompareAddress(const in6_addr* const a, const in6_addr* const b)
{
	return IN6_ADDR_EQUAL(a, b);
}

bool CBanSystem::Bansystem_ValidateInputID(const char* str, NucleusID_t& out, int base)
{
	if (!str || !*str || *str == '-' || base < 2 || base > 36)
		return false;

	size_t start = (*str == '+') ? 1 : 0;
	if (!str[start])
		return false;

	const unsigned __int64 max_val = 18446744073709551615ULL;
	const unsigned __int64 max_safe = max_val / base;
	const unsigned __int64 max_digit = max_val % base;

	unsigned __int64 result = 0;

	for (size_t i = start; str[i]; ++i)
	{
		int digit;
		const unsigned char ch = static_cast<unsigned char>(str[i]);
		if (std::isdigit(ch))
		{
			digit = ch - '0';
		}
		else if (std::isalpha(ch))
		{
			digit = 10 + std::tolower(ch) - 'a';
		}
		else
			return false;

		if (digit >= base)
			return false;

		if (result > max_safe || (result == max_safe && digit > max_digit)) {
			return false;
		}

		result = result * base + digit;
	}

	out = result;
	return true;
}


//-----------------------------------------------------------------------------
// Purpose: authors player by given nucleus id or ip address
// Input  : *playerHandle - 
//			shouldBan     - (only kicks if false)
//			bannedByID	  - The player that banned them or server dummy value of: 00000000
//			*reason       - 
//			bool offline  - Whether this is a add-to-banlist entry rather than a player already in server
//-----------------------------------------------------------------------------
void CBanSystem::AuthorPlayerById(const char* playerHandle, const bool shouldBan, const char* bannedByID, const char* reason, const bool offline, const netadr_t* address, bool bIdOnly )
{
	Assert(VALID_CHARSTAR(playerHandle));

	bool bOnlyDigits = V_IsAllDigit(playerHandle);
	bool bDisconnect = false;
	bool bSave = false;

	in6_addr playerAdr;

	if (!bOnlyDigits)
	{
		if (!BanSystem_ConvertAddress(playerHandle, &playerAdr))
			return;
	}

	if (!reason)
		reason = shouldBan ? "Banned from server" : "Kicked from server";

	if (!bannedByID) //needs to be sent from panel if remote method
		bannedByID = "00000000";

	if (!offline)
	{
		for (int i = 0; i < gpGlobals->maxClients; i++)
		{
			CClient* const pClient = g_pServer->GetClient(i);

			if (!pClient || !pClient->IsConnected())
				continue;

			const CNetChan* const pNetChan = pClient->GetNetChan();

			if (!pNetChan)
				continue;

			if (bOnlyDigits)
			{
				char* pEnd = nullptr;
				const uint64_t nTargetID = strtoull(playerHandle, &pEnd, 10);
				if (nTargetID >= MAX_PLAYERS) // Is it a possible nucleusID?
				{
					const NucleusID_t nNucleusID = pClient->GetNucleusID();

					if (nNucleusID != nTargetID)
						continue;
				}
				else // If its not try by handle.
				{
					const edict_t nClientID = pClient->GetHandle();

					if (nClientID != nTargetID)
						continue;
				}

				if (shouldBan && AddEntry(bIdOnly ? nullptr : &pNetChan->GetRemoteAddress(), pClient->GetNucleusID(), pNetChan->GetName(), bannedByID, reason) && !bSave)
					bSave = true;

				Warning(eDLL_T::SERVER, "Ban system disconnecting player target='%s' slot #%i handle=%i signon=%i uid='%llu' action=%s reason='%s'\n",
					playerHandle, pClient->GetUserID(), pClient->GetHandle(), static_cast<int>(pClient->GetSignonState()),
					pClient->GetNucleusID(), shouldBan ? "ban" : "kick", reason);
				BanSystem_DisconnectClient(pClient, reason);
				bDisconnect = true;

				break;
			}
			else
			{
				if (!BanSystem_CompareAddress(pNetChan->GetRemoteAddress().GetIP(), &playerAdr))
					continue;

				if (shouldBan && AddEntry(bIdOnly ? nullptr : &pNetChan->GetRemoteAddress(), pClient->GetNucleusID(), pNetChan->GetName(), bannedByID, reason) && !bSave)
					bSave = true;

				Warning(eDLL_T::SERVER, "Ban system disconnecting player address='%s' slot #%i handle=%i signon=%i uid='%llu' action=%s reason='%s'\n",
					playerHandle, pClient->GetUserID(), pClient->GetHandle(), static_cast<int>(pClient->GetSignonState()),
					pClient->GetNucleusID(), shouldBan ? "ban" : "kick", reason);
				BanSystem_DisconnectClient(pClient, reason);
				bDisconnect = true;

				break;
			}
		}
	}
	else
	{
		NucleusID_t nuc;
		if (!Bansystem_ValidateInputID(playerHandle, nuc))
		{
			Error(eDLL_T::SERVER, NO_ERROR, "Invalid player handle passed to BanSystem.");
			return;
		}

		//attempt to save by input only.
		if (nuc && AddEntry(address, nuc, "Unknown", bannedByID, reason)) //potentially add setting username support. Tracker can look this up prior to sending.
			bSave = true;
		else
			Msg(eDLL_T::SERVER, "Could not add '%s' to banned list.", playerHandle);
	}

	if (bSave)
	{
		SaveList();
		Msg(eDLL_T::SERVER, "Added '%s' to banned list\n", playerHandle);
	}
	else if (!bDisconnect && shouldBan)
	{
		Msg(eDLL_T::SERVER, "Could not find player '%s' to ban\n", playerHandle);
	}
	else if (bDisconnect)
	{
		Msg(eDLL_T::SERVER, "Kicked '%s' from server\n", playerHandle);
	}
	else if (!shouldBan)
	{
		Msg(eDLL_T::SERVER, "Could not find player '%s' to kick\n", playerHandle);
	}
}

bool CBanSystem::IsBannedInMetaData(const char* criteria, const char* ipAddress, char* outContext)
{
	if (outContext)
		outContext[0] = '\0';

	auto WriteContext = [&](const char* where, const BanMetadata_t* meta)
		{
			if (!outContext)
				return;

			const char* reason = "Banned from server";
			if (meta && meta->m_BanReason.Length() > 0)
				reason = meta->m_BanReason.Get();

			V_snprintf(outContext, 256, "%s:%s", where ? where : "", reason ? reason : "");
		};

	if (VALID_CHARSTAR(criteria))
	{
		NucleusID_t nuc = 0;

		if (V_IsAllDigit(criteria) && Bansystem_ValidateInputID(criteria, nuc) && nuc != 0 && nuc >= MAX_PLAYERS)
		{
			auto itMeta = m_banMetadataById.find(nuc);
			if (itMeta != m_banMetadataById.end())
			{
				WriteContext("criteria_uid_meta", &itMeta->second);
				return true;
			}

			if (m_bannedIdList.find(nuc) != m_bannedIdList.end())
			{
				WriteContext("criteria_uid_list", nullptr);
				return true;
			}
		}
		else
		{
			netadr_t parsedCriteria;
			const in6_addr* pCriteriaIp = nullptr;
			std::string normalizedCriteriaIp;

			if (parsedCriteria.SetFromString(criteria, true))
			{
				pCriteriaIp = parsedCriteria.GetIP();

				if (pCriteriaIp)
					normalizedCriteriaIp = ConvertIpToString(pCriteriaIp);
			}

			if (!normalizedCriteriaIp.empty())
			{
				auto itMetaDirect = m_banMetadataByIp.find(criteria);
				if (itMetaDirect != m_banMetadataByIp.end())
				{
					WriteContext("criteria_ip_meta", &itMetaDirect->second);
					return true;
				}

				auto itMetaNorm = m_banMetadataByIp.find(normalizedCriteriaIp);
				if (itMetaNorm != m_banMetadataByIp.end())
				{
					WriteContext("criteria_ip_meta_norm", &itMetaNorm->second);
					return true;
				}

				if (pCriteriaIp && m_bannedIpList.find(pCriteriaIp) != m_bannedIpList.end())
				{
					WriteContext("criteria_ip_list", nullptr);
					return true;
				}
			}
			else
			{
				for (const auto& kv : m_banMetadataById)
				{
					const BanMetadata_t& meta = kv.second;

					if (meta.m_PlayerName.Length() <= 0)
						continue;

					if (V_stricmp(meta.m_PlayerName.Get(), criteria) != 0)
						continue;

					WriteContext("criteria_name_meta_id", &meta);
					return true;
				}

				for (const auto& kv : m_banMetadataByIp)
				{
					const BanMetadata_t& meta = kv.second;

					if (meta.m_PlayerName.Length() <= 0)
						continue;

					if (V_stricmp(meta.m_PlayerName.Get(), criteria) != 0)
						continue;

					WriteContext("criteria_name_meta_ip", &meta);
					return true;
				}
			}
		}
	}

	if (VALID_CHARSTAR(ipAddress))
	{
		auto itMetaDirect = m_banMetadataByIp.find(ipAddress);
		if (itMetaDirect != m_banMetadataByIp.end())
		{
			WriteContext("ip_meta", &itMetaDirect->second);
			return true;
		}

		netadr_t parsed;
		const in6_addr* pIp = nullptr;
		std::string normalizedIp;

		if (parsed.SetFromString(ipAddress, true))
		{
			pIp = parsed.GetIP();

			if (pIp)
				normalizedIp = ConvertIpToString(pIp);
		}

		if (!normalizedIp.empty())
		{
			auto itMetaNorm = m_banMetadataByIp.find(normalizedIp);
			if (itMetaNorm != m_banMetadataByIp.end())
			{
				WriteContext("ip_meta_norm", &itMetaNorm->second);
				return true;
			}
		}

		if (pIp && m_bannedIpList.find(pIp) != m_bannedIpList.end())
		{
			WriteContext("ip_list", nullptr);
			return true;
		}
	}

	return false;
}

int CBanSystem::FindPlayerInServerIndex(const char* criteria)
{
	if (!VALID_CHARSTAR(criteria))
		return -1;

	bool bOnlyDigits = V_IsAllDigit(criteria);

	uint64_t nTargetID = 0;

	if (bOnlyDigits)
	{
		char* pEnd = nullptr;
		nTargetID = strtoull(criteria, &pEnd, 10);

		if (pEnd == criteria)
			bOnlyDigits = false;
	}

	for (int i = 0; i < gpGlobals->maxClients; i++)
	{
		CClient* const pClient = g_pServer->GetClient(i);

		if (!pClient || !pClient->IsConnected())
			continue;

		const CNetChan* const pNetChan = pClient->GetNetChan();
		if (!pNetChan)
			continue;

		if (bOnlyDigits)
		{
			if (nTargetID >= MAX_PLAYERS) // treat as nucleus id
			{
				const NucleusID_t nNucleusID = pClient->GetNucleusID();

				if (nNucleusID == nTargetID)
					return i;
			}
			else // treat as client handle
			{
				const edict_t nClientID = pClient->GetHandle();

				if (nClientID == nTargetID)
					return i;
			}
		}
		else // I don't actually like this, as player names can be all numbers.. but for now.
		{
			const char* clientName = pNetChan->GetName();

			if (VALID_CHARSTAR(clientName) && V_stricmp(clientName, criteria) == 0)
				return i;
		}
	}

	return -1;
}


bool CBanSystem::IsPlayerInServer(const char* criteria)
{
	return FindPlayerInServerIndex(criteria) != -1;
}

bool CBanSystem::TextBanPlayer(const char* const pszCriteria, const char* const pszReason, const char* const pszExpiry, const char* const pszMutedBy, bool toggle, bool bRemoteCommand, int expiryUnixTimestamp)
{
	int idx = FindPlayerInServerIndex(pszCriteria);

	if (idx < 0)
		return false;

	CClient* const pClient = g_pServer->GetClient(idx);
	if (!pClient || !pClient->IsConnected())
		return false;

	CClientExtended* const pClientExtended = pClient->GetClientExtended();
	if (!pClientExtended)
		return false;

	pClientExtended->SetClientIsCommsBanned(toggle);

	if (toggle)
		pClientExtended->SetCommsBanInfo(pszReason, pszExpiry);


	bool bServerMute = pszMutedBy && strcmp(pszMutedBy, "SERVER") == 0;
	if (!bRemoteCommand && !bServerMute)
		TrackerSocketSystem()->RelayChatMute(pClient->GetClientName(), pClient->GetNucleusID(), pszReason, pszExpiry, pszMutedBy, toggle, expiryUnixTimestamp);

	return true;
}


///////////////////////////////////////////////////////////////////////////////
// Console command handlers
///////////////////////////////////////////////////////////////////////////////

enum KickType_e
{
	KICK_NAME = 0,
	KICK_ID,
	BAN_NAME,
	BAN_ID,
	BAN_NID
};

static void _Author_Client_f(const CCommand& args, const KickType_e type)
{
	if (args.ArgC() < 2)
	{
		return;
	}

	const char* szReason = args.ArgC() > 2 ? args.Arg(2) : nullptr;

	switch (type)
	{
		case KICK_NAME:
		{
			g_BanSystem.KickPlayerByName(args.Arg(1), szReason);
			break;
		}
		case KICK_ID:
		{
			g_BanSystem.KickPlayerById(args.Arg(1), szReason);
			break;
		}
		case BAN_NAME:
		{
			g_BanSystem.BanPlayerByName(args.Arg(1), "00000000", szReason);
			break;
		}
		case BAN_ID:
		{
			g_BanSystem.BanPlayerById(args.Arg(1), "00000000", szReason);
			break;
		}
		case BAN_NID:
		{
			g_BanSystem.BanPlayerByNId(args.Arg(1), szReason);
			break;
		}
		default:
		{
			Assert(0);
		}
	}
}
static void Host_Kick_f(const CCommand& args)
{
	_Author_Client_f(args, KickType_e::KICK_NAME);
}
static void Host_KickID_f(const CCommand& args)
{
	_Author_Client_f(args, KickType_e::KICK_ID);
}
static void Host_Ban_f(const CCommand& args)
{
	_Author_Client_f(args, KickType_e::BAN_NAME);
}
static void Host_BanID_f(const CCommand& args)
{
	_Author_Client_f(args, KickType_e::BAN_ID);
}
static void Host_BanNID_f(const CCommand& args)
{
	_Author_Client_f(args, KickType_e::BAN_NID);
}
static void Host_Unban_f(const CCommand& args)
{
	if (args.ArgC() < 2)
	{
		return;
	}

	g_BanSystem.UnbanPlayer(args.Arg(1));
}
static void Host_ReloadBanList_f()
{
	g_BanSystem.Clear();
	g_BanSystem.LoadList();
}

static ConCommand kick("kick", Host_Kick_f, "Kick a client from the server by user name", FCVAR_RELEASE, nullptr, "kick \"<userId>\"");
static ConCommand kickid("kickid", Host_KickID_f, "Kick a client from the server by handle, nucleus id or ip address", FCVAR_RELEASE, nullptr, "kickid \"<handle>\"/\"<nucleusId>/<ipAddress>\"");
static ConCommand ban("ban", Host_Ban_f, "Bans a client from the server by user name", FCVAR_RELEASE, nullptr, "ban <userId>");
static ConCommand banid("banid", Host_BanID_f, "Bans a client from the server by handle, nucleus id or ip address", FCVAR_RELEASE, nullptr, "banid \"<handle>\"/\"<nucleusId>/<ipAddress>\"");
static ConCommand bannid("bannid", Host_BanNID_f, "Bans a client from the server by nucleus id without banning their IP address", FCVAR_RELEASE, nullptr, "bannid \"<nucleusId>\"");
static ConCommand unban("unban", Host_Unban_f, "Unbans a client from the server by nucleus id or ip address", FCVAR_RELEASE, nullptr, "unban \"<nucleusId>\"/\"<ipAddress>\"");
static ConCommand reload_banlist("banlist_reload", Host_ReloadBanList_f, "Reloads the banned list", FCVAR_RELEASE);

///////////////////////////////////////////////////////////////////////////////
CBanSystem g_BanSystem;
