//====== Copyright © 1996-2005, Valve Corporation, All rights reserved. =======//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================//
#include "engine/server/server.h"
#include "engine/client/client.h"

#include "game/shared/in_buttons.h"
#include "player_command.h"

static ConVar sv_crouchSpamGuardEnable("sv_crouch_spam_guard_enable", "1", FCVAR_RELEASE,
	"Whether to reject sustained crouch spam on multiplayer servers.");
static ConVar sv_crouchSpamGuardWindow("sv_crouch_spam_guard_window", "1.25", FCVAR_RELEASE,
	"Sliding time window in seconds used to detect sustained crouch spam.",
	true, 0.05f, true, 5.0f);
static ConVar sv_crouchSpamGuardMaxPresses("sv_crouch_spam_guard_max_presses", "5", FCVAR_RELEASE,
	"Maximum accepted crouch presses in the spam guard window. Releases are never blocked.",
	true, 1.0f, true, 16.0f);

static constexpr int CROUCH_SPAM_GUARD_MAX_TRACKED_PRESSES = 16;

struct CrouchSpamGuardState
{
	CPlayer* player;
	NucleusID_t platformUserId;
	int acceptedPressTicks[CROUCH_SPAM_GUARD_MAX_TRACKED_PRESSES];
	int acceptedPressCount;
	int lastServerTick;
	bool duckInputDown;
	bool suppressUntilRelease;
};

static CrouchSpamGuardState s_crouchSpamGuardState[MAX_PLAYERS];

//-----------------------------------------------------------------------------
// Purpose: allows short official-feeling crouch bursts while rejecting only
//          sustained spam. A crouch release is never delayed or suppressed.
//-----------------------------------------------------------------------------
static void PlayerMove_ApplyCrouchSpamGuard(CPlayer* player, CUserCmd* ucmd)
{
	const int clientIndex = static_cast<int>(player->GetEdict()) - 1;

	if (clientIndex < 0 || clientIndex >= MAX_PLAYERS || player->IsBot()
		|| gpGlobals->gameMode == GameMode_t::SP_MODE)
		return;

	CrouchSpamGuardState& state = s_crouchSpamGuardState[clientIndex];

	if (!sv_crouchSpamGuardEnable.GetBool())
	{
		state = {};
		return;
	}

	const NucleusID_t platformUserId = player->GetPlatformUserId();
	const int serverTick = gpGlobals->tickCount;
	const bool stateNeedsReset = state.player != player
		|| state.platformUserId != platformUserId
		|| serverTick < state.lastServerTick;

	if (stateNeedsReset)
		state = {};

	state.player = player;
	state.platformUserId = platformUserId;
	state.lastServerTick = serverTick;

	const bool duckInputDown = (ucmd->buttons & IN_DUCK) != 0;

	if (!duckInputDown)
	{
		state.duckInputDown = false;
		state.suppressUntilRelease = false;
		return;
	}

	if (!state.duckInputDown)
	{
		const int windowTicks = Max(TIME_TO_TICKS(sv_crouchSpamGuardWindow.GetFloat()), 1);
		const int maxPresses = sv_crouchSpamGuardMaxPresses.GetInt();
		int retainedPressCount = 0;

		for (int i = 0; i < state.acceptedPressCount; ++i)
		{
			if ((serverTick - state.acceptedPressTicks[i]) < windowTicks)
				state.acceptedPressTicks[retainedPressCount++] = state.acceptedPressTicks[i];
		}

		state.acceptedPressCount = retainedPressCount;

		if (state.acceptedPressCount >= maxPresses)
			state.suppressUntilRelease = true;
		else
		{
			state.suppressUntilRelease = false;
			state.acceptedPressTicks[state.acceptedPressCount++] = serverTick;
		}
	}

	state.duckInputDown = true;

	if (state.suppressUntilRelease)
		ucmd->buttons &= ~IN_DUCK;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
CPlayerMove::CPlayerMove(void)
{
}

//-----------------------------------------------------------------------------
// Purpose: Runs movement commands for the player
// Input  : *player - 
//			*ucmd - 
//			*moveHelper - 
//-----------------------------------------------------------------------------
void CPlayerMove::StaticRunCommand(CPlayerMove* thisp, CPlayer* player, CUserCmd* ucmd, IMoveHelper* moveHelper)
{
	CClientExtended* const cle = g_pServer->GetClientExtended(player->GetEdict() - 1);
	float playerFrameTime;
	
	// Always default to clamped UserCmd frame time if this cvar is set
	if (player_disallow_negative_frametime->GetBool())
		playerFrameTime = fmaxf(ucmd->frametime, 0.0f);
	else
	{
		if (player->m_bGamePaused)
			playerFrameTime = 0.0f;
		else
			playerFrameTime = TICK_INTERVAL;

		if (ucmd->frametime)
			playerFrameTime = ucmd->frametime;
	}

	if (sv_clampPlayerFrameTime->GetBool() && player->m_joinFrameTime > ((*g_pflServerFrameTimeBase) + playerframetimekick_margin->GetFloat()))
		playerFrameTime = 0.0f;

	const float timeAllowedForProcessing = cle->ConsumeMovementTimeForUserCmdProcessing(playerFrameTime);

	if (!player->IsBot() && (timeAllowedForProcessing < playerFrameTime))
		return; // Don't process this command

	PlayerMove_ApplyCrouchSpamGuard(player, ucmd);
	CPlayerMove__RunCommand(thisp, player, ucmd, moveHelper);
}

void VPlayerMove::Detour(const bool bAttach) const
{
	DetourSetup(&CPlayerMove__RunCommand, &CPlayerMove::StaticRunCommand, bAttach);
}
