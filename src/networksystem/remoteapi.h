#pragma once

#include <functional>

class ConVar;

extern ConVar sv_r5_api_base_url;
extern ConVar sv_r5_api_token;
extern ConVar sv_r5_api_timeout;
extern ConVar sv_r5_api_debug;

//-----------------------------------------------------------------------------
// Snapshot of the per-request HTTP settings, captured on the main thread.
//
// Worker threads must NEVER read ConVar string storage directly:
// ConVar::ChangeStringValue() frees and reallocates the internal buffer, so a
// 'const char*' obtained from ConVar::GetString() on a worker thread can dangle
// the instant another thread (e.g. an RCON command) changes the convar, causing
// a use-after-free crash. Capture everything needed into this struct on the main
// thread before spawning the worker.
//-----------------------------------------------------------------------------
struct RemoteApiRequest_t
{
	string m_svUrl;        // fully resolved endpoint URL
	string m_svToken;      // bearer token (empty = none)
	int    m_nTimeout = 3;
	bool   m_bVerifyPeer = true;
	bool   m_bVerbose = false;
};

struct RemoteApiResponse_t
{
	int m_nHttpStatus = 0;
	string m_svBody;
	string m_svDebugMessage;
	rapidjson::Document m_Json;
};

// Captures the shared auth token and timeout. Must be called on the main thread.
void SV_CaptureRemoteApiAuth(RemoteApiRequest_t& request);

void SV_LogRemoteApiConfigIfEnabled();
bool SV_IsRemoteApiDebugEnabled();
bool SV_IsRemoteApiShutdownRequested();
bool SV_StartRemoteApiWorker(const char* const pszName, std::function<void()> worker);
bool SV_SendRemoteApiJsonRequest(const RemoteApiRequest_t& request,
	const rapidjson::Document& requestJson, RemoteApiResponse_t& outResponse,
	string& outMessage, const char* const pszContext,
	const bool bRequireJsonObject);
void SV_BeginRemoteApiShutdown();
void SV_WaitForRemoteApiWorkers();
void SV_GetRemoteServerAddress(string& outServerIp, int& outServerPort);
void SV_GetRemoteServerName(string& outServerName);
int SV_GetRemoteServerMaxPlayers();
void SV_AddRemoteServerAddress(rapidjson::Value& object,
	rapidjson::Document::AllocatorType& allocator,
	const string& serverIp, const int serverPort);
void SV_AddRemoteServerName(rapidjson::Value& object,
	rapidjson::Document::AllocatorType& allocator,
	const string& serverName);
bool SV_ResolveRemoteApiUrl(const char* const pszConfiguredUrl,
	const char* const pszDefaultPath, string& outUrl, string& outMessage,
	const char* const pszConfiguredName);
