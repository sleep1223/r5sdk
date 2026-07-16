#pragma once
#include "tier1/NetAdr.h"
#include "tier2/netadrutils.h"
#include "ebisusdk/EbisuTypes.h"

class CBanSystem
{
public:
	struct Banned_t
	{
		enum BanType_e
		{
			CONNECT = 0,
			COMMUNICATION
		};

		Banned_t(const char* ipAddress = "", NucleusID_t nucleusId = NULL, BanType_e banType = BanType_e::CONNECT, const char* const banExpiry = nullptr)
			: m_Address(ipAddress)
			, m_NucleusID(nucleusId)
			, m_BanType(banType)
			, m_BanExpiry(banExpiry)
		{}

		inline bool operator==(const Banned_t& other) const
		{
			return m_NucleusID == other.m_NucleusID
				&& m_Address.IsEqual_CaseInsensitive(other.m_Address);
		}

		NucleusID_t m_NucleusID;
		CUtlString m_Address;
		BanType_e m_BanType;
		CUtlString m_BanExpiry;
	};

	typedef CUtlVector<Banned_t> BannedList_t;

	struct BanMetadata_t
	{
		NucleusID_t m_NucleusID;
		CUtlString m_PlayerName;
		CUtlString m_BanReason;
		CUtlString m_BannedByID;
		int64_t m_BanTimestamp;
		CUtlString m_IpAddress;

		BanMetadata_t(NucleusID_t nucleusID = 0, const char* playerName = "", const char* banReason = "", const char* bannedByID = "00000000", const char* ipAddress = nullptr, int64_t timestamp = 0)
			: m_NucleusID(nucleusID)
			, m_PlayerName(playerName ? playerName : "")
			, m_BanReason(banReason ? banReason : "")
			, m_BannedByID(bannedByID ? bannedByID : "")
			, m_BanTimestamp(timestamp)
			, m_IpAddress(ipAddress ? ipAddress : "")
		{}
	};

public:
	void LoadList(void);
	void SaveList(void) const;

	void Clear();

	bool AddEntry(const netadr_t* const adr, const NucleusID_t nuc, const char* playerName = "", const char* bannedByID = "00000000", const char* banReason = "");
	bool AddEntry(const NucleusID_t nuc);
	bool DeleteEntry(const netadr_t* const adr, const NucleusID_t nuc);

	bool IsBanned(const netadr_t* const adr, const NucleusID_t nuc) const;
	bool IsBannedInMetaData(const char* criteria, const char* ipAddress = nullptr, char* outReason = nullptr); //usage is primarily for admin purposes, not for player lookups.
	bool IsPlayerInServer(const char* criteria);
	int FindPlayerInServerIndex(const char* criteria);
	bool TextBanPlayer(const char* const pszCriteria, const char* const pszReason, const char* const pszExpiry, const char* const mutedBy, bool toggle, bool bRemoteCommand = false, int expiryUnixTimestamp = 0);

	void KickPlayerByName(const char* playerName, const char* reason = nullptr);
	void KickPlayerById(const char* playerHandle, const char* reason = nullptr);

	void BanPlayerByName(const char* playerName, const char* reason = nullptr);
	void BanPlayerById(const char* playerHandle, const char* reason = nullptr);
	void BanPlayerByNId(const char* playerHandle, const char* reason = nullptr);
	void BanPlayerByName(const char* playerName, const char* bannedByID, const char* reason);
	void BanPlayerById(const char* playerHandle, const char* bannedByID, const char* reason, bool bIdOnly = false);
	void AddIdToBanlist(const char* playerHandle, const char* bannedByID, const char* reason = nullptr, const netadr_t* const adr = nullptr );

	void UnbanPlayer(const char* criteria);

	bool Bansystem_ValidateInputID(const char* str, NucleusID_t& out, int base = 10);

private:
	bool AddEntry(const in6_addr* const adr, const NucleusID_t nuc, const char* playerName = "", const char* bannedByID = "00000000", const char* banReason = "");
	bool DeleteEntry(const in6_addr* const adr, const NucleusID_t nuc);

	bool IsBanned(const in6_addr* const adr, const NucleusID_t nuc) const;

	void AuthorPlayerByName(const char* playerName, const bool bBan, const char* bannedByID = nullptr, const char* reason = nullptr);
	void AuthorPlayerById(const char* playerHandle, const bool bBan, const char* bannedByID = nullptr, const char* reason = nullptr, const bool offline = false, const netadr_t* address = nullptr, bool bIdOnly = false);

	std::string ConvertIpToString(const in6_addr* const adr) const;

	std::unordered_set<NucleusID_t> m_bannedIdList;
	std::unordered_set<IPv6Wrapper_s, IPv6Hasher_s> m_bannedIpList;
	std::unordered_map<NucleusID_t, BanMetadata_t> m_banMetadataById;
	std::unordered_map<std::string, BanMetadata_t> m_banMetadataByIp;
};

extern CBanSystem g_BanSystem;
