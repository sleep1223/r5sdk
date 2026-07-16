global function TimeoutInit																							//mkos
global function Timeout_IsEnabled
global function Timeout_IsPlayerTimedOut
global function Timeout_IsUIDTimedOut
global function Timeout_GetTimeoutPlayers
global function Timeout_SetPlayerTimedOut
global function Timeout_PrintTimeoutData
global function Timeout_GetDefaultTimeoutAmount
global function Timeout_GetTimeoutExpiresTimestamp
global function Timeout_SetupPlayerDataCallbacks

global function CodeCallback_TimeoutCommand
global function AddCallback_TimedOut

struct TimeoutData
{
	int iTimeoutExpiresTimestamp
	int iTimeoutAmount
	string sReason
	string sByPlayer
}

struct
{
	table< string, TimeoutData > m_timedoutPlayers
	array< void functionref( entity, bool ) > m_timeoutCallbacks

} file

struct
{
	bool bIsTimeoutEnabled
	int iDefaultiTimeoutAmount
	string sTimeoutExpiredMessage
	string sTimeoutMessage
	string sByPlayer

} settings

void function TimeoutInit() //✓
{
	RegisterSignal( "NewTimeout" )

	settings.bIsTimeoutEnabled			= GetCurrentPlaylistVarBool( "timeout_enable", true )
	settings.iDefaultiTimeoutAmount		= GetCurrentPlaylistVarInt( "timeout_default_time", 120 )
	settings.sTimeoutMessage			= GetCurrentPlaylistVarString( "timeout_message", "#player, you've been timedout by #admin.\nReason: #reason. \nExpires in: #expires" )
	settings.sTimeoutExpiredMessage		= GetCurrentPlaylistVarString( "timeout_expired_message", "#player, your timeout has expired." )

	if( settings.bIsTimeoutEnabled )
		AddCallback_OnClientConnected( __CheckIsTimedOut )
}

bool function Timeout_IsEnabled()//✓
{
	return settings.bIsTimeoutEnabled
}

void function AddCallback_TimedOut( void functionref( entity, bool ) callbackFunc )//✓
{
	if( file.m_timeoutCallbacks.contains( callbackFunc ) )
		mAssert( 0, "Tried to add callback func %s but was already added with %s()", string( callbackFunc ), FUNC_NAME() )

	file.m_timeoutCallbacks.append( callbackFunc )
}

void function __RunCallbacks( entity player, bool toggle )//✓
{
	foreach( callbackFunc in file.m_timeoutCallbacks )
		callbackFunc( player, toggle )
}

void function __CheckIsTimedOut( entity player ) //✓
{
	if( !IsValid( player ) )
		return

	if( !Timeout_IsPlayerTimedOut( player ) )
		return

	int untimeoutTimestamp = __GetTimeoutExpiresTimestamp( player )
	if( untimeoutTimestamp <= GetUnixTimestamp() )
	{
		Timeout_SetPlayerTimedOut( player, false )
		return
	}

	if( IsTimeInSameMatch( untimeoutTimestamp ) )
		thread __AutoUnTimeoutPlayer( player )
}

void function __AutoUnTimeoutPlayer( entity player ) //✓
{
	if( !IsValid( player ) )
		return

	player.EndSignal( "OnDestroy", "OnDisconnected", "NewTimeout" )
	int timeoutExpiresAt = __GetTimeoutExpiresTimestamp( player )
	int currentTime = GetUnixTimestamp()

	for( ; ; )
	{
		wait maxint( 0, timeoutExpiresAt - currentTime )
		Timeout_SetPlayerTimedOut( player, false )
	}
}

int function Timeout_GetTimeoutExpiresTimestamp( entity player )
{
	int currentTimestamp = GetUnixTimestamp()

	if( !IsValid( player ) || !Timeout_IsPlayerTimedOut( player ))
		return currentTimestamp

	return __GetTimeoutExpiresTimestamp( player )
}

int function __GetTimeoutExpiresTimestamp( entity player ) //✓
{
	return file.m_timedoutPlayers[ player.p.UID ].iTimeoutExpiresTimestamp
}

bool function Timeout_SetPlayerTimedOut( entity player, bool toggle = true, string sByPlayer = "", int iTimeoutAmount = -1, string reason = "" ) //✓
{
	if( !IsValid( player ) || !settings.bIsTimeoutEnabled )
		return false

	string formatMessage
	if( !toggle )
	{
		if( !player.p.bIsTimedOut )
			return false

		formatMessage = ResolveFormattersForPlayerMessage( player, settings.sTimeoutExpiredMessage )
		SendServerMessageToPlayer( player, formatMessage, false )

		delete file.m_timedoutPlayers[ player.p.UID ]
		player.p.bIsTimedOut = false

		__RunCallbacks( player, false )
		SetTimeoutPersistence( player )
		return true
	}

	player.Signal( "NewTimeout" )

	iTimeoutAmount = iTimeoutAmount > 0 ? iTimeoutAmount : settings.iDefaultiTimeoutAmount

	TimeoutData timeoutData

	timeoutData.iTimeoutExpiresTimestamp 	= GetUnixTimestamp() + iTimeoutAmount
	timeoutData.iTimeoutAmount				= iTimeoutAmount
	timeoutData.sReason						= reason != "" ? reason : "{empty}"
	timeoutData.sByPlayer					= sByPlayer != "" ? sByPlayer : "{empty}"

	file.m_timedoutPlayers[ player.p.UID ] <- timeoutData

	formatMessage = ResolveFormattersForPlayerMessage( player, settings.sTimeoutMessage )
	SendServerMessageToPlayer( player, formatMessage, true )

	player.p.bIsTimedOut = true
	SetTimeoutPersistence
	(
		player,
		timeoutData.iTimeoutExpiresTimestamp,
		timeoutData.sReason,
		timeoutData.sByPlayer
	)

	if( iTimeoutAmount > 0 )
		__CheckIsTimedOut( player ) //make sure we run the checker again

	__RunCallbacks( player, true )

	return true
}

string function Timeout_PrintTimeoutData( entity player )//✓
{
	if( !Timeout_IsPlayerTimedOut( player ) )
		return format( "%s is not timed out.", player.p.name )

	TimeoutData td = __GetTimedOutPlayer( player )

	string printData
	{
		printData = format
		(
			"Expiration: %s\nTimeout Amount: %d\nReason: %s, Timeout By: %s",
			Chat_ReadableExpiresTime( td.iTimeoutExpiresTimestamp ),
			td.iTimeoutAmount,
			td.sReason,
			td.sByPlayer
		)
	}

	return printData
}

int function Timeout_GetDefaultTimeoutAmount()
{
	return settings.iDefaultiTimeoutAmount
}

TimeoutData function __GetTimedOutPlayer( entity player )//✓
{
	return file.m_timedoutPlayers[ player.p.UID ]
}

bool function IsTimeInSameMatch( int timestamp )//✓
{
	int roundTime
	if( IsFlowstateActive() )
		roundTime = FlowState_RoundTime() * Flowstate_AutoChangeLevelRounds()
	else
		roundTime = GetCurrentPlaylistVarInt( "round_time", 30 ) * 60

	if( Tracker_GetStartUnixTime() + roundTime > timestamp )
		return true

	return false
}

bool function Timeout_IsPlayerTimedOut( entity player )//✓
{
	return ( player.p.UID in file.m_timedoutPlayers )
}

bool function Timeout_IsUIDTimedOut( string uid )//✓
{
	return ( uid in file.m_timedoutPlayers )
}

table< string, TimeoutData > function Timeout_GetTimeoutPlayers()//✓
{
	return file.m_timedoutPlayers
}

string function GetFormatterValueForPlayer( entity player, string formatter )//✓
{
	switch( formatter )
	{
		case "#player":
			return player.GetPlayerName()

		case "#uid":
			return player.GetPlatformUID()

		case "#ping":
			return ( player.GetLatency() * 1000 ).tostring()

		case "#expires":
			return Chat_ReadableExpiresTime( __GetTimeoutExpiresTimestamp( player ) )

		case "#reason":
			return __GetTimedOutPlayer( player ).sReason

		case "#amount":
			return __GetTimedOutPlayer( player ).iTimeoutAmount.tostring()

		case "#admin":
			return __GetTimedOutPlayer( player ).sByPlayer

		default:
			return formatter
	}

	unreachable
}

string function ResolveFormattersForPlayerMessage( entity player, string message )//✓
{
	return RegexpReplaceFuncPlayer
	(
		message,
		"#[A-Za-z0-9_]+",

		string function( array<string> captures, entity player = null )
		{
			return GetFormatterValueForPlayer( player, captures[ 0 ] )
		},
		player
	)
}

void function CodeCallback_TimeoutCommand( bool toggle, string criteria, int iTimeoutAmount, string fromWebPanelUser, string reason )//✓
{
	entity candidate = GetPlayer( criteria )
	if( !IsValid( candidate ) )
		return

	Timeout_SetPlayerTimedOut( candidate, toggle, fromWebPanelUser, iTimeoutAmount, reason )
}

void function Timeout_SetupPlayerDataCallbacks()
{
	#if TRACKER
		Tracker_RegisterPlayerData( "timeout_expires_timestamp" )
		Tracker_RegisterPlayerData( "timeout_reason" )
		Tracker_RegisterPlayerData( "timeout_issuer_name" )

		AddCallback_PlayerDataFullyLoaded( __CheckTimeoutPersistence )
	#endif
}

void function SetTimeoutPersistence( entity player, int expiresTimestamp = 0, string reason = "", string issuerName = "" )
{
	#if TRACKER
		string uid = player.p.UID

		Tracker_SavePlayerData( uid, "timeout_expires_timestamp", expiresTimestamp )
		Tracker_SavePlayerData( uid, "timeout_reason", reason )
		Tracker_SavePlayerData( uid, "timeout_issuer_name", issuerName )
	#endif
}

void function __CheckTimeoutPersistence( entity player )
{
	#if TRACKER
		string uid 										= player.p.UID
		string persistentExpiresTimestampString 		= Tracker_FetchPlayerData( uid, "timeout_expires_timestamp" )

		if( persistentExpiresTimestampString == "" )
			return

		if( !IsStringNumber( persistentExpiresTimestampString ) )
		{
			#if DEVELOPER
				printt( "[Timeout] Invalid upstream data for expires timestamp:", persistentExpiresTimestampString )
			#endif

			return
		}

		int persistentExpiresTimestamp 	= persistentExpiresTimestampString.tointeger()
		int currentTimestamp 			= GetUnixTimestamp()

		if( persistentExpiresTimestamp > currentTimestamp )
		{
			string issuerName 	= Tracker_FetchPlayerData( uid, "timeout_issuer_name" )
			string reason		= Tracker_FetchPlayerData( uid, "timeout_reason" )

			Timeout_SetPlayerTimedOut
			(
				player,
				true,
				issuerName,
				persistentExpiresTimestamp - currentTimestamp,
				reason
			)
		}
	#endif
}