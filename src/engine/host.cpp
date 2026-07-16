//===== Copyright © 1996-2005, Valve Corporation, All rights reserved. ========//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================//

#include "core/stdafx.h"
#include "tier0/frametask.h"
#include "engine/cmd.h"
#include "engine/host.h"
#include "engine/debugoverlay.h"
#ifndef CLIENT_DLL
#include "server/server.h"
#endif // !CLIENT_DLL
#ifndef DEDICATED
#include "client/clientstate.h"
#include "windows/id3dx.h"
#include "geforce/reflex.h"
#include "vgui/vgui_debugpanel.h"
#include "materialsystem/cmaterialsystem.h"
#endif // !DEDICATED

CCommonHostState* g_pCommonHostState = nullptr;

void CCommonHostState::SetWorldModel(model_t* pModel)
{
	if (worldmodel == pModel)
		return;

	worldmodel = pModel;
	if (pModel)
	{
		worldbrush = pModel->brush.pShared;
	}
	else
	{
		worldbrush = NULL;
	}
}

/*
==================
Host_CountRealTimePackets

Counts the number of
packets in non-prescaled
clock frames (does not
count for bots or Terminal
Services environments)
==================
*/
void Host_CountRealTimePackets()
{
	v_Host_CountRealTimePackets();
#ifndef DEDICATED
	GeForce_SetLatencyMarker(D3D11Device(), SIMULATION_START, MaterialSystem()->GetCurrentFrameCount());
#endif // !DEDICATED
}

/*
==================
_Host_RunFrame

Runs all active servers
==================
*/
void _Host_RunFrame(void* unused, const float deltaTime)
{
	for (IFrameTask* const& task : g_TaskQueueList)
	{
		task->RunFrame();
	}

	g_TaskQueueList.erase(std::remove_if(g_TaskQueueList.begin(), g_TaskQueueList.end(), [](const IFrameTask* task)
		{
			return task->IsFinished();
		}), g_TaskQueueList.end());

#ifndef DEDICATED
	g_TextOverlay.ShouldDraw(deltaTime);
#endif // !DEDICATED

#ifdef DEDICATED
	DebugOverlay_HandleDecayed();
#endif // DEDICATED

	v_Host_RunFrame(unused, deltaTime);
}

void Host_Error(const char* const error, ...)
{
	char buf[1024];
	{/////////////////////////////
		va_list args{};
		va_start(args, error);

		const int ret = V_vsnprintf(buf, sizeof(buf), error, args);

		if (ret < 0)
			buf[0] = '\0';

		va_end(args);
	}/////////////////////////////

	Error(eDLL_T::ENGINE, NO_ERROR, "Host_Error: %s", buf);
	v_Host_Error(buf);
}

void Host_ReparseAllScripts()
{
	// NOTE: the following are already called during "reload" or "reconnect".
	//"aisettings_reparse"
	//"aisettings_reparse_client"

	//"damagedefs_reparse"
	//"damagedefs_reparse_client"

	//"playerSettings_reparse"
	//"fx_impact_reparse"

#ifndef DEDICATED
	// Reparse banks.rson
	Cbuf_AddText(Cbuf_GetCurrentPlayer(), "miles_reboot", cmd_source_t::kCommandSrcCode);
#endif // !DEDICATED

	Cbuf_AddText(Cbuf_GetCurrentPlayer(), "downloadPlaylists", cmd_source_t::kCommandSrcCode);
	Cbuf_AddText(Cbuf_GetCurrentPlayer(), "banlist_reload", cmd_source_t::kCommandSrcCode);

	Cbuf_AddText(Cbuf_GetCurrentPlayer(), "ReloadAimAssistSettings", cmd_source_t::kCommandSrcCode);
	Cbuf_AddText(Cbuf_GetCurrentPlayer(), "reload_localization", cmd_source_t::kCommandSrcCode);

	Cbuf_AddText(Cbuf_GetCurrentPlayer(), "weapon_reparse", cmd_source_t::kCommandSrcCode);

#ifndef DEDICATED
	// Recompile all UI scripts
	Cbuf_AddText(Cbuf_GetCurrentPlayer(), "uiscript_reset", cmd_source_t::kCommandSrcCode);
#endif // !DEDICATED

	bool serverActive = false;

#ifndef CLIENT_DLL
	if (g_pServer->IsActive())
	{
		// If we hit this code path, we are the server (or the listen server),
		// reload it to recompile all scripts.
		Cbuf_AddText(Cbuf_GetCurrentPlayer(), "reload", cmd_source_t::kCommandSrcCode);
		serverActive = true;
	}
#endif // !CLIENT_DLL
#ifndef DEDICATED
	if (!serverActive && g_pClientState->IsActive())
	{
		// If we hit this code path, we are connected to a remote server,
		// reconnect to it to recompile all client side scripts.
		Cbuf_AddText(Cbuf_GetCurrentPlayer(), "reconnect", cmd_source_t::kCommandSrcCode);
	}
#endif // !DEDICATED

	Cbuf_Execute();
}

void Host_Reload()
{
#ifndef CLIENT_DLL
	if (g_pServer && g_pServer->IsActive())
		Cbuf_AddText(Cbuf_GetCurrentPlayer(), "reload\n", cmd_source_t::kCommandSrcCode);
#endif
}

///////////////////////////////////////////////////////////////////////////////
void VHost::Detour(const bool bAttach) const
{
	DetourSetup(&v_Host_RunFrame, &_Host_RunFrame, bAttach);
	DetourSetup(&v_Host_CountRealTimePackets, &Host_CountRealTimePackets, bAttach);

#ifndef DEDICATED // Dedicated already logs this!
	DetourSetup(&v_Host_Error, &Host_Error, bAttach);
#endif // !DEDICATED
}
