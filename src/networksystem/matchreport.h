#pragma once

#include "ebisusdk/EbisuTypes.h"

class CServer;
class CPlayer;

void SV_AppendMatchReportConfigSummary(string& outSummary);
void SV_MatchReportDebugNote(const char* const pszMessage);
void SV_TouchMatchReportPlayerIdentity(const NucleusID_t nNucleusID, const char* const pszPlayerName);
void SV_TouchMatchReportPlayer(CPlayer* const pPlayer);
void SV_ReportMatchEndData(CServer* const pServer);
void SV_RecordMatchReportWeaponKill(CPlayer* const pPlayer, const char* const pszWeaponName);
void SV_RecordMatchReportPlayerKill(CPlayer* const pAttacker, const char* const pszVictimUid, const char* const pszVictimName, const char* const pszWeaponName, const int nDamageSourceId);
void SV_RecordMatchReportWeaponShot(CPlayer* const pPlayer, const char* const pszWeaponName, const int nShots);
void SV_RecordMatchReportWeaponHit(CPlayer* const pPlayer, const char* const pszWeaponName, const float flDamage, const int nHits, const float flBulletsHit, const int nHeadshots);
void SV_RecordMatchReportWeaponKillByUid(const char* const pszPlayerUid, const char* const pszPlayerName, const char* const pszWeaponName);
void SV_RecordMatchReportPlayerKillByUid(const char* const pszAttackerUid, const char* const pszAttackerName, const char* const pszVictimUid, const char* const pszVictimName, const char* const pszWeaponName, const int nDamageSourceId);
void SV_RecordMatchReportWeaponShotByUid(const char* const pszPlayerUid, const char* const pszPlayerName, const char* const pszWeaponName, const int nShots);
void SV_RecordMatchReportWeaponHitByUid(const char* const pszPlayerUid, const char* const pszPlayerName, const char* const pszWeaponName, const float flDamage, const int nHits, const float flBulletsHit, const int nHeadshots);
