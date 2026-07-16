global function MapSkip_Init													//mkos

struct
{
	bool bMapSkipEnabled
	int percentageRequiredToSkip
	float voteDebounceTime
	float voteDecayTime

	table<string,float> playerVoteTimes

} file

void function MapSkip_Init()
{
	file.bMapSkipEnabled 			= GetCurrentPlaylistVarBool( "map_skip_enabled", false )
	file.percentageRequiredToSkip 	= GetCurrentPlaylistVarInt( "map_skip_percentage_required", 60 )
	file.voteDecayTime				= GetCurrentPlaylistVarFloat( "map_vote_decay_time", 60.0 )
	file.voteDebounceTime			= GetCurrentPlaylistVarFloat( "map_vote_debounce_time", 30.0 )

	mAssert( file.voteDecayTime > -1, "map_vote_decay_time must be 0 or greater" )
	if( file.voteDecayTime == 0 )
		file.voteDecayTime = fsGlobal.EndlessFFAorTDM ? 600.0 : FlowState_RoundTime().tofloat()

	AddCallback_OnClientDisconnected( __RemovePlayerVoteSlot )

	if( file.bMapSkipEnabled  )
		Commands_Register( "!skip", cmd_skipmap, [ "/skip", "\\skip" ] )
}

void function cmd_skipmap( string baseCmd, array<string> args, entity activator )
{
	CastMapSkipVote( activator )
}

void function CastMapSkipVote( entity player = null, bool bRecheck = false )
{
	if( player != null )
	{
		if( !CheckRate( player, "vote_skip_map", file.voteDebounceTime, true ) )
			return

		__SetupPlayerVoteSlot( player, Time() )
	}

	int votes 		= GetCurrentMapSkipCount()
	int players 	= GetPlayerArray().len() - GetDisconnectingPlayerCount()
	if( players == 0 )
		return
	float percent = ( votes.tofloat() / players.tofloat() ) * 100.0

	if( !bRecheck && votes == 1 )
	{
		string helpString = "!skip\n" + Commands_GetCommandAliases( "!skip" ).join( "\n" )

		foreach( sPlayer in GetPlayerArray() )
			LocalMsg( sPlayer, "#MAP_SKIP_INITIATED", "#MAP_SKIP_VOTE_INSTRUCTIONS", eMsgUI.DEFAULT, 5.0, "", helpString )
	}

	if( percent >= file.percentageRequiredToSkip )
	{
		SendServerMessage( "Map skip voting complete. Changing to next map" )

		thread
		(
			void function()
			{
				foreach( sPlayer in GetPlayerArray() )
					LocalMsg( sPlayer, "#MAP_SKIP_COMPLETE", "#MAP_SKIP_CHANGE" )

				wait 5

				Flowstate_ForceMapChange( true )
				EndRound()
			}
		)()
	}
	else if( !bRecheck )
	{
		int requiredVotes 				= ceil( ( players * file.percentageRequiredToSkip ) / 100.0 ).tointeger()
		int votesRemainingNeeded 		= maxint( 0, requiredVotes - votes )

		string mapSkipMessage = format
		(
			"Skip votes: %d/%d (%d total votes required, %d more to skip).",
			votes,
			players,
			requiredVotes,
			votesRemainingNeeded
		)

		SendServerMessage( mapSkipMessage )
	}
}

int function GetCurrentMapSkipCount()
{
	table<string,float> voteTimeTable 	= clone file.playerVoteTimes
	float currentTime 					= Time()
	float voteTimeDecay 				= file.voteDecayTime

	foreach( string uid, float voteTime in voteTimeTable )
	{
		if( currentTime - voteTime > voteTimeDecay )
			delete file.playerVoteTimes[ uid ]
	}

	return file.playerVoteTimes.len()
}

void function __SetupPlayerVoteSlot( entity player, float voteTime )
{
	string uid = player.p.UID
	if( !( uid in file.playerVoteTimes ) )
		file.playerVoteTimes[ uid ] <- voteTime
	else
		file.playerVoteTimes[ uid ] = voteTime
}

void function __RemovePlayerVoteSlot( entity player )
{
	string uid = player.p.UID
	if( uid in file.playerVoteTimes )
		delete file.playerVoteTimes[ uid ]

	if( file.playerVoteTimes.len() )
		CastMapSkipVote( null, true ) //recheck on disconnect to satisfy percentages
}