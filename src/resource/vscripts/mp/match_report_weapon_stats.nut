untyped

#if SERVER
global function MatchReportWeaponStats_Init
global function MatchReportWeaponStats_IsInitialized
global function MatchReportWeaponStats_DebugNote
global function MatchReportWeaponStats_RecordPlayerKilled
global function MatchReportWeaponStats_RecordDamageDoneToPlayerForWeapon
global function MatchReportWeaponStats_RecordWeaponAttack
global function MatchReportWeaponStats_RecordWeaponShot
global function MatchReportWeaponStats_RecordWeaponHit

bool matchReportWeaponStatsInitialized = false
bool matchReportWeaponStatsKillLogged = false
bool matchReportWeaponStatsAttackLogged = false

void function MatchReportWeaponStats_DebugNote( string message )
{
	MatchReport_DebugNote( message )
}

void function MatchReportWeaponStats_Init()
{
	if ( matchReportWeaponStatsInitialized )
		return

	matchReportWeaponStatsInitialized = true
	MatchReportWeaponStats_DebugNote( "MatchReportWeaponStats_Init helper-only" )
}

bool function MatchReportWeaponStats_IsInitialized()
{
	return matchReportWeaponStatsInitialized
}

void function MatchReportWeaponStats_OnWeaponAttack( entity player, entity weapon, string weaponRef, int ammoUsed, vector origin, vector dir )
{
	if ( !matchReportWeaponStatsAttackLogged )
	{
		matchReportWeaponStatsAttackLogged = true
		MatchReportWeaponStats_DebugNote( "MatchReportWeaponStats_OnWeaponAttack" )
	}

	MatchReportWeaponStats_RecordWeaponAttack( player, weapon, weaponRef, ammoUsed, origin, dir )
}

void function MatchReportWeaponStats_RecordWeaponAttack( entity player, entity weapon, string weaponRef, int ammoUsed, vector origin, vector dir )
{
	if ( !IsValid( player ) || !player.IsPlayer() )
		return

	string weaponName = weaponRef
	if ( weaponName == "" && IsValid( weapon ) )
		weaponName = weapon.GetWeaponClassName()

	if ( weaponName == "" )
		return

	int shots = ammoUsed
	if ( shots <= 0 )
		shots = 1

	MatchReportWeaponStats_RecordWeaponShot( player, weaponName, shots )
}

void function MatchReportWeaponStats_OnPlayerKilled( entity victim, entity attacker, var damageInfo )
{
	if ( !matchReportWeaponStatsKillLogged )
	{
		matchReportWeaponStatsKillLogged = true
		MatchReportWeaponStats_DebugNote( "MatchReportWeaponStats_OnPlayerKilled" )
	}

	MatchReportWeaponStats_RecordPlayerKilled( victim, attacker, damageInfo )
}

void function MatchReportWeaponStats_RecordPlayerKilled( entity victim, entity attacker, var damageInfo )
{
	if ( !IsValid( victim ) || !victim.IsPlayer() )
		return

	if ( !IsValid( attacker ) || !attacker.IsPlayer() )
		return

	if ( attacker == victim )
		return

	if ( !damageInfo )
		return

	string weaponName = MatchReportWeaponStats_GetWeaponName( damageInfo )
	if ( weaponName == "" )
		return

	MatchReportWeaponStats_RecordPlayerKill( attacker, victim, weaponName, DamageInfo_GetDamageSourceIdentifier( damageInfo ) )
}

void function MatchReportWeaponStats_RecordDamageDoneToPlayerForWeapon( entity attacker, string weaponName, float damage, bool isHeadshot )
{
	if ( !IsValid( attacker ) || !attacker.IsPlayer() )
		return

	if ( weaponName == "" )
		return

	if ( damage <= 0.0 )
		return

	MatchReportWeaponStats_RecordWeaponHit( attacker, weaponName, damage, 1, 1.0, isHeadshot ? 1 : 0 )
}

void function MatchReportWeaponStats_RecordPlayerKill( entity attacker, entity victim, string weaponName, int damageSourceId )
{
	string attackerUid = MatchReportWeaponStats_GetPlayerUid( attacker )
	if ( attackerUid == "" )
		return

	string attackerName = MatchReportWeaponStats_GetPlayerName( attacker )
	string victimUid = MatchReportWeaponStats_GetPlayerUid( victim )
	if ( victimUid == "" )
	{
		MatchReport_RecordWeaponKillByUid( attackerUid, attackerName, weaponName )
		return
	}

	MatchReport_RecordPlayerKillByUid(
		attackerUid,
		attackerName,
		victimUid,
		MatchReportWeaponStats_GetPlayerName( victim ),
		weaponName,
		damageSourceId
	)
}

void function MatchReportWeaponStats_RecordWeaponKill( entity attacker, string weaponName )
{
	string attackerUid = MatchReportWeaponStats_GetPlayerUid( attacker )
	if ( attackerUid == "" )
		return

	MatchReport_RecordWeaponKillByUid( attackerUid, MatchReportWeaponStats_GetPlayerName( attacker ), weaponName )
}

void function MatchReportWeaponStats_RecordWeaponShot( entity player, string weaponName, int shots )
{
	string playerUid = MatchReportWeaponStats_GetPlayerUid( player )
	if ( playerUid == "" )
		return

	MatchReport_RecordWeaponShotByUid( playerUid, MatchReportWeaponStats_GetPlayerName( player ), weaponName, shots )
}

void function MatchReportWeaponStats_RecordWeaponHit( entity attacker, string weaponName, float damage, int hits, float bulletsHit, int headshots )
{
	string attackerUid = MatchReportWeaponStats_GetPlayerUid( attacker )
	if ( attackerUid == "" )
		return

	MatchReport_RecordWeaponHitByUid( attackerUid, MatchReportWeaponStats_GetPlayerName( attacker ), weaponName, damage, hits, bulletsHit, headshots )
}

string function MatchReportWeaponStats_GetPlayerUid( entity player )
{
	if ( !IsValid( player ) || !player.IsPlayer() )
		return ""

	return player.GetPlatformUID()
}

string function MatchReportWeaponStats_GetPlayerName( entity player )
{
	if ( !IsValid( player ) || !player.IsPlayer() )
		return ""

	return player.GetPlayerName()
}

string function MatchReportWeaponStats_GetWeaponName( var damageInfo )
{
	entity weapon = DamageInfo_GetWeapon( damageInfo )
	if ( IsValid( weapon ) )
		return weapon.GetWeaponClassName()

	entity inflictor = DamageInfo_GetInflictor( damageInfo )
	if ( IsValid( inflictor ) )
		return inflictor.GetClassName()

	string damageSourceName = DamageSourceIDToString( DamageInfo_GetDamageSourceIdentifier( damageInfo ) )
	if ( damageSourceName != "" )
		return damageSourceName

	return "unknown"
}
#endif
