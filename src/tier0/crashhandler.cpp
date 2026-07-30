//=============================================================================//
//
// Purpose: Crash handler (overrides the game's implementation!)
//
//=============================================================================//
#include "tier0/binstream.h"
#include "tier0/cpu.h"
#include "tier0/crashhandler.h"

static bool CrashHandler_EnsureSymbolsInitialized()
{
	static bool s_bAttempted = false;
	static bool s_bInitialized = false;

	if (s_bAttempted)
		return s_bInitialized;

	s_bAttempted = true;

	const HANDLE hProcess = GetCurrentProcess();
	SymSetOptions(SymGetOptions() | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);

	if (SymInitialize(hProcess, nullptr, TRUE))
	{
		s_bInitialized = true;
		return true;
	}

	// DbgHelp reports ERROR_INVALID_PARAMETER when the process is already
	// initialized by another component. Symbol lookups are still valid then.
	if (GetLastError() == ERROR_INVALID_PARAMETER)
		s_bInitialized = true;

	return s_bInitialized;
}

static const char* CrashHandler_BaseFileName(const char* const pszPath)
{
	if (!pszPath)
		return "";

	const char* const pszBackSlash = strrchr(pszPath, '\\');
	const char* const pszForwardSlash = strrchr(pszPath, '/');
	const char* pszSlash = pszBackSlash;
	if (!pszSlash || (pszForwardSlash && pszForwardSlash > pszSlash))
		pszSlash = pszForwardSlash;

	return pszSlash ? pszSlash + 1 : pszPath;
}

static void CrashHandler_EnsureDirectoryExists(const char* const pszDirectory)
{
	if (!pszDirectory || !*pszDirectory)
		return;

	char szDirectory[MAX_PATH];
	V_strncpy(szDirectory, pszDirectory, sizeof(szDirectory));
	szDirectory[sizeof(szDirectory) - 1] = '\0';

	for (char* p = szDirectory; *p; ++p)
	{
		if ((*p != '\\' && *p != '/') || p == szDirectory || *(p - 1) == ':')
			continue;

		const char ch = *p;
		*p = '\0';
		CreateDirectoryA(szDirectory, nullptr);
		*p = ch;
	}

	CreateDirectoryA(szDirectory, nullptr);
}

static void CrashHandler_WriteTextFile(const char* const pszPath, const char* const pszText, const DWORD nTextLength)
{
	const HANDLE hTxtFile = CreateFileA(pszPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hTxtFile == INVALID_HANDLE_VALUE)
		return;

	::WriteFile(hTxtFile, pszText, nTextLength, nullptr, nullptr);
	CloseHandle(hTxtFile);
}

static bool CrashHandler_ReadMemory(const DWORD64 nAddress, void* const pBuffer, const SIZE_T nSize)
{
	SIZE_T nBytesRead = 0;
	return nAddress &&
		ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(nAddress), pBuffer, nSize, &nBytesRead) &&
		nBytesRead == nSize;
}

static bool CrashHandler_ReadSQString(const DWORD64 nStringObject, char* const pszBuffer, const SIZE_T nBufferSize)
{
	if (!pszBuffer || nBufferSize < 2)
		return false;

	pszBuffer[0] = '\0';

	SIZE_T nBytesRead = 0;
	if (!nStringObject ||
		!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(nStringObject + 0x40),
			pszBuffer, nBufferSize - 1, &nBytesRead) ||
		nBytesRead == 0)
	{
		return false;
	}

	const SIZE_T nEnd = nBytesRead < nBufferSize ? nBytesRead : nBufferSize - 1;
	pszBuffer[nEnd] = '\0';

	for (SIZE_T i = 0; i < nEnd && pszBuffer[i]; ++i)
	{
		const unsigned char ch = static_cast<unsigned char>(pszBuffer[i]);
		if (ch < 0x20 || ch > 0x7E)
			pszBuffer[i] = '?';
	}

	return true;
}

static bool CrashHandler_WriteMiniDumpFile(const char* const pszPath, EXCEPTION_POINTERS* const pExceptionPointers)
{
	const HANDLE hDmpFile = CreateFileA(pszPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (hDmpFile == INVALID_HANDLE_VALUE)
		return false;

	MINIDUMP_EXCEPTION_INFORMATION dumpExceptionInfo;
	dumpExceptionInfo.ThreadId = GetCurrentThreadId();
	dumpExceptionInfo.ExceptionPointers = pExceptionPointers;
	dumpExceptionInfo.ClientPointers = false;

	const MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
		MiniDumpNormal |
		MiniDumpWithHandleData |
		MiniDumpWithProcessThreadData |
		MiniDumpWithThreadInfo |
		MiniDumpWithUnloadedModules |
		MiniDumpIgnoreInaccessibleMemory);

	const BOOL bWroteDump = MiniDumpWriteDump(
		GetCurrentProcess(),
		GetCurrentProcessId(),
		hDmpFile, dumpType,
		&dumpExceptionInfo, nullptr, nullptr);

	CloseHandle(hDmpFile);

	if (!bWroteDump)
		DeleteFileA(pszPath);

	return bWroteDump == TRUE;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CCrashHandler::Start()
{
	AcquireSRWLockExclusive(&m_Lock);
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CCrashHandler::End()
{
	ReleaseSRWLockExclusive(&m_Lock);
}

//-----------------------------------------------------------------------------
// Purpose: formats the crasher (module, address and exception)
//-----------------------------------------------------------------------------
void CCrashHandler::FormatCrash()
{
	m_Buffer.Append("crash:\n{\n");

	FormatExceptionAddress();
	FormatExceptionCode();

	m_Buffer.Append("}\n");
}

//-----------------------------------------------------------------------------
// Purpose: formats the captured callstack
//-----------------------------------------------------------------------------
void CCrashHandler::FormatCallstack()
{
	m_Buffer.Append("callstack:\n{\n");

	if (m_nCapturedFrames)
	{
		const PEXCEPTION_RECORD pExceptionRecord = m_pExceptionPointers->ExceptionRecord;

		if (m_ppStackTrace[m_nCapturedFrames - 1] == pExceptionRecord->ExceptionAddress)
		{
			const PCONTEXT pContextRecord = m_pExceptionPointers->ContextRecord;
			MEMORY_BASIC_INFORMATION mbi = { 0 };

			const SIZE_T t = VirtualQuery((LPCVOID)pContextRecord->Rsp, &mbi, sizeof(LPCVOID));

			if (t >= sizeof(mbi)
				&& !(mbi.Protect & PAGE_NOACCESS)
				&& (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE))
				&& (mbi.State & MEM_COMMIT))
			{
				m_Buffer.Append("\t// call stack ended; possible return address?\n");
			}
		}
	}
	for (WORD i = 0; i < m_nCapturedFrames; i++)
	{
		m_Buffer.AppendFormat("\t#%02u ", i);
		FormatExceptionAddress(reinterpret_cast<LPCSTR>(m_ppStackTrace[i]), false);
	}

	if (!m_nCapturedFrames)
		m_Buffer.Append("\t<no frames captured>\n");

	m_Buffer.Append("}\n");
}

//-----------------------------------------------------------------------------
// Purpose: formats all the registers and their contents
//-----------------------------------------------------------------------------
void CCrashHandler::FormatRegisters()
{
	m_Buffer.Append("registers:\n{\n");
	const PCONTEXT pContextRecord = m_pExceptionPointers->ContextRecord;

	FormatALU("rax", pContextRecord->Rax);
	FormatALU("rbx", pContextRecord->Rbx);
	FormatALU("rcx", pContextRecord->Rcx);
	FormatALU("rdx", pContextRecord->Rdx);
	FormatALU("rsp", pContextRecord->Rsp);
	FormatALU("rbp", pContextRecord->Rbp);
	FormatALU("rsi", pContextRecord->Rsi);
	FormatALU("rdi", pContextRecord->Rdi);
	FormatALU("r8 ", pContextRecord->R8);
	FormatALU("r9 ", pContextRecord->R9);
	FormatALU("r10", pContextRecord->R10);
	FormatALU("r11", pContextRecord->R11);
	FormatALU("r12", pContextRecord->R12);
	FormatALU("r13", pContextRecord->R13);
	FormatALU("r14", pContextRecord->R14);
	FormatALU("r15", pContextRecord->R15);
	FormatALU("rip", pContextRecord->Rip);

	FormatFPU("xmm0 ", &pContextRecord->Xmm0);
	FormatFPU("xmm1 ", &pContextRecord->Xmm1);
	FormatFPU("xmm2 ", &pContextRecord->Xmm2);
	FormatFPU("xmm3 ", &pContextRecord->Xmm3);
	FormatFPU("xmm4 ", &pContextRecord->Xmm4);
	FormatFPU("xmm5 ", &pContextRecord->Xmm5);
	FormatFPU("xmm6 ", &pContextRecord->Xmm6);
	FormatFPU("xmm7 ", &pContextRecord->Xmm7);
	FormatFPU("xmm8 ", &pContextRecord->Xmm8);
	FormatFPU("xmm9 ", &pContextRecord->Xmm9);
	FormatFPU("xmm10", &pContextRecord->Xmm10);
	FormatFPU("xmm11", &pContextRecord->Xmm11);
	FormatFPU("xmm12", &pContextRecord->Xmm12);
	FormatFPU("xmm13", &pContextRecord->Xmm13);
	FormatFPU("xmm14", &pContextRecord->Xmm14);
	FormatFPU("xmm15", &pContextRecord->Xmm15);

	m_Buffer.Append("}\n");
}

//-----------------------------------------------------------------------------
// Purpose: formats memory needed to reconstruct the active Squirrel VM frame
//-----------------------------------------------------------------------------
void CCrashHandler::FormatExceptionMemory()
{
	m_Buffer.Append("exception_memory:\n{\n");

	if (!m_pExceptionPointers || !m_pExceptionPointers->ContextRecord)
	{
		m_Buffer.Append("\t<no exception context>\n");
		m_Buffer.Append("}\n");
		return;
	}

	const PCONTEXT pContext = m_pExceptionPointers->ContextRecord;
	FormatMemoryBlock("exception_stack", pContext->Rsp, 0x200);

	const DWORD64 nGameBase = reinterpret_cast<DWORD64>(GetModuleHandleA(nullptr));
	const DWORD64 nCrashRva = nGameBase && pContext->Rip >= nGameBase ? pContext->Rip - nGameBase : 0;
	m_Buffer.AppendFormat("\tgame_rva: 0x%llX\n", nCrashRva);

	// These offsets describe the SQObjectPtr assignment used by the Squirrel
	// interpreter. Other crashes still get the exception stack above.
	if (nCrashRva < 0xB1D2D0 || nCrashRva >= 0xB1D320)
	{
		m_Buffer.Append("\tsqvm_targeted: false\n");
		m_Buffer.Append("}\n");
		return;
	}

	m_Buffer.Append("\tsqvm_targeted: true\n");

	const DWORD64 nInstructionWindow = pContext->R14 >= 0x40 ? pContext->R14 - 0x40 : pContext->R14;
	const DWORD64 nDestinationWindow = pContext->Rcx >= 0x40 ? pContext->Rcx - 0x40 : pContext->Rcx;
	const DWORD64 nSourceWindow = pContext->Rdx >= 0x40 ? pContext->Rdx - 0x40 : pContext->Rdx;

	FormatMemoryBlock("sqvm", pContext->Rsi, 0x180);
	FormatMemoryBlock("current_instruction", nInstructionWindow, 0x100);
	FormatMemoryBlock("destination_object", nDestinationWindow, 0x80);
	FormatMemoryBlock("source_object", nSourceWindow, 0x80);

	DWORD64 nInstructionState = 0;
	DWORD64 nLiteralBase = 0;
	DWORD64 nStackBase = 0;
	SIZE_T nBytesRead = 0;

	if (pContext->Rsi &&
		ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(pContext->Rsi + 0x40),
			&nInstructionState, sizeof(nInstructionState), &nBytesRead) &&
		nBytesRead == sizeof(nInstructionState))
	{
		FormatMemoryBlock("sqvm_instruction_state", nInstructionState, 0x80);

		DWORD64 nInstructionStateValues[2] = {};

		if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(nInstructionState),
			nInstructionStateValues, sizeof(nInstructionStateValues), &nBytesRead) &&
			nBytesRead == sizeof(nInstructionStateValues))
		{
			const DWORD64 nInstruction = nInstructionStateValues[0];
			nLiteralBase = nInstructionStateValues[1];
			const DWORD64 nInstructionCursor = nInstruction >= 0x40 ? nInstruction - 0x40 : nInstruction;

			FormatMemoryBlock("sqvm_instruction_cursor", nInstructionCursor, 0x100);
			FormatMemoryBlock("sqvm_literal_base", nLiteralBase, 0x200);
		}
	}

	struct SQObjectSnapshot
	{
		DWORD64 nType;
		DWORD64 nValue;
	};

	SQObjectSnapshot closureObject = {};
	SQObjectSnapshot functionProtoObject = {};
	SQObjectSnapshot sourceNameObject = {};
	SQObjectSnapshot functionNameObject = {};

	if (nInstructionState &&
		CrashHandler_ReadMemory(nInstructionState + 0x10, &closureObject, sizeof(closureObject)) &&
		closureObject.nType == 0x08000100 &&
		CrashHandler_ReadMemory(closureObject.nValue + 0x50, &functionProtoObject, sizeof(functionProtoObject)) &&
		functionProtoObject.nType == 0x08002000)
	{
		CrashHandler_ReadMemory(functionProtoObject.nValue + 0x48, &sourceNameObject, sizeof(sourceNameObject));
		CrashHandler_ReadMemory(functionProtoObject.nValue + 0x58, &functionNameObject, sizeof(functionNameObject));

		char szSourceName[260] = {};
		char szFunctionName[260] = {};
		const bool bHasSourceName = sourceNameObject.nType == 0x08000010 &&
			CrashHandler_ReadSQString(sourceNameObject.nValue, szSourceName, sizeof(szSourceName));
		const bool bHasFunctionName = functionNameObject.nType == 0x08000010 &&
			CrashHandler_ReadSQString(functionNameObject.nValue, szFunctionName, sizeof(szFunctionName));

		m_Buffer.AppendFormat(
			"\tscript_frame: source='%s' function='%s' closure=0x%016llX funcproto=0x%016llX\n",
			bHasSourceName ? szSourceName : "<unavailable>",
			bHasFunctionName ? szFunctionName : "<unavailable>",
			closureObject.nValue,
			functionProtoObject.nValue);
	}

	if (pContext->Rsi &&
		ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(pContext->Rsi + 0x58),
			&nStackBase, sizeof(nStackBase), &nBytesRead) &&
		nBytesRead == sizeof(nStackBase))
	{
		FormatMemoryBlock("sqvm_stack_base", nStackBase, 0x200);
	}

	LONG instructionFields[4] = {};
	if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(pContext->R14),
		instructionFields, sizeof(instructionFields), &nBytesRead) &&
		nBytesRead == sizeof(instructionFields))
	{
		m_Buffer.AppendFormat(
			"\tinstruction: op=%d source_index=%d destination_index=%d arg3=%d\n",
			instructionFields[0],
			instructionFields[1],
			instructionFields[2],
			instructionFields[3]);

		if (nLiteralBase)
		{
			const DWORD64 nComputedSource = static_cast<DWORD64>(
				static_cast<LONGLONG>(nLiteralBase) + static_cast<LONGLONG>(instructionFields[1]) * 0x10);
			const DWORD64 nFaultSourceBase = static_cast<DWORD64>(
				static_cast<LONGLONG>(pContext->Rdx) - static_cast<LONGLONG>(instructionFields[1]) * 0x10);
			m_Buffer.AppendFormat(
				"\tcomputed_source: 0x%016llX register_rdx=0x%016llX match=%s fault_source_base=0x%016llX observed_literal_base=0x%016llX\n",
				nComputedSource,
				pContext->Rdx,
				nComputedSource == pContext->Rdx ? "true" : "false",
				nFaultSourceBase,
				nLiteralBase);

			SQObjectSnapshot literalObject = {};
			if (CrashHandler_ReadMemory(nComputedSource, &literalObject, sizeof(literalObject)))
			{
				m_Buffer.AppendFormat(
					"\tliteral_object: index=%d type=0x%08llX value=0x%016llX",
					instructionFields[1],
					literalObject.nType,
					literalObject.nValue);

				char szLiteral[260] = {};
				if (literalObject.nType == 0x08000010 &&
					CrashHandler_ReadSQString(literalObject.nValue, szLiteral, sizeof(szLiteral)))
				{
					m_Buffer.AppendFormat(" string='%s'", szLiteral);
				}

				m_Buffer.Append("\n");
			}
		}

		if (nStackBase)
		{
			const DWORD64 nComputedDestination = static_cast<DWORD64>(
				static_cast<LONGLONG>(nStackBase) + static_cast<LONGLONG>(instructionFields[2]) * 0x10);
			m_Buffer.AppendFormat(
				"\tcomputed_destination: 0x%016llX register_rcx=0x%016llX match=%s\n",
				nComputedDestination,
				pContext->Rcx,
				nComputedDestination == pContext->Rcx ? "true" : "false");
		}
	}

	m_Buffer.Append("}\n");
}

//-----------------------------------------------------------------------------
// Purpose: formats all loaded modules (verbose)
//-----------------------------------------------------------------------------
void CCrashHandler::FormatModules()
{
	m_Buffer.Append("modules:\n{\n");

	const HANDLE hProcess = GetCurrentProcess();

	DWORD cbNeeded;
	const BOOL result = K32EnumProcessModulesEx(hProcess, m_ppModuleHandles, sizeof(m_ppModuleHandles), &cbNeeded, LIST_MODULES_ALL);

	if (result && cbNeeded <= sizeof(m_ppModuleHandles) && cbNeeded / sizeof(HMODULE))
	{
		CHAR szModuleName[MAX_FILEPATH];
		LPSTR pszModuleName;
		MODULEINFO modInfo;

		for (DWORD i = 0, j = cbNeeded / sizeof(HMODULE); j; i++, j--)
		{
			const DWORD m = GetModuleFileNameA(m_ppModuleHandles[i], szModuleName, sizeof(szModuleName));

			if (m == 0 || (m - 1) > (sizeof(szModuleName) - 2)) // Too small for buffer.
			{
				snprintf(szModuleName, sizeof(szModuleName), "module@%p", m_ppModuleHandles[i]);
				pszModuleName = szModuleName;
			}
			else
			{
				pszModuleName = const_cast<LPSTR>(CrashHandler_BaseFileName(szModuleName));
			}

			K32GetModuleInformation(hProcess, m_ppModuleHandles[i], &modInfo, sizeof(modInfo));

			m_Buffer.AppendFormat("\t%-15s: [%p, %p]\n", 
				pszModuleName, modInfo.lpBaseOfDll, (reinterpret_cast<uintptr_t>(modInfo.lpBaseOfDll) + modInfo.SizeOfImage));
		}
	}

	m_Buffer.Append("}\n");
}

//-----------------------------------------------------------------------------
// Purpose: formats the system information
//-----------------------------------------------------------------------------
void CCrashHandler::FormatSystemInfo()
{
	m_Buffer.Append("system:\n{\n");

	const CPUInformation& pi = GetCPUInformation();

	m_Buffer.AppendFormat("\tcpu_model = \"%s\"\n", pi.m_szProcessorBrand);
	m_Buffer.AppendFormat("\tcpu_speed = %010lld // clock cycles\n", pi.m_Speed);

	DISPLAY_DEVICE& dd = m_HardWareInfo.displayDevice;

	for (DWORD i = 0; ; i++)
	{
		const BOOL f = EnumDisplayDevices(NULL, i, &dd, EDD_GET_DEVICE_INTERFACE_NAME);

		if (!f)
		{
			break;
		}

		if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) // The primary device is the only relevant device.
		{
			m_Buffer.AppendFormat("\tgpu_model = \"%s\"\n", dd.DeviceString);
			m_Buffer.AppendFormat("\tgpu_flags = 0x%08X // primary device\n", dd.StateFlags);

			break;
		}
	}

	MEMORYSTATUSEX& statex = m_HardWareInfo.memoryStatus;

	if (GlobalMemoryStatusEx(&statex))
	{
		m_Buffer.AppendFormat("\tram_total = [%.2lf, %.2lf] // physical/virtual (MiB)\n", (f64)(statex.ullTotalPhys / (1024.0 * 1024.0)), (f64)(statex.ullTotalVirtual / (1024.0 * 1024.0)));
		m_Buffer.AppendFormat("\tram_avail = [%.2lf, %.2lf] // physical/virtual (MiB)\n", (f64)(statex.ullAvailPhys / (1024.0 * 1024.0)), (f64)(statex.ullAvailVirtual / (1024.0 * 1024.0)));
	}

	DWORD sectorsPerCluster, bytesPerSector, freeClusters, totalClusters;

	if (GetDiskFreeSpaceA(NULL, &sectorsPerCluster, &bytesPerSector, &freeClusters, &totalClusters))
	{
		m_HardWareInfo.totalDiskSpace = (u64)totalClusters * sectorsPerCluster * bytesPerSector;
		m_HardWareInfo.availDiskSpace = (u64)freeClusters * sectorsPerCluster * bytesPerSector;

		m_Buffer.AppendFormat("\tdsk_total = %.2lf // (MiB)\n", (f64)(m_HardWareInfo.totalDiskSpace / (1024.0 * 1024.0)));
		m_Buffer.AppendFormat("\tdsk_avail = %.2lf // (MiB)\n", (f64)(m_HardWareInfo.availDiskSpace / (1024.0 * 1024.0)));
	}

	m_Buffer.Append("}\n");
}

//-----------------------------------------------------------------------------
// Purpose: formats the build information
//-----------------------------------------------------------------------------
void CCrashHandler::FormatBuildInfo()
{
	m_Buffer.AppendFormat("build_id: %u\n", g_SDKDll.GetNTHeaders()->FileHeader.TimeDateStamp);
	m_Buffer.AppendFormat("session_id: %s\n", g_LogSessionUUID.c_str());
	m_Buffer.AppendFormat("log_dir: %s\n", g_LogSessionDirectory.c_str());
	m_Buffer.AppendFormat("process_id: %lu\n", GetCurrentProcessId());
	m_Buffer.AppendFormat("thread_id: %lu\n", GetCurrentThreadId());
}

//-----------------------------------------------------------------------------
// Purpose: formats the module, address and exception
//-----------------------------------------------------------------------------
void CCrashHandler::FormatExceptionAddress()
{
	FormatExceptionAddress(static_cast<LPCSTR>(m_pExceptionPointers->ExceptionRecord->ExceptionAddress));
}

//-----------------------------------------------------------------------------
// Purpose: formats the module, address and exception
// Input  : pExceptionAddress - 
//-----------------------------------------------------------------------------
void CCrashHandler::FormatExceptionAddress(const LPCSTR pExceptionAddress, const bool bSetCrashModule)
{
	HMODULE hCrashedModule;
	if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, pExceptionAddress, &hCrashedModule))
	{
		m_Buffer.AppendFormat("\t!!!unknown-module!!!: %p", pExceptionAddress);
		FormatSymbolInfo(reinterpret_cast<DWORD64>(pExceptionAddress));
		m_Buffer.Append("\n");
		if (bSetCrashModule)
			m_nCrashMsgFlags = 0; // Display the "unknown DLL or EXE" message.
		return;
	}

	const LPCSTR pModuleBase = reinterpret_cast<LPCSTR>(pExceptionAddress - reinterpret_cast<LPCSTR>(hCrashedModule));

	CHAR szCrashedModuleFullName[MAX_PATH];
	if (GetModuleFileNameExA(GetCurrentProcess(), hCrashedModule, szCrashedModuleFullName, sizeof(szCrashedModuleFullName)) - 1 > 0x1FE)
	{
		m_Buffer.AppendFormat("\tmodule@%p: %p", (void*)hCrashedModule, pModuleBase);
		FormatSymbolInfo(reinterpret_cast<DWORD64>(pExceptionAddress));
		m_Buffer.Append("\n");
		if (bSetCrashModule)
			m_nCrashMsgFlags = 2; // Display the "Apex crashed" message without additional information regarding the module.
		return;
	}

	// NOTE: original implementation strips the extension as well, but we keep
	// this in as its useful for when additional modules are loaded that aren't
	// part of the OS or game
	const char* const szCrashedModuleName = CrashHandler_BaseFileName(szCrashedModuleFullName);

	m_Buffer.AppendFormat("\t%-15s: %p", szCrashedModuleName, pModuleBase);
	FormatSymbolInfo(reinterpret_cast<DWORD64>(pExceptionAddress));
	m_Buffer.Append("\n");
	if (bSetCrashModule)
		m_nCrashMsgFlags = 1; // Display the "Apex crashed in <module>" message.

	// Only set it once to the crashing module,
	// empty strings get treated as "unknown
	// DLL or EXE" in the crashmsg executable.
	if (bSetCrashModule && !m_CrashingModule.Length())
	{
		m_CrashingModule.Append(szCrashedModuleName);
	}
}

//-----------------------------------------------------------------------------
// Purpose: appends best-effort symbol and source information for an address
//-----------------------------------------------------------------------------
void CCrashHandler::FormatSymbolInfo(const DWORD64 nAddress)
{
	if (!nAddress || !CrashHandler_EnsureSymbolsInitialized())
		return;

	struct SymbolInfoBuffer_t
	{
		SYMBOL_INFO symbol;
		char name[MAX_SYM_NAME];
	};

	SymbolInfoBuffer_t symbolInfo = {};
	PSYMBOL_INFO pSymbol = &symbolInfo.symbol;
	pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
	pSymbol->MaxNameLen = MAX_SYM_NAME;

	DWORD64 nDisplacement = 0;
	if (SymFromAddr(GetCurrentProcess(), nAddress, &nDisplacement, pSymbol))
	{
		m_Buffer.AppendFormat(" // %s", pSymbol->Name);
		if (nDisplacement)
			m_Buffer.AppendFormat("+0x%llX", nDisplacement);
	}

	IMAGEHLP_LINE64 lineInfo = {};
	lineInfo.SizeOfStruct = sizeof(lineInfo);

	DWORD nLineDisplacement = 0;
	if (SymGetLineFromAddr64(GetCurrentProcess(), nAddress, &nLineDisplacement, &lineInfo) && lineInfo.FileName)
	{
		m_Buffer.AppendFormat(" [%s:%lu", lineInfo.FileName, lineInfo.LineNumber);
		if (nLineDisplacement)
			m_Buffer.AppendFormat("+0x%X", nLineDisplacement);
		m_Buffer.Append("]");
	}
}

//-----------------------------------------------------------------------------
// Purpose: formats the exception code
//-----------------------------------------------------------------------------
void CCrashHandler::FormatExceptionCode()
{
	const DWORD nExceptionCode = m_pExceptionPointers->ExceptionRecord->ExceptionCode;

	if (nExceptionCode > EXCEPTION_IN_PAGE_ERROR)
	{
		m_Buffer.AppendFormat(ExceptionToString(), nExceptionCode);
	}
	else if (nExceptionCode >= EXCEPTION_ACCESS_VIOLATION)
	{
		const CHAR* pszException = "EXCEPTION_IN_PAGE_ERROR";

		if (nExceptionCode == EXCEPTION_ACCESS_VIOLATION)
		{
			pszException = "EXCEPTION_ACCESS_VIOLATION";
		}

		const ULONG_PTR uExceptionInfo0 = m_pExceptionPointers->ExceptionRecord->ExceptionInformation[0];
		const ULONG_PTR uExceptionInfo1 = m_pExceptionPointers->ExceptionRecord->ExceptionInformation[1];

		if (uExceptionInfo0)
		{
			if (uExceptionInfo0 == 1)
			{
				m_Buffer.AppendFormat("\t%s(write): %p\n", pszException, uExceptionInfo1);
			}
			else if (uExceptionInfo0 == 8)
			{
				m_Buffer.AppendFormat("\t%s(execute): %p\n", pszException, uExceptionInfo1);
			}
			else
			{
				m_Buffer.AppendFormat("\t%s(unknown): %p\n", pszException, uExceptionInfo1);
			}
		}
		else
		{
			m_Buffer.AppendFormat("\t%s(read): %p\n", pszException, uExceptionInfo1);
		}

		if (uExceptionInfo0 != 8)
		{
			if (IsPageAccessible())
			{
				FormatExceptionAddress();
			}
		}
	}
	else
	{
		m_Buffer.AppendFormat(ExceptionToString(), nExceptionCode);
	}
}

//-----------------------------------------------------------------------------
// Purpose: formats the arithmetic logic register and its content
// Input  : *pszRegister - 
//			nContent - 
//-----------------------------------------------------------------------------
void CCrashHandler::FormatALU(const char* const pszRegister, const DWORD64 nContent)
{
	if (nContent >= 1000000)
	{
		if (nContent > UINT_MAX)
		{
			// Print the full 64bits of the register.
			m_Buffer.AppendFormat("\t%s = 0x%016llX\n", pszRegister, nContent);
		}
		else
		{
			m_Buffer.AppendFormat("\t%s = 0x%08X\n", pszRegister, nContent);
		}
	}
	else if (nContent >= 10)
	{
		// Print as decimal with a hexadecimal comment.
		m_Buffer.AppendFormat("\t%s = %-6i // 0x%08X\n", pszRegister, nContent, nContent);
	}
	else
	{
		// Print as decimal only.
		m_Buffer.AppendFormat("\t%s = %-10i\n", pszRegister, nContent);
	}
}

//-----------------------------------------------------------------------------
// Purpose: formats the floating point register and its content
// Input  : *pszRegister - 
//			*pxContent - 
//-----------------------------------------------------------------------------
void CCrashHandler::FormatFPU(const char* const pszRegister, const M128A* const pxContent)
{
	const DWORD nVec[4] =
	{
		static_cast<DWORD>(pxContent->Low & UINT_MAX),
		static_cast<DWORD>(pxContent->Low >> 32),
		static_cast<DWORD>(pxContent->High & UINT_MAX),
		static_cast<DWORD>(pxContent->High >> 32),
	};

	m_Buffer.AppendFormat("\t%s = [ [%.8g, %.8g, %.8g, %.8g]", pszRegister,
		*reinterpret_cast<const FLOAT*>(&nVec[0]),
		*reinterpret_cast<const FLOAT*>(&nVec[1]),
		*reinterpret_cast<const FLOAT*>(&nVec[2]),
		*reinterpret_cast<const FLOAT*>(&nVec[3]));

	const DWORD nHighest = *MaxElement(std::begin(nVec), std::end(nVec));

	if (nHighest >= 1000000)
	{
		m_Buffer.AppendFormat(", [0x%08X, 0x%08X, 0x%08X, 0x%08X] ]\n",
			nVec[0], nVec[1], nVec[2], nVec[3]);
	}
	else
	{
		m_Buffer.AppendFormat(", [%i, %i, %i, %i] ]\n",
			static_cast<LONG>(nVec[0]), static_cast<LONG>(nVec[1]),
			static_cast<LONG>(nVec[2]), static_cast<LONG>(nVec[3]));
	}
}

//-----------------------------------------------------------------------------
// Purpose: safely formats a memory region without dereferencing crash pointers
//-----------------------------------------------------------------------------
void CCrashHandler::FormatMemoryBlock(const char* const pszName, const DWORD64 nAddress, const SIZE_T nSize)
{
	m_Buffer.AppendFormat("\t%s:\n\t{\n\t\taddress: 0x%016llX\n", pszName, nAddress);

	if (!nAddress || !nSize)
	{
		m_Buffer.Append("\t\tstatus: unavailable\n\t}\n");
		return;
	}

	MEMORY_BASIC_INFORMATION memoryInfo = {};
	const SIZE_T nQuerySize = VirtualQuery(reinterpret_cast<LPCVOID>(nAddress), &memoryInfo, sizeof(memoryInfo));
	if (nQuerySize == sizeof(memoryInfo))
	{
		m_Buffer.AppendFormat(
			"\t\tregion: base=0x%016llX size=0x%llX state=0x%X protect=0x%X type=0x%X\n",
			reinterpret_cast<DWORD64>(memoryInfo.BaseAddress),
			static_cast<unsigned long long>(memoryInfo.RegionSize),
			memoryInfo.State,
			memoryInfo.Protect,
			memoryInfo.Type);
	}

	BYTE memory[0x200] = {};
	const SIZE_T nRequestedSize = nSize < sizeof(memory) ? nSize : sizeof(memory);
	SIZE_T nBytesRead = 0;
	const BOOL bRead = ReadProcessMemory(
		GetCurrentProcess(),
		reinterpret_cast<LPCVOID>(nAddress),
		memory,
		nRequestedSize,
		&nBytesRead);

	m_Buffer.AppendFormat("\t\tread: %s bytes=0x%llX error=%lu\n",
		bRead ? "ok" : "failed",
		static_cast<unsigned long long>(nBytesRead),
		bRead ? ERROR_SUCCESS : GetLastError());

	for (SIZE_T nOffset = 0; nOffset < nBytesRead; nOffset += 0x20)
	{
		m_Buffer.AppendFormat("\t\t0x%016llX:", nAddress + nOffset);

		for (SIZE_T nColumn = 0; nColumn < 0x20 && nOffset + nColumn < nBytesRead; nColumn += sizeof(DWORD64))
		{
			DWORD64 nValue = 0;
			const SIZE_T nRemaining = nBytesRead - (nOffset + nColumn);
			const SIZE_T nCopySize = nRemaining < sizeof(nValue) ? nRemaining : sizeof(nValue);
			memcpy(&nValue, &memory[nOffset + nColumn], nCopySize);
			m_Buffer.AppendFormat(" %016llX", nValue);
		}

		m_Buffer.Append("\n");
	}

	m_Buffer.Append("\t}\n");
}

//-----------------------------------------------------------------------------
// Purpose: returns the current exception code as string
// Output : exception code, "UNKNOWN_EXCEPTION" if exception code doesn't exist in this context
//-----------------------------------------------------------------------------
const char* CCrashHandler::ExceptionToString(const DWORD nExceptionCode) const
{
	switch (nExceptionCode)
	{
	case EXCEPTION_BREAKPOINT:               { return "\tEXCEPTION_BREAKPOINT"               ": %08X\n"; };
	case EXCEPTION_SINGLE_STEP:              { return "\tEXCEPTION_SINGLE_STEP"              ": %08X\n"; };
	case EXCEPTION_ACCESS_VIOLATION:         { return "\tEXCEPTION_ACCESS_VIOLATION"         ": %08X\n"; };
	case EXCEPTION_IN_PAGE_ERROR:            { return "\tEXCEPTION_IN_PAGE_ERROR"            ": %08X\n"; };
	case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    { return "\tEXCEPTION_ARRAY_BOUNDS_EXCEEDED"    ": %08X\n"; };
	case EXCEPTION_ILLEGAL_INSTRUCTION:      { return "\tEXCEPTION_ILLEGAL_INSTRUCTION"      ": %08X\n"; };
	case EXCEPTION_INVALID_DISPOSITION:      { return "\tEXCEPTION_INVALID_DISPOSITION"      ": %08X\n"; };
	case EXCEPTION_NONCONTINUABLE_EXCEPTION: { return "\tEXCEPTION_NONCONTINUABLE_EXCEPTION" ": %08X\n"; };
	case EXCEPTION_PRIV_INSTRUCTION:         { return "\tEXCEPTION_PRIV_INSTRUCTION"         ": %08X\n"; };
	case EXCEPTION_STACK_OVERFLOW:           { return "\tEXCEPTION_STACK_OVERFLOW"           ": %08X\n"; };
	case EXCEPTION_DATATYPE_MISALIGNMENT:    { return "\tEXCEPTION_DATATYPE_MISALIGNMENT"    ": %08X\n"; };
	case EXCEPTION_FLT_DENORMAL_OPERAND:     { return "\tEXCEPTION_FLT_DENORMAL_OPERAND"     ": %08X\n"; };
	case EXCEPTION_FLT_DIVIDE_BY_ZERO:       { return "\tEXCEPTION_FLT_DIVIDE_BY_ZERO"       ": %08X\n"; };
	case EXCEPTION_FLT_INEXACT_RESULT:       { return "\tEXCEPTION_FLT_INEXACT_RESULT"       ": %08X\n"; };
	case EXCEPTION_FLT_INVALID_OPERATION:    { return "\tEXCEPTION_FLT_INVALID_OPERATION"    ": %08X\n"; };
	case EXCEPTION_FLT_OVERFLOW:             { return "\tEXCEPTION_FLT_OVERFLOW"             ": %08X\n"; };
	case EXCEPTION_FLT_STACK_CHECK:          { return "\tEXCEPTION_FLT_STACK_CHECK"          ": %08X\n"; };
	case EXCEPTION_FLT_UNDERFLOW:            { return "\tEXCEPTION_FLT_UNDERFLOW"            ": %08X\n"; };
	case EXCEPTION_INT_DIVIDE_BY_ZERO:       { return "\tEXCEPTION_INT_DIVIDE_BY_ZERO"       ": %08X\n"; };
	case EXCEPTION_INT_OVERFLOW:             { return "\tEXCEPTION_INT_OVERFLOW"             ": %08X\n"; };
	default:                                 { return "\tUNKNOWN_EXCEPTION"                  ": %08X\n"; };
	}
}

//-----------------------------------------------------------------------------
// Purpose: returns the current exception code as string
//-----------------------------------------------------------------------------
const char* CCrashHandler::ExceptionToString() const
{
	return ExceptionToString(m_pExceptionPointers->ExceptionRecord->ExceptionCode);
}

//-----------------------------------------------------------------------------
// Purpose: tests if memory page is accessible
// Output : true if accessible, false otherwise
//-----------------------------------------------------------------------------
bool CCrashHandler::IsPageAccessible() const
{
	const PCONTEXT pContextRecord = m_pExceptionPointers->ContextRecord;
	MEMORY_BASIC_INFORMATION mbi = { 0 };

	const SIZE_T t = VirtualQuery((LPCVOID)pContextRecord->Rsp, &mbi, sizeof(LPCVOID));
	if (t < sizeof(mbi))
	{
		return false;
	}

	if (!(mbi.State & MEM_COMMIT) || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
		return false;

	return (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
		PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

//-----------------------------------------------------------------------------
// Purpose: captures the callstack
//-----------------------------------------------------------------------------
void CCrashHandler::CaptureCallStack()
{
	m_nCapturedFrames = 0;

	if (!m_pExceptionPointers || !m_pExceptionPointers->ContextRecord)
		return;

	CrashHandler_EnsureSymbolsInitialized();

	CONTEXT context = *m_pExceptionPointers->ContextRecord;
	STACKFRAME64 stackFrame = {};

	stackFrame.AddrPC.Mode = AddrModeFlat;
	stackFrame.AddrPC.Offset = context.Rip;
	stackFrame.AddrFrame.Mode = AddrModeFlat;
	stackFrame.AddrFrame.Offset = context.Rbp;
	stackFrame.AddrStack.Mode = AddrModeFlat;
	stackFrame.AddrStack.Offset = context.Rsp;

	const HANDLE hProcess = GetCurrentProcess();
	const HANDLE hThread = GetCurrentThread();

	DWORD64 nLastAddress = 0;

	while (m_nCapturedFrames < NUM_FRAMES_TO_CAPTURE)
	{
		const DWORD64 nAddress = stackFrame.AddrPC.Offset;
		if (!nAddress || nAddress == nLastAddress)
			break;

		m_ppStackTrace[m_nCapturedFrames++] = reinterpret_cast<PVOID>(nAddress);
		nLastAddress = nAddress;

		if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, hProcess, hThread,
			&stackFrame, &context, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
		{
			break;
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: writes the stack trace and minidump to the disk
//-----------------------------------------------------------------------------
void CCrashHandler::WriteFile()
{
	const char* const pszLogDirectory = g_LogSessionDirectory.empty()
		? "platform/logs"
		: g_LogSessionDirectory.c_str();

	CrashHandler_EnsureDirectoryExists(pszLogDirectory);

	CFmtStrQuietTruncationN<MAX_PATH> latestCrashText;
	CFmtStrQuietTruncationN<MAX_PATH> latestMiniDump;

	latestCrashText.Format("%s/%s.txt", pszLogDirectory, "apex_crash");
	latestMiniDump.Format("%s/%s.dmp", pszLogDirectory, "minidump");

	CrashHandler_WriteTextFile(latestCrashText.String(), m_Buffer.String(), (DWORD)m_Buffer.Length());
	CrashHandler_WriteMiniDumpFile(latestMiniDump.String(), m_pExceptionPointers);
}

//-----------------------------------------------------------------------------
// Purpose: creates the crashmsg process displaying the error to the user
// the process has to be separate as the current process is getting killed
//-----------------------------------------------------------------------------
void CCrashHandler::CreateMessageProcess() const
{
	CFmtStrQuietTruncationN<256> messageCmdLine;

	const PEXCEPTION_RECORD pExceptionRecord = m_pExceptionPointers->ExceptionRecord;
	const PCONTEXT pContextRecord = m_pExceptionPointers->ContextRecord;

	if (pExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
		pExceptionRecord->ExceptionInformation[0] == 8 &&
		pExceptionRecord->ExceptionInformation[1] != pContextRecord->Rip)
	{
		messageCmdLine.Append(CRASHMESSAGE_MSG_EXECUTABLE" overclock");
	}
	else
	{
		messageCmdLine.Format(CRASHMESSAGE_MSG_EXECUTABLE" crash %hhu \"%s\"",
			m_nCrashMsgFlags, m_CrashingModule.String());
	}

	STARTUPINFOA startupInfo = { 0 };
	PROCESS_INFORMATION processInfo;

	startupInfo.cb = sizeof(STARTUPINFOA);

	if (CreateProcessA(NULL, (LPSTR)messageCmdLine.String(),
		NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &startupInfo, &processInfo))
	{
		CloseHandle(processInfo.hProcess);
		CloseHandle(processInfo.hThread);
	}
}

//-----------------------------------------------------------------------------
// Purpose: calls the crash callback
//-----------------------------------------------------------------------------
void CCrashHandler::CrashCallback() const
{
	if (m_pCrashCallback)
	{
		m_pCrashCallback(this);
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : 
// Output : 
//-----------------------------------------------------------------------------
long __stdcall BottomLevelExceptionFilter(EXCEPTION_POINTERS* const pExceptionInfo)
{
	g_CrashHandler.Start();

	// If the exception couldn't be handled, terminate the process
	if (g_CrashHandler.GetExit())
	{
		ExitProcess(EXIT_FAILURE);
	}

	g_CrashHandler.Reset();
	g_CrashHandler.SetExceptionPointers(pExceptionInfo);

	// Let the higher level exception handlers deal with this particular
	// exception.
	if (g_CrashHandler.ExceptionToString() == g_CrashHandler.ExceptionToString(0xFFFFFFFF))
	{
		g_CrashHandler.End();
		return EXCEPTION_CONTINUE_SEARCH;
	}

	// Don't run when a debugger is present.
	if (IsDebuggerPresent())
	{
		g_CrashHandler.End();
		return EXCEPTION_CONTINUE_SEARCH;
	}

	g_CrashHandler.SetExit(true);

	g_CrashHandler.CaptureCallStack();

	g_CrashHandler.FormatCrash();
	g_CrashHandler.FormatCallstack();
	g_CrashHandler.FormatRegisters();
	g_CrashHandler.FormatExceptionMemory();
	g_CrashHandler.FormatModules();
	g_CrashHandler.FormatSystemInfo();
	g_CrashHandler.FormatBuildInfo();

	g_CrashHandler.WriteFile();

	// Run the crash callback
	g_CrashHandler.CrashCallback();

	// End it here, the next recursive call terminates the process.
	g_CrashHandler.End();

	return EXCEPTION_EXECUTE_HANDLER;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CCrashHandler::Init()
{
	InitializeSRWLock(&m_Lock);

	// Initialize the DbgHelp symbol handler now, on the main thread at startup,
	// rather than lazily from inside the exception filter. DbgHelp is not
	// thread-safe; doing this here guarantees symbol initialization never runs
	// (or races) while a crash is being formatted. The in-crash calls to
	// CrashHandler_EnsureSymbolsInitialized() then short-circuit, and all crash
	// formatting remains serialized by the exclusive SRW lock above.
	CrashHandler_EnsureSymbolsInitialized();

	m_hExceptionHandler = AddVectoredExceptionHandler(TRUE, BottomLevelExceptionFilter);
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CCrashHandler::Shutdown()
{
	if (m_hExceptionHandler)
	{
		RemoveVectoredExceptionHandler(m_hExceptionHandler);
		m_hExceptionHandler = nullptr;
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CCrashHandler::Reset()
{
	m_Buffer.Clear();
	m_CrashingModule.Clear();
	m_nCrashMsgFlags = 0;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
CCrashHandler::CCrashHandler()
	: m_ppStackTrace()
	, m_nCapturedFrames(0)
	, m_ppModuleHandles()
	, m_pCrashCallback(nullptr)
	, m_hExceptionHandler(nullptr)
	, m_pExceptionPointers(nullptr)
	, m_nCrashMsgFlags(0)
	, m_bExit(false)
	, m_bMessageCreated(false)
{
	Init();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
CCrashHandler::~CCrashHandler()
{
	Shutdown();
}

CCrashHandler g_CrashHandler;
