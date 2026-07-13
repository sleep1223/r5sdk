#ifndef LOGGER_H
#define LOGGER_H

void EngineLoggerSink(LogType_t logType, LogLevel_t logLevel, eDLL_T context,
	const char* pszLogger, const char* pszFormat, va_list args,
	const UINT exitCode /*= NO_ERROR*/, const char* pszUptimeOverride /*= nullptr*/);

void SDK_WriteProcessExitDiagnostic(const char* pszSource, UINT exitCode, const char* pszDetail = nullptr);
void SDK_WriteRuntimeBreadcrumb(const char* pszSource, const char* pszDetail = nullptr);
void SDK_AppendDiagnosticLogFile(const char* pszFileName, const char* pszText);

#endif // LOGGER_H
