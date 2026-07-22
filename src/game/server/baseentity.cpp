//======= Copyright (c) 1996-2009, Valve Corporation, All rights reserved. ======
//
// Purpose: The base class from which all game entities are derived.
//
//===============================================================================
#include "core/stdafx.h"
#include "baseentity.h"
#include "engine/gl_model_private.h"
#include "engine/modelinfo.h"

//-----------------------------------------------------------------------------
// 
//-----------------------------------------------------------------------------
CCollisionProperty* CBaseEntity::CollisionProp()
{
	return &m_Collision;
}

//-----------------------------------------------------------------------------
// 
//-----------------------------------------------------------------------------
const CCollisionProperty* CBaseEntity::CollisionProp() const
{
	return &m_Collision;
}

//-----------------------------------------------------------------------------
// 
//-----------------------------------------------------------------------------
CServerNetworkProperty* CBaseEntity::NetworkProp()
{
	return &m_Network;
}

//-----------------------------------------------------------------------------
// 
//-----------------------------------------------------------------------------
const CServerNetworkProperty* CBaseEntity::NetworkProp() const
{
	return &m_Network;
}

//-----------------------------------------------------------------------------
// 
//-----------------------------------------------------------------------------
model_t* CBaseEntity::GetModel(void)
{
	return (model_t*)g_pModelInfoServer->GetModel(GetModelIndex());
}

const HSCRIPT CBaseEntity::GetScriptInstance()
{
	return v_CBaseEntity__GetScriptInstance(this);
}

bool CBaseEntity::DiscardFirstEntityLink(void)
{
	// Entity links use a 14-bit hash-table handle rather than an EHANDLE.
	constexpr int INVALID_ENTITY_LINK_INDEX = 0x3FFF;

	if (m_firstChildEntityLink != INVALID_ENTITY_LINK_INDEX)
	{
		m_firstChildEntityLink = INVALID_ENTITY_LINK_INDEX;
		return true;
	}

	if (m_firstParentEntityLink != INVALID_ENTITY_LINK_INDEX)
	{
		m_firstParentEntityLink = INVALID_ENTITY_LINK_INDEX;
		return true;
	}

	return false;
}

static void CBaseEntity_UnlinkFromEnt(CBaseEntity* const thisp, CBaseEntity* const pOther)
{
	if (!thisp)
		return;

	if (pOther)
	{
		v_CBaseEntity__UnlinkFromEnt(thisp, pOther);
		return;
	}

	if (thisp->DiscardFirstEntityLink())
	{
		Warning(eDLL_T::SERVER,
			"CBaseEntity::UnlinkFromEnt: discarded a stale entity link for '%p' because the linked entity no longer exists.\n",
			static_cast<void*>(thisp));
	}
}

void VCBaseEntity::Detour(const bool bAttach) const
{
	DetourSetup(&v_CBaseEntity__UnlinkFromEnt, &CBaseEntity_UnlinkFromEnt, bAttach);
}
