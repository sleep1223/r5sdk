//=============================================================================//
// 
// Purpose: Expose native code to VScript API
// 
//-----------------------------------------------------------------------------
// 
// Read the documentation in 'game/shared/vscript_shared.cpp' before modifying
// existing code or adding new code!
// 
// To create server script bindings:
// - use the DEFINE_SERVER_SCRIPTFUNC_NAMED() macro.
// - prefix your function with "ServerScript_" i.e.: "ServerScript_GetVersion".
// 
//=============================================================================//

#include "core/stdafx.h"
#include "core/logger.h"
#include "common/callback.h"
#include "engine/server/server.h"
#include "engine/host_state.h"
#include "engine/debugoverlay.h"
#include "networksystem/matchreport.h"
#include "pluginsystem/pluginsystem.h"
#include "vscript/vscript.h"
#include "vscript/languages/squirrel_re/include/sqvm.h"

#include "game/shared/vscript_gamedll_defs.h"

#include "game/shared/vscript_shared.h"
#include "game/shared/vscript_debug_overlay_shared.h"
#include "game/server/util_server.h"

#include "liveapi/liveapi.h"
#include "vscript_server.h"
#include "player.h"
#include "detour_impl.h"

#include <networksystem/hostmanager.h>
#include <random>

#include "game/server/logger.h"
#include <vscript/languages/squirrel_re/include/sqarrayutils.h>

/*
=====================
SQVM_ServerScript_f

  Executes input on the
  VM in SERVER context.
=====================
*/
static void SQVM_ServerScript_f(const CCommand& args)
{
    if (args.ArgC() >= 2)
    {
        Script_Execute(args.ArgS(), SQCONTEXT::SERVER);
    }
}

static ConCommand script("script", SQVM_ServerScript_f, "Run input code as SERVER script on the VM", FCVAR_DEVELOPMENTONLY | FCVAR_GAMEDLL | FCVAR_CHEAT | FCVAR_SERVER_FRAME_THREAD);

//-----------------------------------------------------------------------------
// Purpose: server NDebugOverlay proxies
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_DebugDrawSolidBox(HSQUIRRELVM v)
{
    return SharedScript_DebugDrawSolidBox(v);
}
static SQRESULT ServerScript_DebugDrawSweptBox(HSQUIRRELVM v)
{
    return SharedScript_DebugDrawSweptBox(v);
}
static SQRESULT ServerScript_DebugDrawTriangle(HSQUIRRELVM v)
{
    return SharedScript_DebugDrawTriangle(v);
}
static SQRESULT ServerScript_DebugDrawSolidSphere(HSQUIRRELVM v)
{
    return SharedScript_DebugDrawSolidSphere(v);
}
static SQRESULT ServerScript_DebugDrawCapsule(HSQUIRRELVM v)
{
    return SharedScript_DebugDrawCapsule(v);
}

//-----------------------------------------------------------------------------
// Purpose: calculates the duration for the debug text overlay
//-----------------------------------------------------------------------------
static float ServerScript_DebugScreenText_DetermineDuration(HSQUIRRELVM v)
{
    const float serverFPS = script_server_fps->GetFloat();
    // Make sure the overlay exists as long as the entire server
    // script frame, as it must last until the next call from the
    // server is initiated. Otherwise the following happens:
    // 
    // - 1 / script_server_fps < NDEBUG_PERSIST_TILL_NEXT_SERVER =
    //                           text will flicker as they decay
    //                           before the next frame is fired.
    // - 1 / script_server_fps > NDEBUG_PERSIST_TILL_NEXT_SERVER =
    //                           text will overlap with previous
    //                           as the prev hasn't decayed yet.
    return 1.0f / serverFPS;
}

//-----------------------------------------------------------------------------
// Purpose: internal handler for adding debug texts on screen through scripts
//-----------------------------------------------------------------------------
static void ServerScript_Internal_DebugScreenTextWithColor(HSQUIRRELVM v, const float posX, const float posY, const Color color, const char* const text)
{
    const float duration = ServerScript_DebugScreenText_DetermineDuration(v);
    g_pDebugOverlay->AddScreenTextOverlay(posX, posY, duration, color.r(), color.g(), color.b(), color.a(), text);
}

//-----------------------------------------------------------------------------
// Purpose: adds a debug text on the screen at given position
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_DebugScreenText(HSQUIRRELVM v)
{
    if (g_pDebugOverlay)
    {
        SQFloat posX;
        SQFloat posY;
        const SQChar* text = nullptr;

        sq_getfloat(v, 2, &posX);
        sq_getfloat(v, 3, &posY);
        sq_getstring(v, 4, &text);

        const Color color(255, 255, 255, 255);
        ServerScript_Internal_DebugScreenTextWithColor(v, posX, posY, color, text);
    }

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: adds a debug text on the screen at given position with color
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_DebugScreenTextWithColor(HSQUIRRELVM v)
{
    if (g_pDebugOverlay)
    {
        SQFloat posX;
        SQFloat posY;
        const SQChar* text = nullptr;
        const SQVector3D* colorVec = nullptr;

        sq_getfloat(v, 2, &posX);
        sq_getfloat(v, 3, &posY);
        sq_getstring(v, 4, &text);
        sq_getvector(v, 5, &colorVec);

        const Color color = Script_VectorToColor(colorVec, 1.0f);
        ServerScript_Internal_DebugScreenTextWithColor(v, posX, posY, color, text);
    }

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: sets whether the server could auto reload at this time (e.g. if
// server admin has host_autoReloadRate AND host_autoReloadRespectGameState
// set, and its time to auto reload, but the match hasn't finished yet, wait
// until this is set to proceed the reload of the server
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_SetAutoReloadState(HSQUIRRELVM v)
{
    SQBool state = false;
    sq_getbool(v, 2, &state);

    g_hostReloadState = state;
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: kicks a player by given name
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_KickPlayerByName(HSQUIRRELVM v)
{
    const SQChar* playerName = nullptr;
    const SQChar* reason = nullptr;

    sq_getstring(v, 2, &playerName);
    sq_getstring(v, 3, &reason);

    if (!VALID_CHARSTAR(playerName))
    {
        v_SQVM_ScriptError("Empty or null player name");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    // Discard empty strings, this will use the default message instead.
    if (!VALID_CHARSTAR(reason))
        reason = nullptr;

    g_BanSystem.KickPlayerByName(playerName, reason);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: kicks a player by given handle or id
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_KickPlayerById(HSQUIRRELVM v)
{
    const SQChar* playerHandle = nullptr;
    const SQChar* reason = nullptr;

    sq_getstring(v, 2, &playerHandle);
    sq_getstring(v, 3, &reason);

    if (!VALID_CHARSTAR(playerHandle))
    {
        v_SQVM_ScriptError("Empty or null player handle");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    // Discard empty strings, this will use the default message instead.
    if (!VALID_CHARSTAR(reason))
        reason = nullptr;

    g_BanSystem.KickPlayerById(playerHandle, reason);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: bans a player by given name
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_BanPlayerByName(HSQUIRRELVM v)
{
    const SQChar* playerName = nullptr;
    const SQChar* reason = nullptr;
    const SQChar* bannedByID = nullptr;

    sq_getstring(v, 2, &playerName);
    sq_getstring(v, 3, &reason);
    if (sq_gettop(v) >= 4)
        sq_getstring(v, 4, &bannedByID);

    if (!VALID_CHARSTAR(playerName))
    {
        v_SQVM_ScriptError("Empty or null player name");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    // Discard empty strings, this will use the default message instead.
    if (!VALID_CHARSTAR(reason))
        reason = nullptr;

    if (!VALID_CHARSTAR(bannedByID))
        bannedByID = nullptr;

    g_BanSystem.BanPlayerByName(playerName, bannedByID, reason);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: bans a player by given handle or id
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_BanPlayerById(HSQUIRRELVM v)
{
    const SQChar* playerHandle = nullptr;
    const SQChar* reason = nullptr;
    const SQChar* bannedByID = nullptr;

    sq_getstring(v, 2, &playerHandle);
    sq_getstring(v, 3, &reason);
    if (sq_gettop(v) >= 4)
        sq_getstring(v, 4, &bannedByID);

    if (!VALID_CHARSTAR(playerHandle))
    {
        v_SQVM_ScriptError("Empty or null player handle");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    // Discard empty strings, this will use the default message instead.
    if (!VALID_CHARSTAR(reason))
        reason = nullptr;

    if (!VALID_CHARSTAR(bannedByID))
        bannedByID = nullptr;

    g_BanSystem.BanPlayerById(playerHandle, bannedByID, reason);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: bans a player by given handle or id
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_BanPlayerByIdOnly(HSQUIRRELVM v)
{
    const SQChar* playerHandle = nullptr;
    const SQChar* reason = nullptr;
    const SQChar* bannedByID = nullptr;

    sq_getstring(v, 2, &playerHandle);
    sq_getstring(v, 3, &reason);
    sq_getstring(v, 4, &bannedByID);

    if (!VALID_CHARSTAR(playerHandle))
    {
        v_SQVM_ScriptError("Empty or null player handle");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    // Discard empty strings, this will use the default message instead.
    if (!VALID_CHARSTAR(reason))
        reason = nullptr;

    if (!VALID_CHARSTAR(bannedByID))
        bannedByID = nullptr;

    g_BanSystem.BanPlayerById(playerHandle, bannedByID, reason, true);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}


//-----------------------------------------------------------------------------
// Purpose: unbans a player by given nucleus id or ip address
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_UnbanPlayer(HSQUIRRELVM v)
{
    const SQChar* szCriteria = nullptr;
    sq_getstring(v, 2, &szCriteria);

    if (!VALID_CHARSTAR(szCriteria))
    {
        v_SQVM_ScriptError("Empty or null player criteria");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    g_BanSystem.UnbanPlayer(szCriteria);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_BroadcastServerTextMessage(HSQUIRRELVM v)
{
    const SQChar* pszPrefix = nullptr;
    const SQChar* pszMessage = nullptr;
    SQBool bAdminMsg = false;

    sq_getstring(v, 2, &pszPrefix);
    sq_getstring(v, 3, &pszMessage);
    sq_getbool(v, 4, &bAdminMsg);

    if (!VALID_CHARSTAR(pszPrefix))
    {
        v_SQVM_ScriptError("Null prefix string");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    if (!VALID_CHARSTAR(pszMessage))
    {
        v_SQVM_ScriptError("Null message string");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    SVC_SystemSayText message(pszPrefix, pszMessage, bAdminMsg);

    g_pServer->BroadcastMessage(&message, true, false);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_SendServerTextMessage(HSQUIRRELVM v)
{
    CPlayer* pPlayer = nullptr;
    const SQChar* pszPrefix = nullptr;
    const SQChar* pszMessage = nullptr;
    SQBool bAdminMsg = false;

    if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)))
        return SQ_ERROR;

    sq_getstring(v, 2, &pszPrefix);
    sq_getstring(v, 3, &pszMessage);
    sq_getbool(v, 4, &bAdminMsg);

    if (!VALID_CHARSTAR(pszPrefix))
    {
        v_SQVM_ScriptError("Null prefix string");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    if (!VALID_CHARSTAR(pszMessage))
    {
        v_SQVM_ScriptError("Null message string");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }
    
    CClient* const pClient = g_pServer->GetClient(pPlayer->GetEdict() - 1);
    
    if (!pClient)
        return SQ_ERROR;

    SVC_SystemSayText message(pszPrefix, pszMessage, bAdminMsg);
    
    sq_pushbool(v, pClient->SendNetMsgEx(&message, false, false, false));
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_IsMuted(HSQUIRRELVM v)
{
    CPlayer* pPlayer = nullptr;

    if (!v_sq_getentity(v, reinterpret_cast<SQEntity*>(&pPlayer)) || !pPlayer || !g_pServer)
        return SQ_ERROR;

    CClientExtended* const pClientExtended = g_pServer->GetClientExtended(pPlayer->GetEdict() - 1);

    if (!pClientExtended)
        return SQ_ERROR;

    sq_pushbool(v, pClientExtended->IsClientCommsBanned());
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: gets the number of real players on this server
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_GetNumHumanPlayers(HSQUIRRELVM v)
{
    sq_pushinteger(v, g_pServer->GetNumHumanPlayers());
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: gets the number of fake players on this server
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_GetNumFakeClients(HSQUIRRELVM v)
{
    sq_pushinteger(v, g_pServer->GetNumFakeClients());
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: gets our current session id
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_GetSessionID(HSQUIRRELVM v)
{
    sq_pushstring(v, g_LogSessionUUID.c_str(), (SQInteger)g_LogSessionUUID.length());
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: writes crash diagnostics breadcrumbs from script
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_SDK_WriteRuntimeBreadcrumb(HSQUIRRELVM v)
{
    const SQChar* pszSource = nullptr;
    const SQChar* pszDetail = nullptr;

    sq_getstring(v, 2, &pszSource);
    if (sq_gettop(v) >= 3)
        sq_getstring(v, 3, &pszDetail);

    if (VALID_CHARSTAR(pszSource))
        SDK_WriteRuntimeBreadcrumb(pszSource, VALID_CHARSTAR(pszDetail) ? pszDetail : nullptr);

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: writes match report diagnostics to the dedicated debug log
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_MatchReport_DebugNote(HSQUIRRELVM v)
{
    const SQChar* pszMessage = nullptr;
    sq_getstring(v, 2, &pszMessage);

    if (VALID_CHARSTAR(pszMessage))
        SV_MatchReportDebugNote(pszMessage);

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static bool ServerScript_GetPlayerArg(HSQUIRRELVM v, const SQInteger nArg, CPlayer*& outPlayer)
{
    if (sq_gettop(v) < nArg)
    {
        v_SQVM_ScriptError("Missing entity argument %d", nArg - 1);
        return false;
    }

    SQObjectPtr& obj = stack_get(v, nArg);
    if (sq_type(obj) != OT_ENTITY)
    {
        v_SQVM_ScriptError("Argument %d must be an entity", nArg - 1);
        return false;
    }

    CPlayer* const pCandidate = reinterpret_cast<CPlayer*>(_userpointer(obj));
    if (!pCandidate)
    {
        v_SQVM_ScriptError("Argument %d was a null entity", nArg - 1);
        return false;
    }

    if (!g_pServer)
    {
        v_SQVM_ScriptError("Argument %d cannot be validated without an active server", nArg - 1);
        return false;
    }

    for (int i = 0; i < g_pServer->GetMaxClients(); ++i)
    {
        CClient* const pClient = g_pServer->GetClient(i);
        if (!pClient || !pClient->IsConnected())
            continue;

        if (UTIL_PlayerByIndex(pClient->GetHandle()) != pCandidate)
            continue;

        outPlayer = pCandidate;
        return true;
    }

    v_SQVM_ScriptError("Argument %d must be a connected player entity", nArg - 1);
    return false;
}

//-----------------------------------------------------------------------------
// Purpose: records a weapon kill for match-end reporting
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_MatchReport_RecordWeaponKill(HSQUIRRELVM v)
{
    CPlayer* pPlayer = nullptr;
    const SQChar* pszWeaponName = nullptr;

    if (!ServerScript_GetPlayerArg(v, 2, pPlayer))
        return SQ_ERROR;

    sq_getstring(v, 3, &pszWeaponName);

    if (!VALID_CHARSTAR(pszWeaponName))
    {
        v_SQVM_ScriptError("Empty or null weapon name");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    SV_RecordMatchReportWeaponKill(pPlayer, pszWeaponName);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: records a weapon kill for match-end reporting by stable player UID
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_MatchReport_RecordWeaponKillByUid(HSQUIRRELVM v)
{
    const SQChar* pszPlayerUid = nullptr;
    const SQChar* pszPlayerName = nullptr;
    const SQChar* pszWeaponName = nullptr;

    sq_getstring(v, 2, &pszPlayerUid);
    sq_getstring(v, 3, &pszPlayerName);
    sq_getstring(v, 4, &pszWeaponName);

    if (!VALID_CHARSTAR(pszWeaponName))
    {
        v_SQVM_ScriptError("Empty or null weapon name");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    SV_RecordMatchReportWeaponKillByUid(pszPlayerUid, pszPlayerName, pszWeaponName);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: records a kill event for match-end reporting
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_MatchReport_RecordPlayerKill(HSQUIRRELVM v)
{
    CPlayer* pAttacker = nullptr;
    const SQChar* pszVictimUid = nullptr;
    const SQChar* pszVictimName = nullptr;
    const SQChar* pszWeaponName = nullptr;
    SQInteger nDamageSourceId = 0;

    if (!ServerScript_GetPlayerArg(v, 2, pAttacker))
        return SQ_ERROR;

    sq_getstring(v, 3, &pszVictimUid);
    sq_getstring(v, 4, &pszVictimName);
    sq_getstring(v, 5, &pszWeaponName);
    sq_getinteger(v, 6, &nDamageSourceId);

    if (!VALID_CHARSTAR(pszWeaponName))
    {
        v_SQVM_ScriptError("Empty or null weapon name");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    if (!VALID_CHARSTAR(pszVictimUid))
    {
        SV_RecordMatchReportWeaponKill(pAttacker, pszWeaponName);
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }

    SV_RecordMatchReportPlayerKill(pAttacker, pszVictimUid, pszVictimName, pszWeaponName, static_cast<int>(nDamageSourceId));
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: records a kill event for match-end reporting by stable player UIDs
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_MatchReport_RecordPlayerKillByUid(HSQUIRRELVM v)
{
    const SQChar* pszAttackerUid = nullptr;
    const SQChar* pszAttackerName = nullptr;
    const SQChar* pszVictimUid = nullptr;
    const SQChar* pszVictimName = nullptr;
    const SQChar* pszWeaponName = nullptr;
    SQInteger nDamageSourceId = 0;

    sq_getstring(v, 2, &pszAttackerUid);
    sq_getstring(v, 3, &pszAttackerName);
    sq_getstring(v, 4, &pszVictimUid);
    sq_getstring(v, 5, &pszVictimName);
    sq_getstring(v, 6, &pszWeaponName);
    sq_getinteger(v, 7, &nDamageSourceId);

    if (!VALID_CHARSTAR(pszWeaponName))
    {
        v_SQVM_ScriptError("Empty or null weapon name");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    SV_RecordMatchReportPlayerKillByUid(pszAttackerUid, pszAttackerName, pszVictimUid, pszVictimName,
        pszWeaponName, static_cast<int>(nDamageSourceId));
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: records weapon shots for match-end reporting
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_MatchReport_RecordWeaponShot(HSQUIRRELVM v)
{
    CPlayer* pPlayer = nullptr;
    const SQChar* pszWeaponName = nullptr;
    SQInteger nShots = 0;

    if (!ServerScript_GetPlayerArg(v, 2, pPlayer))
        return SQ_ERROR;

    sq_getstring(v, 3, &pszWeaponName);
    sq_getinteger(v, 4, &nShots);

    if (!VALID_CHARSTAR(pszWeaponName))
    {
        v_SQVM_ScriptError("Empty or null weapon name");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    SV_RecordMatchReportWeaponShot(pPlayer, pszWeaponName, static_cast<int>(nShots));
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: records weapon shots for match-end reporting by stable player UID
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_MatchReport_RecordWeaponShotByUid(HSQUIRRELVM v)
{
    const SQChar* pszPlayerUid = nullptr;
    const SQChar* pszPlayerName = nullptr;
    const SQChar* pszWeaponName = nullptr;
    SQInteger nShots = 0;

    sq_getstring(v, 2, &pszPlayerUid);
    sq_getstring(v, 3, &pszPlayerName);
    sq_getstring(v, 4, &pszWeaponName);
    sq_getinteger(v, 5, &nShots);

    if (!VALID_CHARSTAR(pszWeaponName))
    {
        v_SQVM_ScriptError("Empty or null weapon name");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    SV_RecordMatchReportWeaponShotByUid(pszPlayerUid, pszPlayerName, pszWeaponName, static_cast<int>(nShots));
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: records weapon hits and damage for match-end reporting
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_MatchReport_RecordWeaponHit(HSQUIRRELVM v)
{
    CPlayer* pPlayer = nullptr;
    const SQChar* pszWeaponName = nullptr;
    SQFloat flDamage = 0.0f;
    SQInteger nHits = 0;
    SQFloat flBulletsHit = 0.0f;
    SQInteger nHeadshots = 0;

    if (!ServerScript_GetPlayerArg(v, 2, pPlayer))
        return SQ_ERROR;

    sq_getstring(v, 3, &pszWeaponName);
    sq_getfloat(v, 4, &flDamage);
    sq_getinteger(v, 5, &nHits);
    sq_getfloat(v, 6, &flBulletsHit);
    sq_getinteger(v, 7, &nHeadshots);

    if (!VALID_CHARSTAR(pszWeaponName))
    {
        v_SQVM_ScriptError("Empty or null weapon name");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    SV_RecordMatchReportWeaponHit(pPlayer, pszWeaponName, static_cast<float>(flDamage), static_cast<int>(nHits), static_cast<float>(flBulletsHit), static_cast<int>(nHeadshots));
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: records weapon hits and damage for match-end reporting by stable player UID
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_MatchReport_RecordWeaponHitByUid(HSQUIRRELVM v)
{
    const SQChar* pszPlayerUid = nullptr;
    const SQChar* pszPlayerName = nullptr;
    const SQChar* pszWeaponName = nullptr;
    SQFloat flDamage = 0.0f;
    SQInteger nHits = 0;
    SQFloat flBulletsHit = 0.0f;
    SQInteger nHeadshots = 0;

    sq_getstring(v, 2, &pszPlayerUid);
    sq_getstring(v, 3, &pszPlayerName);
    sq_getstring(v, 4, &pszWeaponName);
    sq_getfloat(v, 5, &flDamage);
    sq_getinteger(v, 6, &nHits);
    sq_getfloat(v, 7, &flBulletsHit);
    sq_getinteger(v, 8, &nHeadshots);

    if (!VALID_CHARSTAR(pszWeaponName))
    {
        v_SQVM_ScriptError("Empty or null weapon name");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    SV_RecordMatchReportWeaponHitByUid(pszPlayerUid, pszPlayerName, pszWeaponName,
        static_cast<float>(flDamage), static_cast<int>(nHits), static_cast<float>(flBulletsHit),
        static_cast<int>(nHeadshots));
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: sets a class var on the server and each client
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_ScriptSetClassVar(HSQUIRRELVM v)
{
    CPlayer* player = nullptr;

    if (!v_sq_getentity(v, (SQEntity*)&player))
        return SQ_ERROR;

    const SQChar* key = nullptr;
    sq_getstring(v, 2, &key);

    if (!VALID_CHARSTAR(key))
    {
        v_SQVM_ScriptError("Empty or null class key");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    const SQChar* val = nullptr;
    sq_getstring(v, 3, &val);

    if (!VALID_CHARSTAR(val))
    {
        v_SQVM_ScriptError("Empty or null class value");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    CClient* const client = g_pServer->GetClient(player->GetEdict() - 1);
    SVC_SetClassVar msg(key, val);

    const bool success = client->SendNetMsgEx(&msg, false, true, false);

    if (success)
    {
        const char* pArgs[3] = {
            "_setClassVarServer",
            key,
            val
        };

        const CCommand cmd((int)V_ARRAYSIZE(pArgs), pArgs, cmd_source_t::kCommandSrcCode);
        const int oldIdx = *g_nCommandClientIndex;

        *g_nCommandClientIndex = client->GetUserID();
        v__setClassVarServer_f(cmd);

        *g_nCommandClientIndex = oldIdx;
    }

    sq_pushbool(v, success);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: checks if the provided hull type is valid
//-----------------------------------------------------------------------------
static bool Internal_ServerScript_ValidateHull(const SQInteger hull)
{
    if (hull < 0 || hull >= Hull_e::NUM_HULLS)
    {
        v_SQVM_ScriptError("Hull type with value %d does not index the maximum number of hulls(%d)", hull, Hull_e::NUM_HULLS);
        return false;
    }

    return true;
}

//-----------------------------------------------------------------------------
// Purpose: checks if the provided half-extents is valid
//-----------------------------------------------------------------------------
static bool Internal_ServerScript_NavMesh_GetExtents(HSQUIRRELVM v, const SQInteger stackIdx, rdVec3D* const out)
{
    const SQVector3D* extents = nullptr;
    sq_getvector(v, stackIdx, &extents);

    const SQFloat maxMagnitudeSqr = 9000000.0f;
    const SQFloat magnitudeSqr = extents->Dot();

    if (magnitudeSqr > maxMagnitudeSqr)
    {
        v_SQVM_ScriptError("Extents magnitude (%f) is too big. Max magnitude is %f", sqrtf(magnitudeSqr), sqrtf(maxMagnitudeSqr));
        return false;
    }

    if (extents->x <= 0.f || extents->y <= 0.f || extents->z <= 0.f)
    {
        v_SQVM_ScriptError("Extents elements (%f, %f, %f) must all be greater than zero", extents->x, extents->y, extents->z);
        return false;
    }

    out->init(extents->x, extents->y, extents->z);
    return true;
}

//-----------------------------------------------------------------------------
// Purpose: finds the nearest poly to given point, with an optional bounds filter.
//-----------------------------------------------------------------------------
static bool Internal_ServerScript_NavMesh_FindNearestPos(HSQUIRRELVM v, const bool useBounds)
{
    SQInteger hullIdx;
    sq_getinteger(v, useBounds ? 4 : 3, &hullIdx);

    if (!Internal_ServerScript_ValidateHull(hullIdx))
        return false;

    const Hull_e hullType = Hull_e(hullIdx);

    const NavMeshType_e navType = NAI_Hull::NavMeshType(hullType);
    const dtNavMesh* const nav = Detour_GetNavMeshByType(navType);

    if (!nav)
    {
        v_SQVM_ScriptError("NavMesh \"%s\" for hull \"%s\" hasn't been loaded!",
            NavMesh_GetNameForType(navType), g_aiHullNames[hullType]);

        return false;
    }

    rdVec3D halfExtents;

    if (useBounds)
    {
        if (!Internal_ServerScript_NavMesh_GetExtents(v, 3, &halfExtents))
            return false;
    }
    else
    {
        const Vector3D& maxs = NAI_Hull::Maxs(hullType);
        halfExtents.init(maxs.x, maxs.y, maxs.z);
    }

    const SQVector3D* point = nullptr;
    sq_getvector(v, 2, &point);

    const rdVec3D searchPoint(point->x, point->y, point->z);

    dtNavMeshQuery query;
    query.attachNavMeshUnsafe(nav);

    dtQueryFilter filter;
    filter.setIncludeFlags(DT_POLYFLAGS_ALL);
    filter.setExcludeFlags(DT_POLYFLAGS_DISABLED);

    dtPolyRef nearestRef;
    rdVec3D nearestPt;

    const dtStatus status = query.findNearestPoly(&searchPoint, &halfExtents, &filter, &nearestRef, &nearestPt);

    if (dtStatusFailed(status) || !nearestRef)
    {
        v->PushNull();
        return true;
    }

    const SQVector3D result(nearestPt.x, nearestPt.y, nearestPt.z);
    sq_pushvector(v, &result);

    return true;
}

//-----------------------------------------------------------------------------
// Purpose: finds the nearest polygon to provided point
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_NavMesh_GetNearestPos(HSQUIRRELVM v)
{
    const bool ret = Internal_ServerScript_NavMesh_FindNearestPos(v, false);

    if (!ret)
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: finds the nearest polygon to provided point within extents
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_NavMesh_GetNearestPosInBounds(HSQUIRRELVM v)
{
    const bool ret = Internal_ServerScript_NavMesh_FindNearestPos(v, true);

    if (!ret)
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}


//-----------------------------------------------------------------------------
// Purpose: adds an id to banlist
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_AddBanByID(HSQUIRRELVM v)
{
    const SQChar* nuc = nullptr;
    const SQChar* reason = nullptr;
    const SQChar* bannedByID = nullptr;

    sq_getstring( v, 2, &nuc);
    sq_getstring( v, 3, &reason  );
    sq_getstring( v, 4, &bannedByID );

    NucleusID_t parsedNucleusId = 0;
    if (!VALID_CHARSTAR(nuc) ||
        !g_BanSystem.Bansystem_ValidateInputID(nuc, parsedNucleusId) ||
        parsedNucleusId == 0)
    {
        v_SQVM_ScriptError("Invalid UID passed to AddBanByID");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    if ( !VALID_CHARSTAR( bannedByID ) )
        bannedByID = nullptr;

    if ( !VALID_CHARSTAR( reason ) )
        reason = nullptr;

    g_BanSystem.AddIdToBanlist(nuc, bannedByID, reason);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

std::atomic<int64_t> g_MatchID{ 0 };

//not exposed to sqvm
void setMatchID(int64_t newID)
{
    g_MatchID.store(newID);
}

//not exposed to sqvm
int64_t getMatchID()
{
    return g_MatchID.load();
}

// exposed to sqvm - retrieves matchID
static SQRESULT ServerScript_TrackerMatchID__internal(HSQUIRRELVM v)
{
    std::string matchIDStr = std::to_string(getMatchID());
    sq_pushstring(v, matchIDStr.c_str(), -1);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

// set match ID
static int64_t selfSetMatchID()
{
    const int64_t nowAsInt64 = static_cast<int64_t>(std::time(nullptr));
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int64_t> distr(1, INT64_MAX - nowAsInt64);

    const int64_t newID = distr(gen) + nowAsInt64;
    setMatchID(newID);

    return newID;
}

// Check of is currently running -- returns true if logging, false if not running
static SQRESULT ServerScript_TrackerIsLogging__internal(HSQUIRRELVM v)
{
    sq_pushbool(v, g_pTracker && g_pTracker->IsLogging());
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_TrackerGetLogState__internal(HSQUIRRELVM v) //probably should deprecate
{
    SQInteger flag = 0;

    if (g_pTracker && SQ_SUCCEEDED(sq_getinteger(v, 2, &flag)))
    {
        bool state = g_pTracker->GetLogState(g_pTracker->intToLogState(flag));
        sq_pushbool(v, state);
    }
    else
    {
        Error(eDLL_T::SERVER, NO_ERROR, "SQ_ERROR: SQ_GetLogState");
        sq_pushbool(v, SQFalse);
    }

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}



static SQRESULT ServerScript_TrackerLogEvent__internal(HSQUIRRELVM v) //Todo: Deprecate og logstrings and pull from squirrel global stat structs.
{
    const SQChar* logString = nullptr;
    SQBool encrypt = SQFalse;

    SQRESULT getStringResult = sq_getstring(v, 2, &logString);
    SQRESULT getBoolResult = sq_getbool(v, 3, &encrypt);

    if (SQ_SUCCEEDED(getStringResult) && SQ_SUCCEEDED(getBoolResult))
    {
        if (!VALID_CHARSTAR(logString))
        {
            v_SQVM_ScriptError("Invalid CHARSTAR during logging.");
            SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
        }

        if (!g_pTracker)
        {
            v_SQVM_ScriptError("Tracker logger is not initialized.");
            SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
        }

        g_pTracker->LogEvent(logString, encrypt != SQFalse);
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }
    else
    {
        v_SQVM_ScriptError("Error retrieving parameters in %s", __FUNCTION__);
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }
}


static SQRESULT ServerScript_InitializeTrackerLogThread__internal(HSQUIRRELVM v)
{
    SQBool encrypt = SQFalse;
    if (getMatchID() == 0)
        selfSetMatchID();

    if (SQ_FAILED(sq_getbool(v, 2, &encrypt)))
    {
        v_SQVM_ScriptError("Failed to retrieve 'encrypt' parameter in %s", __FUNCTION__);
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    if (!g_pTracker)
    {
        v_SQVM_ScriptError("Tracker logger is not initialized.");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    g_pTracker->InitializeLogThread(encrypt != SQFalse);

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}



static SQRESULT ServerScript_TrackerStopLogging__internal(HSQUIRRELVM v)
{
    SQBool sendToAPI = false;

    if (SQ_FAILED(sq_getbool(v, 2, &sendToAPI)))
    {
        v_SQVM_ScriptError("Failed to retrieve 'sendToAPI' parameter in %s", __FUNCTION__);
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    if (g_pTracker)
        g_pTracker->StopLogging(sendToAPI != SQFalse);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: deletes oldest logs after specified MB limit within specified
//          logfolder defined in settings json
//-----------------------------------------------------------------------------

static SQRESULT ServerScript_TrackerCleanupLogs__internal(HSQUIRRELVM v)
{
    TRACKER::CleanupLogs(FileSystem());
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: mkos debug - prints to console without devmode
//-----------------------------------------------------------------------------

static SQRESULT ServerScript_sqprint__internal(HSQUIRRELVM v)
{
    const SQChar* sqprintmsg = nullptr;

    if (SQ_FAILED(sq_getstring(v, 2, &sqprintmsg)) || !VALID_CHARSTAR(sqprintmsg))
    {
        v_SQVM_ScriptError("Failed to retrieve 'sqprintmsg' parameter in %s", __FUNCTION__);
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    std::string str = sqprintmsg;
    Msg(eDLL_T::SERVER, ":Tracker: %s\n", str.c_str());

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}


static SQRESULT ServerScript_sqerror__internal(HSQUIRRELVM v)
{
    const SQChar* sqprintmsg = nullptr;

    if (SQ_FAILED(sq_getstring(v, 2, &sqprintmsg)) || !VALID_CHARSTAR(sqprintmsg))
    {
        v_SQVM_ScriptError("Failed to retrieve 'sqprintmsg' parameter in %s.", __FUNCTION__);
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    std::string str = sqprintmsg;
    Error(eDLL_T::SERVER, NO_ERROR, ":Tracker: %s\n", str.c_str());

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_sqwarning__internal(HSQUIRRELVM v)
{
    const SQChar* sqprintmsg = nullptr;

    if (SQ_FAILED(sq_getstring(v, 2, &sqprintmsg)) || !VALID_CHARSTAR(sqprintmsg))
    {
        v_SQVM_ScriptError("Failed to retrieve 'sqprintmsg' parameter in %s", __FUNCTION__);
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    std::string str = sqprintmsg;
    Warning(eDLL_T::SERVER, ":Tracker: %s\n\n", str.c_str());

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: facilitates communication between sqvm and logger api calls
// for ea account verification
//-----------------------------------------------------------------------------

static SQRESULT ServerScript_TrackerEAVerify__internal(HSQUIRRELVM v)
{
    const SQChar* pToken = nullptr;
    const SQChar* pOID = nullptr;
    const SQChar* pEAName = nullptr;

    if (SQ_FAILED(sq_getstring(v, 2, &pToken)) || !pToken ||
        SQ_FAILED(sq_getstring(v, 3, &pOID)) || !pOID ||
        SQ_FAILED(sq_getstring(v, 4, &pEAName)) || !pEAName)
    {
        v_SQVM_ScriptError("Failed to retrieve parameters in %s", __FUNCTION__);
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    //We must make a copy here, because we are passing it to another thread, getting the stack object will not
    //matter even if we ref count it. This is because the server can reload / mapchange freeing the object
    //regardless of our ref counting.
    std::string token(pToken);
    std::string oid(pOID);
    std::string ea_name(pEAName);

    TrackerDispatch
    (
        [token = std::move(token), oid = std::move(oid), ea_name = std::move(ea_name)]()
        {
            int32_t status_num = 0;
            std::string status = TRACKER::VerifyEaAccount(token, oid, ea_name);

            try {
                status_num = std::stoi(status);
            }
            catch (const std::invalid_argument& e) {
                Msg(eDLL_T::SERVER, "Error: Invalid argument for conversion: %s in ServerScript_TrackerEAVerify__internal\n", e.what());
            }
            catch (const std::out_of_range& e) {
                Msg(eDLL_T::SERVER, "Error: Value out of range for conversion: %s in ServerScript_TrackerEAVerify__internal\n", e.what());
            }
            catch (...)
            {
                Msg(eDLL_T::SERVER, "Unknown error in ServerScript_TrackerEAVerify__internal\n");
            }

            g_TaskQueue.Dispatch
            (
                [oid, status_num]
                {
                    if (g_pServer && g_pServer->IsActive())
                    {
                        const char* const oidC = oid.c_str();
                        bool success = CALL_SERVER_SCRIPT_FUNC
                        (
                            "CodeCallback_VerifyEaAccount",
                            MakeNoCopyStr(oidC),
                            status_num,
                            "void functionref( string oid, int status )"
                        );

                        if (!success)
                            Error(eDLL_T::SERVER, NO_ERROR, "Failed to execute CodeCallback_VerifyEaAccount for oid '%s'.\n", oid.c_str());
                    }
                },
                0
           );
        }
    );

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}


//-----------------------------------------------------------------------------
// Purpose: api calls for stats
//-----------------------------------------------------------------------------

static SQRESULT ServerScript_TrackerUpdatePlayerCount__internal(HSQUIRRELVM v)
{
    const SQChar* action = nullptr;
    const SQChar* player = nullptr;
    const SQChar* OID = nullptr;
    const SQChar* count = nullptr;

    if (SQ_FAILED(sq_getstring(v, 2, &action)) || !action ||
        SQ_FAILED(sq_getstring(v, 3, &player)) || !player ||
        SQ_FAILED(sq_getstring(v, 4, &OID)) || !OID ||
        SQ_FAILED(sq_getstring(v, 5, &count)) || !count
    )
    {
        v_SQVM_ScriptError("Failed to retrieve parameters in %s", __FUNCTION__);
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    TRACKER::UPDATE_PLAYER_COUNT(action, player, OID, count);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}


static SQRESULT ServerScript_TrackerEndMatchUpdate__internal(HSQUIRRELVM v)
{
    const SQChar* recap = nullptr;

    if (SQ_FAILED(sq_getstring(v, 2, &recap)) || !recap )
    {
        v_SQVM_ScriptError("Failed to retrieve parameters in %s", __FUNCTION__);
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    TRACKER::NOTIFY_END_OF_MATCH(recap);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_FetchPlayerPersistenceData__internal(HSQUIRRELVM v)
{
    const SQChar* player_oid = nullptr;

    if (sq_gettype(v, 2) != OT_STRING)
    {
        v_SQVM_ScriptError("First parameter is expected to be of type string, but %s was provided.\n", IdType2Name(sq_gettype(v, 2)));
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }
    if (SQ_FAILED(sq_getstring(v, 2, &player_oid)) || !player_oid)
    {
        v_SQVM_ScriptError("Failed to retrieve 'player_oid' parameter.\n");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }
    if (sq_gettype(v, 3) != OT_ARRAY)
    {
        v_SQVM_ScriptError("Second parameter is expected to be of type array. %s provided.\n", IdType2Name(sq_gettype(v, 3)));
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }
    if (sq_gettype(v, 4) != OT_ARRAY)
    {
        v_SQVM_ScriptError("Third parameter is expected to be of type array. %s provided.\n", IdType2Name(sq_gettype(v, 4)));
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    std::string playerUID = player_oid;
    std::vector<std::string> requestedStats = SQArrayToVector<std::string>(v, 3);
    std::vector<std::string> requestedSettings = SQArrayToVector<std::string>(v, 4);

    TRACKER::TaskManager::getInstance().RequestPlayerPersistenceData
    (
        std::move( playerUID ),
        std::move( requestedStats ),
        std::move( requestedSettings )
    );

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_FetchBatchPersistenceData__internal(HSQUIRRELVM v)
{
    if (sq_gettype(v, 2) != OT_ARRAY)
    {
        v_SQVM_ScriptError("First parameter is expected to be of type array. %s provided.\n", IdType2Name(sq_gettype(v, 2)));
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }
    if (sq_gettype(v, 3) != OT_ARRAY)
    {
        v_SQVM_ScriptError("Second parameter is expected to be of type array. %s provided.\n", IdType2Name(sq_gettype(v, 3)));
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }
    if (sq_gettype(v, 4) != OT_ARRAY)
    {
        v_SQVM_ScriptError("Third parameter is expected to be of type array. %s provided.\n", IdType2Name(sq_gettype(v, 4)));
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    std::vector<std::string> playerOids = SQArrayToVector<std::string>(v, 2);
    std::vector<std::string> requestedStats = SQArrayToVector<std::string>(v, 3);
    std::vector<std::string> requestedSettings = SQArrayToVector<std::string>(v, 4);

    TRACKER::TaskManager::getInstance().RequestBatchPlayerPersistenceData(
        std::move(playerOids),
        std::move(requestedStats),
        std::move(requestedSettings));

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}


static SQRESULT ServerScript_GetPlayerPersistenceData__internal(HSQUIRRELVM v)
{
    const SQChar* player_oid = nullptr;

    if (SQ_FAILED(sq_getstring(v, 2, &player_oid)) || !player_oid)
     {
         v_SQVM_ScriptError("Failed to retrieve 'player_oid' parameter.\n");
         SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
     }

     std::string statsJson = TRACKER::GetPlayerJsonData(player_oid);

     if (statsJson.empty() || statsJson == "NA")
     {
         sq_newtable(v);
         SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
     }

     //Msg(eDLL_T::SERVER, "Received JSON: %s\n", statsJson.c_str());
     rapidjson::Document document;
     if (document.Parse(statsJson.c_str()).HasParseError())
    {
        Error(eDLL_T::SERVER, NO_ERROR, "JSON parsing failed: %s\n", rapidjson::GetParseError_En(document.GetParseError()));
        sq_newtable(v);
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }

    if (!document.IsObject())
    {
        Error(eDLL_T::SERVER, NO_ERROR, "JSON root is not an object\n");
        sq_newtable(v);
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }

    if (document.HasMember("error"))
    {
        const char* errorMsg = "";
        if (document["error"].IsString())
            errorMsg = document["error"].GetString();

        bool available = true; //assume marked available unless api response indicates stats are not available for this player.
        if (document.HasMember("data") && document["data"].IsObject())
        {
            if (document["data"].HasMember("available") && document["data"]["available"].IsBool())
                available = document["data"]["available"].GetBool();
        }

        if( available ) //only show non standard errors.
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Error fetching stats -- %s", errorMsg ? errorMsg : "");

        sq_newtable(v);
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }

    sq_newtable(v);
    for (rapidjson::Value::ConstMemberIterator itr = document.MemberBegin(); itr != document.MemberEnd(); ++itr)
    {
        const char* key = itr->name.GetString();
        if (!key || key[0] == '\0')
        {
            Msg(eDLL_T::SERVER, "Empty key in stats skipped\n");
            continue;
        }

        sq_pushstring(v, key, -1);
        const rapidjson::Value& value = itr->value;

        if (strcmp(key, "settings") == 0 && value.IsObject())
        {
            //Msg(eDLL_T::SERVER, "Settings found. Iterating.\n");
            sq_newtable(v);
            for (auto m = value.MemberBegin(); m != value.MemberEnd(); ++m)
            {
                const char* subKey = m->name.GetString();

                if (!VALID_CHARSTAR(subKey))
                    continue;

                sq_pushstring(v, subKey, -1);
                const rapidjson::Value& subVal = m->value;
                if (subVal.IsString())
                {
                    //Msg(eDLL_T::SERVER, "settings[%s] = %s\n", subKey, subVal.GetString());
                    sq_pushstring(v, subVal.GetString(), -1);
                }
                else if (subVal.IsInt())
                    sq_pushinteger(v, subVal.GetInt());
                else if (subVal.IsBool())
                    sq_pushbool(v, subVal.GetBool());
                else if (subVal.IsFloat() || subVal.IsDouble())
                    sq_pushfloat(v, static_cast<float>(subVal.GetDouble()));
                else
                {
                    Msg(eDLL_T::SERVER, "Unknown value type in settings table. Skipping\n");
                    sq_pop(v, 1);
                    continue;
                }

                sq_newslot(v, -3);
            }

            sq_newslot(v, -3);
        }
        else if (value.IsArray())
        {
            sq_newarray(v, 0);
            int arrayType = -1;
            auto getTypeName = [](int type) -> const char*
                {
                    switch (type)
                    {
                    case 0: return "int";
                    case 1: return "string";
                    case 2: return "bool";
                    case 3: return "float";
                    case 4: return "object";
                    case 5: return "unknown";
                    default: return "none";
                    }
                };

            for (rapidjson::SizeType i = 0; i < value.Size(); i++)
            {
                const rapidjson::Value& elem = value[i];
                int currentType = -1;
                if (elem.IsInt())
                    currentType = 0;
                else if (elem.IsString())
                    currentType = 1;
                else if (elem.IsBool())
                    currentType = 2;
                else if (elem.IsFloat() || elem.IsDouble())
                    currentType = 3;
                else if (elem.IsObject())
                    currentType = 4;
                else
                {
                    Warning(eDLL_T::SERVER, "Key '%s': Unsupported array element at index %zu\n", key, static_cast<size_t>(i));
                    continue;
                }

                if (arrayType == -1)
                    arrayType = currentType;
                else if (currentType != arrayType)
                {
                    Warning(eDLL_T::SERVER, "Key '%s': Cannot add element of type '%s' to array of type '%s'",
                        key, getTypeName(currentType), getTypeName(arrayType));
                    continue;
                }

                if (elem.IsInt())
                    sq_pushinteger(v, elem.GetInt());
                else if (elem.IsString())
                    sq_pushstring(v, elem.GetString(), -1);
                else if (elem.IsBool())
                    sq_pushbool(v, elem.GetBool());
                else if (elem.IsFloat() || elem.IsDouble())
                    sq_pushfloat(v, static_cast<float>(elem.GetDouble()));
                else if (elem.IsObject())
                {
                    sq_newtable(v);
                    for (auto objItr = elem.MemberBegin(); objItr != elem.MemberEnd(); ++objItr)
                    {
                        const char* objKey = objItr->name.GetString();
                        if (!objKey || objKey[0] == '\0')
                            continue;
                        sq_pushstring(v, objKey, -1);
                        const rapidjson::Value& objVal = objItr->value;
                        if (objVal.IsInt())
                            sq_pushinteger(v, objVal.GetInt());
                        else if (objVal.IsString())
                            sq_pushstring(v, objVal.GetString(), -1);
                        else if (objVal.IsBool())
                            sq_pushbool(v, objVal.GetBool());
                        else if (objVal.IsFloat() || objVal.IsDouble())
                            sq_pushfloat(v, static_cast<float>(objVal.GetDouble()));
                        else
                            sq_pushstring(v, "Invalid Stat Array Value", -1);
                        sq_newslot(v, -3);
                    }
                }
                sq_arrayappend(v, -2);
            }
            sq_newslot(v, -3);
        }
        else
        {
            if (value.IsInt())
                sq_pushinteger(v, value.GetInt());
            else if (value.IsString())
                sq_pushstring(v, value.GetString(), -1);
            else if (value.IsBool())
                sq_pushbool(v, value.GetBool());
            else if (value.IsFloat() || value.IsDouble())
                sq_pushfloat(v, static_cast<float>(value.GetDouble()));
            else
                sq_pushstring(v, "Invalid value type", -1);

            sq_newslot(v, -3);
        }
    }

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}


static SQRESULT ServerScript_TrackerUpdateLiveStats__internal(HSQUIRRELVM v)
{
    const SQChar* stats_json = nullptr;

    if (SQ_FAILED(sq_getstring(v, 2, &stats_json)) || !VALID_CHARSTAR(stats_json))
    {
        v_SQVM_ScriptError("Failed to get stats_json\n");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    std::string copyStatsJson(stats_json); //we must do this, as a thread is spawned to ship livestats

    TRACKER::UpdateLiveStats(copyStatsJson);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}



static SQRESULT ServerScript_TrackerResetStats__internal(HSQUIRRELVM v)
{
    const SQChar* player_oid = nullptr;
    if (SQ_SUCCEEDED(sq_getstring(v, 2, &player_oid)) && player_oid)
    {
        TRACKER::TaskManager::getInstance().ResetPlayerData(player_oid);
        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }

    SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
}


static SQRESULT ServerScript_FetchGlobalTrackerSettings__internal(HSQUIRRELVM v)
{
    const SQChar* query = nullptr;
    if (SQ_SUCCEEDED(sq_getstring(v, 2, &query)) && VALID_CHARSTAR(query))
    {
        std::string queryCopy(query);

        TrackerDispatch
        (
            [query = std::move(queryCopy)]
            {
                std::string settings = TRACKER::FetchGlobalSettings(query.c_str());

                g_TaskQueue.Dispatch([query, settings = std::move(settings)]
                {
                    bool success = CALL_SERVER_SCRIPT_FUNC("CodeCallback_TrackerGlobalSettingsReady", settings.c_str(), "void functionref( string )");
                    if (!success)
                        Error(eDLL_T::SERVER, NO_ERROR, "Failed to execute CodeCallback_TrackerGlobalSettingsReady for query '%s'.\n", query.c_str());
                }, 0 );
            }
        );

        SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
    }

    v_SQVM_ScriptError("Failed to get query string in %s", __FUNCTION__);
    SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
}




//-----------------------------------------------------------------------------
// Purpose: fetches a setting value by key in the settings map.
// loaded from (r5rdev_config)
//-----------------------------------------------------------------------------

static SQRESULT ServerScript_TrackerGetSetting__internal(HSQUIRRELVM v)
{
     const SQChar* setting_key = nullptr;
     if (!SQ_SUCCEEDED(sq_getstring(v, 2, &setting_key)) || !VALID_CHARSTAR(setting_key))
     {
         SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
     }

     if (strcmp(setting_key, "apikey") == 0)
     {
         sq_pushstring(v, "Cannot return apikey in script vm", -1);
     }
     else
     {
         std::string setting_value = TRACKER::GetSetting(setting_key);
         sq_pushstring(v, setting_value.c_str(), -1);
     }

     SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}


static SQRESULT ServerScript_TrackerReloadConfig__internal(HSQUIRRELVM v)
{
    TRACKER::ReloadConfig("r5rdev_config.json");
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_TrackerRestartWebsocket__internal(HSQUIRRELVM v)
{
    TrackerSocketSystem()->Shutdown();
    TrackerSocketSystem()->Reconnect();

    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

static SQRESULT ServerScript_ToggleTextBan(HSQUIRRELVM v)
{
    const SQChar* criteria = nullptr;
    const SQChar* reason = nullptr;
    const SQChar* expiry = nullptr;
    const SQChar* mutedBy = nullptr;
    SQBool toggleParam = SQFalse;
    SQInteger expiryUnixTimestamp = 0;

    if (SQ_FAILED(sq_getstring(v, 2, &criteria)) ||
        SQ_FAILED(sq_getstring(v, 3, &reason)) ||
        SQ_FAILED(sq_getstring(v, 4, &expiry)) ||
        SQ_FAILED(sq_getstring(v, 5, &mutedBy)) ||
        SQ_FAILED(sq_getbool(v, 6, &toggleParam)) ||
        SQ_FAILED(sq_getinteger(v, 7, &expiryUnixTimestamp)))
    {
        v_SQVM_ScriptError("Failed to retrieve parameters in %s", __FUNCTION__);
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    if (!VALID_CHARSTAR(criteria))
    {
        v_SQVM_ScriptError("Cannot toggle player mute, invalid criteria provided.");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    g_BanSystem.TextBanPlayer(criteria, reason, expiry, mutedBy, toggleParam != SQFalse, false, expiryUnixTimestamp);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//-----------------------------------------------------------------------------
// Purpose: saves a recorded animation on the disk to be used by bakery
//-----------------------------------------------------------------------------
static SQRESULT ServerScript_SaveRecordedAnimation(HSQUIRRELVM v)
{
    if (!developer->GetBool())
    {
        v_SQVM_ScriptError("SaveRecordedAnimation() is dev only!");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    AnimRecordingAssetHeader_s* const animRecording = v_ServerScript_GetRecordedAnimationFromCurrentStack(v);

    if (!animRecording)
    {
        v_SQVM_ScriptError("Parameter must be a recorded animation");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    if (animRecording->numRecordedFrames == 0)
    {
        v_SQVM_ScriptError("Recorded animation has 0 frames");
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    const SQChar* fileName = nullptr;
    sq_getstring(v, 3, &fileName);

    char fileNameBuf[MAX_OSPATH];
    const int fmtResult = snprintf(fileNameBuf, sizeof(fileNameBuf), "anim_recording/%s.anir", fileName);

    if (fmtResult < 0)
    {
        v_SQVM_ScriptError("Failed to format recorded animation file name; provided name \"%s\" is invalid", fileName);
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    FileSystem()->CreateDirHierarchy("anim_recording/", "MOD");
    FileHandle_t animRecordingFile = FileSystem()->Open(fileNameBuf, "wb", "MOD");

    if (animRecordingFile == FILESYSTEM_INVALID_HANDLE)
    {
        v_SQVM_ScriptError("Failed to open recorded animation file \"%s\" for write", fileNameBuf);
        SCRIPT_CHECK_AND_RETURN(v, SQ_ERROR);
    }

    AnimRecordingFileHeader_s fileHdr;

    fileHdr.magic = ANIR_FILE_MAGIC;
    fileHdr.fileVersion = ANIR_FILE_VERSION;
    fileHdr.assetVersion = ANIR_ASSET_VERSION;

    fileHdr.startPos = animRecording->startPos;
    fileHdr.startAngles = animRecording->startAngles;

    fileHdr.stringBufSize = 0;

    fileHdr.numElements = 0;
    fileHdr.numSequences = 0;

    fileHdr.numRecordedFrames = animRecording->numRecordedFrames;
    fileHdr.numRecordedOverlays = animRecording->numRecordedOverlays;

    fileHdr.animRecordingId = animRecording->animRecordingId;
    FileSystem()->Write(&fileHdr, sizeof(AnimRecordingFileHeader_s), animRecordingFile);

    // This information can only be retrieved by counting the number
    // of valid pose parameter names.
    int numElems = 0;
    int stringBufLen = 0;

    // Write out the pose parameters.
    for (int i = 0; i < ANIR_MAX_ELEMENTS; i++)
    {
        const char* const poseParamName = animRecording->poseParamNames[i];

        if (poseParamName)
            numElems++;
        else
            break;

        const ssize_t strLen = (ssize_t)strlen(poseParamName) + 1; // Include the null too.
        FileSystem()->Write(poseParamName, strLen, animRecordingFile);

        stringBufLen += (int)strLen;
    }

    // Write out the pose values.
    for (int i = 0; i < numElems; i++)
    {
        const Vector2D* poseParamValue = &animRecording->poseParamValues[i];
        FileSystem()->Write(poseParamValue, sizeof(Vector2D), animRecordingFile);
    }

    int numSeqs = 0;

    // Write out the animation sequence names.
    for (int i = 0; i < ANIR_MAX_SEQUENCES; i++)
    {
        const char* const animSequenceName = animRecording->animSequences[i];

        if (animSequenceName)
            numSeqs++;
        else
            break;

        const ssize_t strLen = (ssize_t)strlen(animSequenceName) + 1; // Include the null too.
        FileSystem()->Write(animSequenceName, strLen, animRecordingFile);

        stringBufLen += (int)strLen;
    }

    // Write out the recorded frames.
    for (int i = 0; i < animRecording->numRecordedFrames; i++)
    {
        assert(animRecording->recordedFrames);

        const AnimRecordingFrame_s* const frame = &animRecording->recordedFrames[i];
        FileSystem()->Write(frame, sizeof(AnimRecordingFrame_s), animRecordingFile);
    }

    // Write out the recorded overlays.
    for (int i = 0; i < animRecording->numRecordedOverlays; i++)
    {
        assert(animRecording->recordedOverlays);

        const AnimRecordingOverlay_s* const overlay = &animRecording->recordedOverlays[i];
        FileSystem()->Write(overlay, sizeof(AnimRecordingOverlay_s), animRecordingFile);
    }

    // Update the data in the header if we ended up writing
    // elements and sequences.
    if (numElems > 0 || numSeqs > 0)
    {
        FileSystem()->Seek(animRecordingFile, offsetof(AnimRecordingFileHeader_s, stringBufSize), FILESYSTEM_SEEK_HEAD);

        FileSystem()->Write(&stringBufLen, sizeof(int), animRecordingFile);
        FileSystem()->Write(&numElems, sizeof(int), animRecordingFile);
        FileSystem()->Write(&numSeqs, sizeof(int), animRecordingFile);
    }

    FileSystem()->Close(animRecordingFile);

    Msg(eDLL_T::SERVER, "Recorded animation saved to \"%s\"\n", fileNameBuf);
    SCRIPT_CHECK_AND_RETURN(v, SQ_OK);
}

//---------------------------------------------------------------------------------
// Purpose: registers script functions in SERVER context
// Input  : *s - 
//---------------------------------------------------------------------------------
void Script_RegisterServerFunctions(CSquirrelVM* s)
{
    Script_RegisterCommonAbstractions(s);
    Script_RegisterCoreServerFunctions(s);
    Script_RegisterAdminServerFunctions(s);

    Script_RegisterLiveAPIFunctions(s);

    // NOTE: plugin functions must always come after SDK functions!
    for (auto& callback : !PluginSystem()->GetRegisterServerScriptFuncsCallbacks())
    {
        // Register script functions inside plugins.
        callback.Function()(s);
    }
}

void Script_RegisterServerEnums(CSquirrelVM* const s)
{
    Script_RegisterLiveAPIEnums(s);
}

//---------------------------------------------------------------------------------
// Purpose: core server script functions
// Input  : *s - 
//---------------------------------------------------------------------------------
void Script_RegisterCoreServerFunctions(CSquirrelVM* s)
{
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, DebugDrawSolidBox, "Draw a debug overlay solid box", "void", "vector origin, vector mins, vector maxs, vector color, float alpha, bool drawThroughWorld, float duration", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, DebugDrawSweptBox, "Draw a debug overlay swept box", "void", "vector start, vector end, vector mins, vector maxs, vector angles, vector color, float alpha, bool drawThroughWorld, float duration", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, DebugDrawTriangle, "Draw a debug overlay triangle", "void", "vector p1, vector p2, vector p3, vector color, float alpha, bool drawThroughWorld, float duration", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, DebugDrawSolidSphere, "Draw a debug overlay solid sphere", "void", "vector origin, float radius, int theta, int phi, vector color, float alpha, bool drawThroughWorld, float duration", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, DebugDrawCapsule, "Draw a debug overlay capsule", "void", "vector start, vector end, float radius, vector color, float alpha, bool drawThroughWorld, float duration", false);

    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, SetAutoReloadState, "Set whether we can auto-reload the server", "void", "bool canAutoReload", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, GetSessionID, "Gets our current session ID", "string", "", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, SDK_WriteRuntimeBreadcrumb, "Writes runtime breadcrumb for crash diagnostics", "void", "string source, string detail", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, MatchReport_DebugNote, "Writes match report diagnostics when sv_match_report_debug is enabled", "void", "string message", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, MatchReport_RecordWeaponKill, "Records a weapon kill for match-end reporting", "void", "entity player, string weaponName", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, MatchReport_RecordPlayerKill, "Records a player kill event for match-end reporting", "void", "entity attacker, string victimUid, string victimName, string weaponName, int damageSourceId", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, MatchReport_RecordWeaponShot, "Records weapon shots for match-end reporting", "void", "entity player, string weaponName, int shots", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, MatchReport_RecordWeaponHit, "Records weapon hits and damage for match-end reporting", "void", "entity player, string weaponName, float damage, int hits, float bulletsHit, int headshots", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, MatchReport_RecordWeaponKillByUid, "Records a weapon kill for match-end reporting by stable player UID", "void", "string uid, string playerName, string weaponName", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, MatchReport_RecordPlayerKillByUid, "Records a player kill event for match-end reporting by stable player UIDs", "void", "string attackerUid, string attackerName, string victimUid, string victimName, string weaponName, int damageSourceId", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, MatchReport_RecordWeaponShotByUid, "Records weapon shots for match-end reporting by stable player UID", "void", "string uid, string playerName, string weaponName, int shots", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, MatchReport_RecordWeaponHitByUid, "Records weapon hits and damage for match-end reporting by stable player UID", "void", "string uid, string playerName, string weaponName, float damage, int hits, float bulletsHit, int headshots", false);

    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, NavMesh_GetNearestPos, "Finds the nearest position to the provided point on the hull's NavMesh using the hull's bounds as extents", "vector ornull", "vector searchPoint, int hullType", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, NavMesh_GetNearestPosInBounds, "Finds the nearest position to the provided point on the hull's NavMesh using provided bounds as extents", "vector ornull", "vector searchPoint, vector halfExtents, int hullType", false);

    //for stat settings (api keys, discord webhooks, server identifiers, preferences))
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, TrackerGetSetting__internal, "Fetches value by key", "string", "string", false );
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, TrackerReloadConfig__internal, "Reloads R5R.DEV config file", "void", "", false);

    //for tracker
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, InitializeTrackerLogThread__internal, "Initializes internal logevent thread", "void", "bool", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, TrackerLogEvent__internal, "Logs event with GameEvent,Encryption", "void", "string, bool", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, TrackerMatchID__internal, "Gets the match ID", "string", "", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, TrackerStopLogging__internal, "Stops the logging thread, writes remaining queued messages", "void", "bool", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, TrackerIsLogging__internal, "Checks if the log thread is running, atomic", "bool", "", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, TrackerGetLogState__internal, "Checks various states, returns true false", "bool", "int", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, TrackerCleanupLogs__internal, "Deletes oldest logs in platform/eventlogs when directory exceeds 20mb", "void", "", false);

    // for tracker api
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, FetchGlobalTrackerSettings__internal, "Fetches global settings based on query", "void", "string", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, FetchBatchPersistenceData__internal, "Fetches batch player stats queries", "void", "array< string >, array< string >, array< string >", false); //specify what stats|settings
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, FetchPlayerPersistenceData__internal, "Initializes grabbing stats for player", "void", "string, array< string >, array< string >", false); //specify what stats|settings
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, GetPlayerPersistenceData__internal, "Gets stats table for player from native map", "table", "string", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, TrackerResetStats__internal, "Sets map value for player_oid stats to empty string", "void", "string", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, TrackerUpdateLiveStats__internal, "Updates live server stats R5R.DEV", "void", "string", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, TrackerUpdatePlayerCount__internal, "Updates LIVE player count on R5R.DEV", "void", "string, string, string, string", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, TrackerEndMatchUpdate__internal, "Updates match recap on R5R.DEV", "void", "string", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, TrackerEAVerify__internal, "Verifys EA Account on R5R.DEV", "void", "string, string, string", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, TrackerRestartWebsocket__internal, "Restarts the websocket from scripts.", "void", "", false);

    // for debugging the sqvm
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, sqprint__internal, "Prints string to console window from sqvm", "void", "string", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, sqerror__internal, "Prints error string to console window from sqvm", "void", "string", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, sqwarning__internal, "Prints warning string to console window from sqvm", "void", "string", false);
    //default sdk
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, SaveRecordedAnimation, "Saves an anim_recording asset to be used by bakery. (dev only)", "void", "var recordedAnim, string fileName", false);
}

//---------------------------------------------------------------------------------
// Purpose: admin server script functions
// Input  : *s - 
//---------------------------------------------------------------------------------
void Script_RegisterAdminServerFunctions(CSquirrelVM* s)
{
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, GetNumHumanPlayers, "Gets the number of human players on the server", "int", "", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, GetNumFakeClients, "Gets the number of bot players on the server", "int", "", false);

    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, KickPlayerByName, "Kicks a player from the server by name", "void", "string name, string reason", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, KickPlayerById, "Kicks a player from the server by handle or nucleus id", "void", "string id, string reason", false);

    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, BanPlayerByName, "Bans a player from the server by name", "void", "string name, string reason, ...", true);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, BanPlayerById, "Bans a player from the server by handle or nucleus id", "void", "string id, string reason, ...", true);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, BanPlayerByIdOnly, "Bans a player from the server by handle or nucleus id", "void", "string id, string reason, string bannedByID", false); //This should be removed and add an optional bool to original

    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, AddBanByID, "Adds a player to the banlist by nucleus id", "void", "string nucleusId, string reason, string bannedByID", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, UnbanPlayer, "Unbans a player from the server by nucleus id or ip address", "void", "string handle", false);

    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, BroadcastServerTextMessage, "Broadcasts a chatmessage to all clients", "void", "string prefix, string message, bool adminMsg", false);
    DEFINE_SERVER_SCRIPTFUNC_NAMED(s, ToggleTextBan, "Toggles a text mute/unmute for a client.", "void", "string criteria, string reason, string expiry, string mutedBy, bool toggle, int expiryUnixTimestamp", false);
}

//---------------------------------------------------------------------------------
// Purpose: script code class function registration
//---------------------------------------------------------------------------------
static void Script_RegisterServerEntityClassFuncs()
{
    v_Script_RegisterServerEntityClassFuncs();
    static bool initialized = false;

    if (initialized)
        return;

    initialized = true;
}
//---------------------------------------------------------------------------------
static void Script_RegisterServerPlayerClassFuncs()
{
    v_Script_RegisterServerPlayerClassFuncs();
    static bool initialized = false;

    if (initialized)
        return;

    initialized = true;

    g_serverScriptPlayerStruct->AddFunction("SetClassVar",
        "ScriptSetClassVar",
        "Change a variable in the player's class settings",
        "bool",
        "string key, string value",
        false,
        ServerScript_ScriptSetClassVar);

    g_serverScriptPlayerStruct->AddFunction("SendServerTextMessage",
        "ScriptSendServerTextMessage",
        "Sends a chat message to a player",
        "bool",
        "string prefix, string message, bool adminMsg",
        false,
        ServerScript_SendServerTextMessage);

    g_serverScriptPlayerStruct->AddFunction("IsMuted",
        "ScriptIsMuted",
        "Returns if the user is comms banned",
        "bool",
        "",
        false,
        ServerScript_IsMuted);

}
//---------------------------------------------------------------------------------
static void Script_RegisterServerAIClassFuncs()
{
    v_Script_RegisterServerAIClassFuncs();
    static bool initialized = false;

    if (initialized)
        return;

    initialized = true;
}
//---------------------------------------------------------------------------------
static void Script_RegisterServerWeaponClassFuncs()
{
    v_Script_RegisterServerWeaponClassFuncs();
    static bool initialized = false;

    if (initialized)
        return;

    initialized = true;
}
//---------------------------------------------------------------------------------
static void Script_RegisterServerProjectileClassFuncs()
{
    v_Script_RegisterServerProjectileClassFuncs();
    static bool initialized = false;

    if (initialized)
        return;

    initialized = true;
}
//---------------------------------------------------------------------------------
static void Script_RegisterServerTitanSoulClassFuncs()
{
    v_Script_RegisterServerTitanSoulClassFuncs();
    static bool initialized = false;

    if (initialized)
        return;

    initialized = true;
}
//---------------------------------------------------------------------------------
static void Script_RegisterServerPlayerDecoyClassFuncs()
{
    v_Script_RegisterServerPlayerDecoyClassFuncs();
    static bool initialized = false;

    if (initialized)
        return;

    initialized = true;
}
//---------------------------------------------------------------------------------
static void Script_RegisterServerSpawnpointClassFuncs()
{
    v_Script_RegisterServerSpawnpointClassFuncs();
    static bool initialized = false;

    if (initialized)
        return;

    initialized = true;
}
//---------------------------------------------------------------------------------
static void Script_RegisterServerFirstPersonProxyClassFuncs()
{
    v_Script_RegisterServerFirstPersonProxyClassFuncs();
    static bool initialized = false;

    if (initialized)
        return;

    initialized = true;
}

void VScriptServer::Detour(const bool bAttach) const
{
    DetourSetup(&v_ServerScript_DebugScreenText, &ServerScript_DebugScreenText, bAttach);
    DetourSetup(&v_ServerScript_DebugScreenTextWithColor, &ServerScript_DebugScreenTextWithColor, bAttach);

    DetourSetup(&v_Script_RegisterServerEntityClassFuncs, &Script_RegisterServerEntityClassFuncs, bAttach);
    DetourSetup(&v_Script_RegisterServerPlayerClassFuncs, &Script_RegisterServerPlayerClassFuncs, bAttach);
    DetourSetup(&v_Script_RegisterServerAIClassFuncs, &Script_RegisterServerAIClassFuncs, bAttach);
    DetourSetup(&v_Script_RegisterServerWeaponClassFuncs, &Script_RegisterServerWeaponClassFuncs, bAttach);
    DetourSetup(&v_Script_RegisterServerProjectileClassFuncs, &Script_RegisterServerProjectileClassFuncs, bAttach);
    DetourSetup(&v_Script_RegisterServerTitanSoulClassFuncs, &Script_RegisterServerTitanSoulClassFuncs, bAttach);
    DetourSetup(&v_Script_RegisterServerPlayerDecoyClassFuncs, &Script_RegisterServerPlayerDecoyClassFuncs, bAttach);
    DetourSetup(&v_Script_RegisterServerSpawnpointClassFuncs, &Script_RegisterServerSpawnpointClassFuncs, bAttach);
    DetourSetup(&v_Script_RegisterServerFirstPersonProxyClassFuncs, &Script_RegisterServerFirstPersonProxyClassFuncs, bAttach);
}
