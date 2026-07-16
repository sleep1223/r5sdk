untyped																					//~mkos

#if DEVELOPER
	global function DEV_GenerateBackendSources
#endif

#if TRACKER && HAS_TRACKER_DLL
//////////////////////////////
// INTERNAL STATS FUNCTIONS //
//////////////////////////////

global function Stats__InternalInit

global function Stats__RegisterStatOutboundData
global function Stats__GenerateOutBoundJsonData
global function Stats__ClearOutboundCache

global function Stats__AddPlayerStatsTable
global function Stats__GetPlayerStatsTable
global function Stats__GetRoundStatsTables
global function Stats__ResetTableByValueType
global function Stats__PlayerExists
global function Stats__RawGetStat
global function Stats__RawSetStat

global function Stats__SetStatKeys
global function Stats__GetStatKeys

global function GetPlayerStatInt
global function GetPlayerStatString
global function GetPlayerStatBool
global function GetPlayerStatFloat

global function GetPlayerStatArray
global function GetPlayerStatArrayInt
global function GetPlayerStatArrayString
global function GetPlayerStatArrayBool
global function GetPlayerStatArrayFloat
global function PlayerStatArray_Append

global function SetPlayerStatInt
global function SetPlayerStatString
global function SetPlayerStatBool
global function SetPlayerStatFloat

global function GetPlayerRoundStatInt
global function GetPlayerRoundStatString
global function GetPlayerRoundStatBool
global function GetPlayerRoundStatFloat

global function GetPlayerSettingsTable
global function MakeVarArrayInt

global typedef StatsTable table< string, var >
typedef UIDString string

struct
{
	table< UIDString, StatsTable > onlineStatsTables
	table< UIDString, StatsTable > localStatsTables //populated on disconnect.
	table< UIDString, string > generatedOutBoundJsonData
	array< string > statKeys

	table< string, var functionref( UIDString UID ) > registeredStatOutboundValues

} file

void function Stats__InternalInit()
{
	AddCallback_OnClientDisconnected( OnDisconnected )
}

void function OnDisconnected( entity player )
{
	if( !Stats__PlayerExists( player.p.UID ) )
		return

	__AggregateStats( player )
}

StatsTable function Stats__GetPlayerStatsTable( UIDString uid )
{
	if( !Stats__PlayerExists( uid ) ) //Tracker_IsStatsReadyFor( entity player )
	{
		#if DEVELOPER
			//Assert( false, "Attempted to use " + FUNC_NAME() + "() on a player who's stats were not yet available" )
		#endif

		return EmptyStats()
	}

	return file.onlineStatsTables[ uid ]
}

table< UIDString, StatsTable > function Stats__GetRoundStatsTables()
{
	return file.localStatsTables
}

StatsTable function EmptyStats()
{
	StatsTable emptyStats
	return emptyStats
}

bool function Stats__PlayerExists( UIDString uid )
{
	return ( uid in file.onlineStatsTables )
}

void function Stats__SetStatKeys( array<string> keys )
{
	file.statKeys = keys
}

array<string> function Stats__GetStatKeys()
{
	return file.statKeys
}

var function Stats__RawGetStat( UIDString player_oid, string statname, bool online = true )
{
	table< UIDString, StatsTable > statsTable = online ? file.onlineStatsTables : file.localStatsTables

	if ( player_oid in statsTable && statname in statsTable[player_oid] )
		return statsTable[player_oid][statname]
	else
		return null
}

void function Stats__RawSetStat( UIDString uid, string statKey, var value, bool online = true )
{
	// there is a better way of doing this.
	if(online)
	{
		if ( uid in file.onlineStatsTables && statKey in file.onlineStatsTables[ uid ] )
			file.onlineStatsTables[ uid ][ statKey ] = value
	}
	else
	{
		if ( uid in file.localStatsTables && statKey in file.localStatsTables[ uid ] )
			file.localStatsTables[ uid ][ statKey ] = value
	}
}

array<string> function Stats__AddPlayerStatsTable( UIDString player_oid )
{
	var rawStatsTable = GetPlayerPersistenceData__internal( player_oid )
	array<string> statKeys = []

	if ( typeof rawStatsTable == "table" && rawStatsTable.len() > 0 )
	{
		table<string, var> statsTable = {}

        foreach ( key, value in rawStatsTable )
        {
			statKeys.append( expect string( key ) )

			if ( key == "settings" && typeof value == "table" )
            {
                table settingsTable = {}

                foreach ( subKey, subVal in value )
                    settingsTable[ expect string( subKey ) ] <- subVal

                statsTable[ expect string( key ) ] <- settingsTable
            }
            else
            {
                statsTable[ expect string( key ) ] <- value
            }
        }

		file.onlineStatsTables[ player_oid ] <- statsTable

		entity player = GetPlayerEntityByUID( player_oid )

		if( IsValid( player ) && !Tracker_IgnoreResync( player ) )
		{
			StatsTable localTable = clone statsTable
			Stats__ResetTableByValueType( localTable )

			file.localStatsTables[ player_oid ] <- localTable
		}
	}
	#if DEVELOPER
		else
			printw( "Stats table was empty for", player_oid )
	#endif

	return statKeys
}

table<string, var> function GetPlayerSettingsTable( UIDString oid )
{
    if ( !Stats__PlayerExists( oid ) )
        return EmptyStats()

    if ( !( "settings" in file.onlineStatsTables[ oid ] ) )
        return EmptyStats()

    var rawSettings = file.onlineStatsTables[ oid ][ "settings" ]
    if ( typeof rawSettings != "table" )
        return EmptyStats()

    table<string, var> settings = {}
    foreach ( key, value in rawSettings )
        settings[ expect string( key ) ] <- value

    return settings
}

int function GetPlayerStatInt( UIDString player_oid, string statname )
{
	if ( player_oid in file.onlineStatsTables && statname in file.onlineStatsTables[ player_oid ] )
		return expect int( file.onlineStatsTables[ player_oid ][ statname ] )

	return 0
}

string function GetPlayerStatString( UIDString player_oid, string statname )
{
	if ( player_oid in file.onlineStatsTables && statname in file.onlineStatsTables[ player_oid ] )
		return expect string( file.onlineStatsTables[ player_oid ][ statname ] )

	return ""
}

bool function GetPlayerStatBool( UIDString player_oid, string statname )
{
	if ( player_oid in file.onlineStatsTables && statname in file.onlineStatsTables[ player_oid ] )
		return expect bool( file.onlineStatsTables[ player_oid ][ statname ] )

	return false
}

float function GetPlayerStatFloat( UIDString player_oid, string statname )
{
	if ( player_oid in file.onlineStatsTables && statname in file.onlineStatsTables[ player_oid ] )
		return expect float( file.onlineStatsTables[ player_oid ][ statname ] )

	return 0.0
}

array<var> function GetPlayerStatArray( UIDString player_oid, string statname )
{
	array<var> statArray

	if ( player_oid in file.onlineStatsTables && statname in file.onlineStatsTables[ player_oid ] )
	{
		string typeCheck = typeof file.onlineStatsTables[ player_oid ][ statname ]
		if( typeCheck == "array" )
		{
			foreach( v in file.onlineStatsTables[ player_oid ][ statname ] )
				statArray.append( v )
		}
		else
			printw( "GetPlayerStatArray Warning: Tried to return array<var> from type", typeCheck, "for statKey" + "'" + statname + "'" )
	}

	return statArray
}

array<string> function GetPlayerStatArrayString( UIDString player_oid, string statname )
{
	array<string> statArray

	if ( player_oid in file.onlineStatsTables && statname in file.onlineStatsTables[ player_oid ] )
	{
		foreach( var value in file.onlineStatsTables[ player_oid ][ statname ] )
			statArray.append( expect string( value ) )
	}

	return statArray
}

array<int> function GetPlayerStatArrayInt( UIDString player_oid, string statname )
{
	array<int> statArray

	if ( player_oid in file.onlineStatsTables && statname in file.onlineStatsTables[ player_oid ] )
	{
		foreach( var value in file.onlineStatsTables[ player_oid ][ statname ] )
			statArray.append( expect int( value ) )
	}

	return statArray
}

array<float> function GetPlayerStatArrayFloat( UIDString player_oid, string statname )
{
	array<float> statArray

	if ( player_oid in file.onlineStatsTables && statname in file.onlineStatsTables[ player_oid ] )
	{
		foreach( var value in file.onlineStatsTables[ player_oid ][ statname ] )
			statArray.append( expect float( value ) )
	}

	return statArray
}

array<bool> function GetPlayerStatArrayBool( UIDString player_oid, string statname )
{
	array<bool> statArray

	if ( player_oid in file.onlineStatsTables && statname in file.onlineStatsTables[ player_oid ] )
	{
		foreach( var value in file.onlineStatsTables[ player_oid ][ statname ] )
			statArray.append( expect bool( value ) )
	}

	return statArray
}

//all stat arrays have a backend limit of 1000 items.
void function PlayerStatArray_Append( UIDString player_oid, string statname, var value )
{
	if ( player_oid in file.onlineStatsTables && statname in file.onlineStatsTables[ player_oid ] )
		file.onlineStatsTables[ player_oid ][ statname ].append( value )
}

void function SetPlayerStatInt( UIDString player_oid, string statname, int value )
{
	if ( player_oid in file.onlineStatsTables && statname in file.onlineStatsTables[ player_oid ] )
		file.onlineStatsTables[ player_oid ][ statname ] = value
}

void function SetPlayerStatString( UIDString player_oid, string statname, string value )
{
	#if DEVELOPER
		// This assert is only on dev as the string will be discarded later anyway f it exceeds the length
		mAssert( value.len() <= 30, "Invalid string length for the value of statname \"" + statname + "\" value: \"" + value )
	#endif

	if ( player_oid in file.onlineStatsTables && statname in file.onlineStatsTables[ player_oid ] )
		file.onlineStatsTables[ player_oid ][ statname ] = value
}

void function SetPlayerStatBool( UIDString player_oid, string statname, bool value )
{
	if ( player_oid in file.onlineStatsTables && statname in file.onlineStatsTables[ player_oid ] )
		file.onlineStatsTables[ player_oid ][ statname ] = value
}

void function SetPlayerStatFloat( UIDString player_oid, string statname, float value )
{
	if ( player_oid in file.onlineStatsTables && statname in file.onlineStatsTables[ player_oid ] )
		file.onlineStatsTables[ player_oid ][ statname ] = value
}

// These are not handled by script registered stats and it is futile to send out,
// as they will be dropped in the backend.
const array<string> IGNORE_STATS =
[
	"player",
	"jumps",
	"settings",
	"total_time_played",
	"total_matches",
	"score"
]

array<string> function GenerateOutBoundDataList()
{
	array<string> generatedOutboundList = []

	foreach( key in file.statKeys )
	{
		if( !IGNORE_STATS.contains( key ) )
			generatedOutboundList.append( key )
	}

	return generatedOutboundList
}

string function Stats__GenerateOutBoundJsonData( UIDString UID )
{
	if( empty( UID ) )
	{
		mAssert( false, "empty UID passed to " + FUNC_NAME() )
		return ""
	}

	if( ( UID in file.generatedOutBoundJsonData ) && !empty( file.generatedOutBoundJsonData[ UID ] ) )
		return file.generatedOutBoundJsonData[ UID ]

	tracker.RunShipFunctions( UID )

	string json = "";
	array<string> validOutBoundStats = GenerateOutBoundDataList()

	foreach( statKey in validOutBoundStats )
	{
		if( statKey in file.registeredStatOutboundValues )
		{
			var data
			switch( __ShouldUseLocal_internal( UID, statKey ) )
			{
				case true:
					data = Stats__RawGetStat( UID, statKey, false )
					break

				case false:
					data = file.registeredStatOutboundValues[ statKey ]( UID )
					break
			}

			string vType = typeof( data )

			switch( vType )
			{
				case "string":
					json += "\"" + statKey + "\": \"" + expect string( data ) + "\", ";
					break

				case "int":
					json += "\"" + statKey + "\": " + expect int( data ).tostring() + ", ";
					break

				case "float":
					json += "\"" + statKey + "\": " + format( "%.7f", expect float( data ) ) + ", ";
					break

				case "bool":
					json += "\"" + statKey + "\": " + expect bool( data ).tostring() + ", ";
					break

				case "array":
                {
                    string arrayJson = "["
                    foreach( item in data )
                    {
                        string itemType = typeof( item )
                        switch( itemType )
                        {
                            case "string":
                                arrayJson += "\"" + expect string( item ) + "\", ";
                                break
                            case "int":
                                arrayJson += expect int( item ).tostring() + ", ";
                                break
                            case "float": //we must format, or non fractional floats are truncated and misinterpreted as int, ie: 1.0 becomes 1
										  //rounding can also occur at precision exceeding 5 decimal places when casting to string.
                                arrayJson += format( "%.7f", expect float( item ) ) + ", ";
                                break
                            case "bool":
                                arrayJson += expect bool( item ).tostring() + ", ";
                                break
                            default:
                                arrayJson += "null, ";
                        }
                    }
                    if( arrayJson.len() > 1 )
                        arrayJson = arrayJson.slice( 0, arrayJson.len() - 2 )

                    arrayJson += "]"
                    json += "\"" + statKey + "\": " + arrayJson + ", ";
                    break
                }

				#if DEVELOPER
					default:
						printw( "Unsupported stat value type for", "\""+ statKey + "\":", vType )
				#endif
			}
		}
	}

	file.generatedOutBoundJsonData[ UID ] <- json
	return json
}

void function Stats__ClearOutboundCache()
{
	file.generatedOutBoundJsonData = {} //reassign reference
}

void function Stats__RegisterStatOutboundData( string statname, var functionref( string UID ) func )
{
	if( ( statname in file.registeredStatOutboundValues ) )
	{
		sqerror( "Tried to add func " + string( func ) + "() as an outbound data func for [" + statname + "] but func " + string( file.registeredStatOutboundValues[statname] ) + "() is already defined to handle outbound data for stat." )
		return
	}

	file.registeredStatOutboundValues[ statname ] <- func
}

void function __AggregateStats( entity player )
{
	string uid = player.p.UID
	array<string> validOutBoundStats = GenerateOutBoundDataList()

	foreach( statKey in validOutBoundStats )
	{
		if( !( statKey in file.registeredStatOutboundValues ) )
			continue

		__AggregateStat_internal( player, statKey )
	}
}

void function __AggregateStat_internal( entity player, string statKey )
{
	string uid = player.p.UID

	var data = file.registeredStatOutboundValues[ statKey ]( uid )
	string vType = typeof( data )

	switch( vType )
	{
		case "int":

			int addValue = expect int( data )
			int storedValue = GetPlayerRoundStatInt( uid, statKey )
			Stats__RawSetStat( uid, statKey, MakeVar( addValue + storedValue ), false )
			break

		case "float":
			float addValue = expect float( data )
			float storedValue = GetPlayerRoundStatFloat( uid, statKey )
			Stats__RawSetStat( uid, statKey, MakeVar( addValue + storedValue ), false )
			break

		case "bool":
		case "string":
		case "array":
			Stats__RawSetStat( uid, statKey, data, false )
			break

		case "table":
		default:
			mAssert( false, "%s is currently unsupported.", vType )
	}
}

var function MakeVar( ... )
{
	if( vargc > 0 )
		return vargv[ 0 ]

	mAssert( false, "Called MakeVar with no arguments." )
	return null
}

var function MakeVarArrayInt( array<int> typedArray )
{
	array arr
	foreach( item in typedArray )
		arr.append( item )

	return arr
}

void function Stats__ResetTableByValueType( StatsTable statsTbl )
{
	foreach( string statKey, var statValue in statsTbl )
	{
		string vType = typeof( statValue )
		switch( vType )
		{
			case "int":
				statsTbl[ statKey ] = 0
				break

			case "float":
				statsTbl[ statKey] = 0.0
				break

			case "string":
				statsTbl[ statKey ] = ""
				break

			case "bool":
				statsTbl[ statKey ] = false
				break

			case "table":
				statsTbl[ statKey ] = {}
				break

			case "array":
				statsTbl[ statKey ] = []
				break

			default:
				#if DEVELOPER
					mAssert( false, "Unsupported stat type \"" + vType + "\" for stat key \"" + statKey + "\"" )
				#endif
		}
	}
}

int function GetPlayerRoundStatInt( UIDString player_oid, string statname )
{
	if ( player_oid in file.localStatsTables && statname in file.localStatsTables[ player_oid ] )
		return expect int( file.localStatsTables[ player_oid ][ statname ] )

	return 0
}

string function GetPlayerRoundStatString( UIDString player_oid, string statname )
{
	if ( player_oid in file.localStatsTables && statname in file.localStatsTables[ player_oid ] )
		return expect string( file.localStatsTables[ player_oid ][ statname ] )

	return ""
}

bool function GetPlayerRoundStatBool( UIDString player_oid, string statname )
{
	if ( player_oid in file.localStatsTables && statname in file.localStatsTables[ player_oid ] )
		return expect bool( file.localStatsTables[ player_oid ][ statname ] )

	return false
}

float function GetPlayerRoundStatFloat( UIDString player_oid, string statname )
{
	if ( player_oid in file.localStatsTables && statname in file.localStatsTables[ player_oid ] )
		return expect float( file.localStatsTables[ player_oid ][ statname ] )

	return 0.0
}

bool function __ShouldUseLocal_internal( UIDString uid, string statKey )
{
	if( !Tracker_StatLocalAllowed( statKey ) )
		return false

	entity player = GetPlayerEntityByUID( uid )
	if( IsValid( player ) )
	{
		if( Tracker_GetPlayerLeftFlag( player ) )
		{
			__AggregateStat_internal( player, statKey )
			return true
		}

		return false
	}

	return true
}
#else //TRACKER && HAS_TRACKER_DLL

global function Stats__GetStatKeys

global function GetPlayerStatInt
global function GetPlayerStatString
global function GetPlayerStatBool
global function GetPlayerStatFloat

global function SetPlayerStatInt
global function SetPlayerStatString
global function SetPlayerStatBool
global function SetPlayerStatFloat

global function GetPlayerStatArray
global function GetPlayerStatArrayInt
global function GetPlayerStatArrayString
global function GetPlayerStatArrayBool
global function GetPlayerStatArrayFloat
global function PlayerStatArray_Append
global function Stats__RawGetStat

array<string> function Stats__GetStatKeys(){ return [] }

int function GetPlayerStatInt( string player, string statname ){ return 0 }
string function GetPlayerStatString( string player, string statname ){ return "" }
bool function GetPlayerStatBool( string player, string statname ){ return false }
float function GetPlayerStatFloat( string player, string statname ){ return 0.0 }

void function SetPlayerStatInt( string player, string statname, int value ){}
void function SetPlayerStatString( string player, string statname, string value ){}
void function SetPlayerStatBool( string player, string statname, bool value ){}
void function SetPlayerStatFloat( string player, string statname, float value ){}

array<var> function GetPlayerStatArray( string player_oid, string statname ){ return [] }
array<int> function GetPlayerStatArrayInt( string player_oid, string statname ){ return [] }
array<string> function GetPlayerStatArrayString( string player_oid, string statname ){ return [] }
array<float> function GetPlayerStatArrayFloat( string player_oid, string statname ){ return [] }
array<bool> function GetPlayerStatArrayBool( string player_oid, string statname ){ return [] }
void function PlayerStatArray_Append( string player_oid, string statname, var value ){}
var function Stats__RawGetStat( string uid, string key ){ return "" }
#endif // ELSE !TRACKER && !HAS_TRACKER_DLL

// SHARED

#if DEVELOPER
void function DEV_GenerateBackendSources()
{
	//File: weapons_projectiles_per_shot
	string file = "weapon_projectiles_per_shot.txt"
	string directory = "scripts/devfiles/"
	DevTextBufferClear()

	string weapon_projectiles_per_shot_string = ""
	foreach( int id, string weapon in DamageSourceIDToStringTable() )
	{
		if( !WeaponIsPrecached( weapon ) ) //wont have a set file / wont be used.
			continue

		weapon_projectiles_per_shot_string += format( "%s=%s\n", weapon, GetWeaponSettingIntFromFile( weapon, "projectiles_per_shot" ).tostring() )
	}

	DevTextBufferWrite( weapon_projectiles_per_shot_string )
	DevP4Checkout( file )
	DevTextBufferDumpToFile( directory + file )
	printt( "Generated: ", directory + file )

	//File:
}
#endif