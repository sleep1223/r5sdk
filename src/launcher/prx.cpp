#include <core/stdafx.h>
#include <core/init.h>
#include <core/logger.h>
#include <launcher/prx.h>

//-----------------------------------------------------------------------------
// Purpose: shutdown and unload SDK
//-----------------------------------------------------------------------------
void h_exit_or_terminate_process(UINT uExitCode)
{
	SDK_WriteProcessExitDiagnostic("game exit_or_terminate_process", uExitCode);

	if (v_exit_or_terminate_process)
		v_exit_or_terminate_process(uExitCode);

	TerminateProcess(GetCurrentProcess(), uExitCode);
}

void VPRX::Detour(const bool bAttach) const
{
	if (v_exit_or_terminate_process)
		DetourSetup(&v_exit_or_terminate_process, &h_exit_or_terminate_process, bAttach);
}
