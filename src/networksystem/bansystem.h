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

public:
	void LoadList(void);
	void SaveList(void) const;

	void Clear();

	bool AddEntry(const netadr_t* const adr, const NucleusID_t nuc);
	bool AddEntry(const NucleusID_t nuc);
	bool DeleteEntry(const netadr_t* const adr, const NucleusID_t nuc);

	bool IsBanned(const netadr_t* const adr, const NucleusID_t nuc) const;

	void KickPlayerByName(const char* playerName, const char* reason = nullptr);
	void KickPlayerById(const char* playerHandle, const char* reason = nullptr);

	void BanPlayerByName(const char* playerName, const char* reason = nullptr);
	void BanPlayerById(const char* playerHandle, const char* reason = nullptr);
	void BanPlayerByNId(const char* playerHandle, const char* reason = nullptr);

	void UnbanPlayer(const char* criteria);

private:
	bool AddEntry(const in6_addr* const adr, const NucleusID_t nuc);
	bool DeleteEntry(const in6_addr* const adr, const NucleusID_t nuc);

	bool IsBanned(const in6_addr* const adr, const NucleusID_t nuc) const;

	void AuthorPlayerByName(const char* playerName, const bool bBan, const char* reason = nullptr);
	void AuthorPlayerById(const char* playerHandle, const bool bBan, const char* reason = nullptr);
	void AuthorPlayerByNId(const char* playerHandle, const bool bBan, const char* reason = nullptr);

	std::unordered_set<NucleusID_t> m_bannedIdList;
	std::unordered_set<IPv6Wrapper_s, IPv6Hasher_s> m_bannedIpList;
};

extern CBanSystem g_BanSystem;
