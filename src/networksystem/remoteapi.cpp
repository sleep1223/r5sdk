//=============================================================================//
//
// Purpose: Shared helpers for remote HTTP API integrations.
//
//=============================================================================//

#include "core/stdafx.h"
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include "tier1/cvar.h"
#include "thirdparty/curl/include/curl/curl.h"
#include "tier2/curlutils.h"
#include "tier2/jsonutils.h"
#include "game/server/gameinterface.h"
#include "networksystem/hostmanager.h"
#include "networksystem/remoteapi.h"
#include "networksystem/matchreport.h"
#include "networksystem/playeraccess.h"

ConVar sv_r5_api_base_url("sv_r5_api_base_url", "", FCVAR_RELEASE | FCVAR_ACCESSIBLE_FROM_THREADS,
	"Base HTTP(S) URL for remote R5 API endpoints. Full endpoint URL convars take precedence.");
ConVar sv_r5_api_token("sv_r5_api_token", "", FCVAR_RELEASE | FCVAR_ACCESSIBLE_FROM_THREADS | FCVAR_PROTECTED | FCVAR_DONTRECORD,
	"Optional bearer token for remote R5 API endpoints.");
ConVar sv_r5_api_timeout("sv_r5_api_timeout", "3", FCVAR_RELEASE | FCVAR_ACCESSIBLE_FROM_THREADS,
	"Remote R5 API request timeout in seconds.", true, 1.f, false, 0.f, "seconds");
ConVar sv_r5_api_debug("sv_r5_api_debug", "0", FCVAR_RELEASE | FCVAR_ACCESSIBLE_FROM_THREADS,
	"Enables verbose shared diagnostics for remote R5 API integrations.", false, 0.f, true, 1.f, "0 = Disable, 1 = Enable.");

static string s_svLastLoggedRemoteApiConfig;
static std::atomic_bool s_bRemoteApiShutdownRequested(false);
static std::mutex s_RemoteApiWorkerMutex;
static std::condition_variable s_RemoteApiWorkerCondition;
static unsigned int s_nRemoteApiWorkers = 0;

bool SV_IsRemoteApiShutdownRequested()
{
	return s_bRemoteApiShutdownRequested.load();
}

bool SV_IsRemoteApiDebugEnabled()
{
	return sv_r5_api_debug.GetBool();
}

void SV_CaptureRemoteApiAuth(RemoteApiRequest_t& request)
{
	const char* const pszToken = sv_r5_api_token.GetString();
	request.m_svToken = VALID_CHARSTAR(pszToken) ? pszToken : "";
	request.m_nTimeout = sv_r5_api_timeout.GetInt();
}

bool SV_SendRemoteApiJsonRequest(const RemoteApiRequest_t& request,
	const rapidjson::Document& requestJson, RemoteApiResponse_t& outResponse,
	string& outMessage, const char* const pszContext,
	const bool bRequireJsonObject)
{
	outResponse.m_nHttpStatus = 0;
	outResponse.m_svBody.clear();
	outResponse.m_svDebugMessage.clear();
	outResponse.m_Json.SetObject();

	const char* const pszSafeContext = VALID_CHARSTAR(pszContext) ? pszContext : "remote api";

	rapidjson::StringBuffer stringBuffer;
	JSON_DocumentToBufferDeserialize(requestJson, stringBuffer);

	CURLParams params;
	params.writeFunction = CURLWriteStringCallback;
	params.timeout = request.m_nTimeout;
	params.verifyPeer = request.m_bVerifyPeer;
	params.verbose = request.m_bVerbose;

	curl_slist* sList = nullptr;
	CURL* const curl = CURLInitRequest(request.m_svUrl.c_str(), stringBuffer.GetString(),
		outResponse.m_svBody, sList, params);
	if (!curl)
	{
		if (sList)
			curl_slist_free_all(sList);

		outMessage = "curl init failed";
		return false;
	}

	string svAuthHeader;
	if (!request.m_svToken.empty())
	{
		svAuthHeader = Format("Authorization: Bearer %s", request.m_svToken.c_str());
		curl_slist* const sListWithAuth = curl_slist_append(sList, svAuthHeader.c_str());
		if (!sListWithAuth)
		{
			curl_slist_free_all(sList);
			curl_easy_cleanup(curl);
			outMessage = "curl auth header append failed";
			return false;
		}

		sList = sListWithAuth;
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, sList);
	}

	const CURLcode result = CURLSubmitRequest(curl, sList);
	if (!CURLHandleError(curl, result, outMessage, false))
		return false;

	const CURLINFO status = CURLRetrieveInfo(curl);
	outResponse.m_nHttpStatus = int(status);

	if (request.m_bVerbose)
	{
		outResponse.m_svDebugMessage = Format("%s response HTTP %d: %s",
			pszSafeContext, outResponse.m_nHttpStatus,
			outResponse.m_svBody.empty() ? "<empty>" : outResponse.m_svBody.c_str());
	}

	if (status < 200 || status >= 300)
	{
		outMessage = Format("%s endpoint returned HTTP %d", pszSafeContext, outResponse.m_nHttpStatus);
		return false;
	}

	if (!bRequireJsonObject)
		return true;

	outResponse.m_Json.Parse(outResponse.m_svBody.c_str(), outResponse.m_svBody.length());

	if (outResponse.m_Json.HasParseError())
	{
		outMessage = Format("%s JSON parse error at position %zu: %s",
			pszSafeContext, outResponse.m_Json.GetErrorOffset(),
			rapidjson::GetParseError_En(outResponse.m_Json.GetParseError()));
		return false;
	}

	if (!outResponse.m_Json.IsObject())
	{
		outMessage = Format("%s JSON root was not an object", pszSafeContext);
		return false;
	}

	return true;
}

bool SV_StartRemoteApiWorker(const char* const pszName, std::function<void()> worker)
{
	if (!worker || SV_IsRemoteApiShutdownRequested())
		return false;

	const string svName = VALID_CHARSTAR(pszName) ? pszName : "unknown";

	{
		std::lock_guard<std::mutex> lock(s_RemoteApiWorkerMutex);
		if (s_bRemoteApiShutdownRequested.load())
			return false;

		++s_nRemoteApiWorkers;
	}

	try
	{
		std::thread([svName, worker = std::move(worker)]
			{
				try
				{
					worker();
				}
				catch (const std::exception& ex)
				{
					Warning(eDLL_T::SERVER, "Remote API worker '%s' failed: %s\n",
						svName.c_str(), ex.what());
				}
				catch (...)
				{
					Warning(eDLL_T::SERVER, "Remote API worker '%s' failed with an unknown exception\n",
						svName.c_str());
				}

				{
					std::lock_guard<std::mutex> lock(s_RemoteApiWorkerMutex);
					Assert(s_nRemoteApiWorkers > 0);
					if (s_nRemoteApiWorkers > 0)
						--s_nRemoteApiWorkers;
				}

				s_RemoteApiWorkerCondition.notify_all();
			}).detach();
	}
	catch (...)
	{
		{
			std::lock_guard<std::mutex> lock(s_RemoteApiWorkerMutex);
			Assert(s_nRemoteApiWorkers > 0);
			if (s_nRemoteApiWorkers > 0)
				--s_nRemoteApiWorkers;
		}

		s_RemoteApiWorkerCondition.notify_all();
		return false;
	}

	return true;
}

void SV_BeginRemoteApiShutdown()
{
	s_bRemoteApiShutdownRequested.store(true);
	s_RemoteApiWorkerCondition.notify_all();
}

void SV_WaitForRemoteApiWorkers()
{
	std::unique_lock<std::mutex> lock(s_RemoteApiWorkerMutex);
	if (s_nRemoteApiWorkers == 0)
		return;

	Msg(eDLL_T::SERVER, "Waiting for %u remote API worker(s) to finish\n", s_nRemoteApiWorkers);

	s_RemoteApiWorkerCondition.wait(lock, []
		{
			return s_nRemoteApiWorkers == 0;
		});
}

void SV_LogRemoteApiConfigIfEnabled()
{
	if (SV_IsRemoteApiShutdownRequested())
		return;

	string svSummary;
	SV_AppendPlayerAccessConfigSummary(svSummary);
	SV_AppendMatchReportConfigSummary(svSummary);

	if (svSummary.empty())
	{
		s_svLastLoggedRemoteApiConfig.clear();
		return;
	}

	const char* const pszBaseUrl = sv_r5_api_base_url.GetString();
	const char* const pszToken = sv_r5_api_token.GetString();
	const string svConfig = Format("base_url=%s auth=%d timeout=%ds debug=%d; %s",
		VALID_CHARSTAR(pszBaseUrl) ? "set" : "missing",
		VALID_CHARSTAR(pszToken) ? 1 : 0,
		sv_r5_api_timeout.GetInt(),
		sv_r5_api_debug.GetBool() ? 1 : 0,
		svSummary.c_str());

	if (svConfig == s_svLastLoggedRemoteApiConfig)
		return;

	s_svLastLoggedRemoteApiConfig = svConfig;
	Msg(eDLL_T::SERVER, "Remote API enabled: %s\n", svConfig.c_str());
}

static bool SV_IsIPv4MappedAddress(const unsigned char* const pBytes)
{
	for (int i = 0; i < 10; ++i)
	{
		if (pBytes[i] != 0)
			return false;
	}

	return pBytes[10] == 0xff && pBytes[11] == 0xff;
}

static bool SV_IsAllZeroAddress(const unsigned char* const pBytes, const int nStart, const int nEnd)
{
	for (int i = nStart; i < nEnd; ++i)
	{
		if (pBytes[i] != 0)
			return false;
	}

	return true;
}

static bool SV_IsUsableRemoteServerAddress(const CNetAdr& address)
{
	if (address.GetType() != netadrtype_t::NA_IP)
		return false;

	const unsigned char* const pBytes = reinterpret_cast<const unsigned char*>(address.GetIP());
	if (SV_IsAllZeroAddress(pBytes, 0, 16))
		return false;

	if (SV_IsIPv4MappedAddress(pBytes))
	{
		if (SV_IsAllZeroAddress(pBytes, 12, 16))
			return false;

		return !(pBytes[12] == 169 && pBytes[13] == 254);
	}

	return !(pBytes[0] == 0xfe && (pBytes[1] & 0xc0) == 0x80);
}

static bool SV_IsUsableRemoteServerAddressString(const char* const pszAddress)
{
	if (!VALID_CHARSTAR(pszAddress))
		return false;

	CNetAdr address;
	if (!address.SetFromString(pszAddress))
		return false;

	return SV_IsUsableRemoteServerAddress(address);
}

void SV_GetRemoteServerAddress(string& outServerIp, int& outServerPort)
{
	outServerIp.clear();
	outServerPort = 0;

	const CNetAdr& hostAdr = g_ServerHostManager.GetHostIP();
	if (hostAdr.GetType() == netadrtype_t::NA_IP)
	{
		outServerPort = int(ntohs(hostAdr.GetPort()));

		char addressBuffer[128];
		hostAdr.ToString(addressBuffer, sizeof(addressBuffer), true);

		if (SV_IsUsableRemoteServerAddress(hostAdr) && VALID_CHARSTAR(addressBuffer))
			outServerIp = addressBuffer;
	}

	if (outServerIp.empty())
	{
		const char* const pszHostIp = hostip ? hostip->GetString() : "";
		outServerIp = SV_IsUsableRemoteServerAddressString(pszHostIp) ? pszHostIp : "";
	}

	if (outServerPort <= 0)
		outServerPort = hostport ? hostport->GetInt() : 0;
}

void SV_GetRemoteServerName(string& outServerName)
{
	outServerName.clear();

	const char* const pszHostname = hostname ? hostname->GetString() : "";
	if (VALID_CHARSTAR(pszHostname))
		outServerName = pszHostname;
}

int SV_GetRemoteServerMaxPlayers()
{
	return gpGlobals ? gpGlobals->maxClients : 0;
}

void SV_AddRemoteServerAddress(rapidjson::Value& object,
	rapidjson::Document::AllocatorType& allocator,
	const string& serverIp, const int serverPort)
{
	object.AddMember("serverIp", rapidjson::Value(serverIp.c_str(), serverIp.length(), allocator), allocator);
	object.AddMember("serverPort", serverPort, allocator);
}

void SV_AddRemoteServerName(rapidjson::Value& object,
	rapidjson::Document::AllocatorType& allocator,
	const string& serverName)
{
	object.AddMember("serverName", rapidjson::Value(serverName.c_str(), serverName.length(), allocator), allocator);
}

bool SV_ResolveRemoteApiUrl(const char* const pszConfiguredUrl,
	const char* const pszDefaultPath, string& outUrl, string& outMessage,
	const char* const pszConfiguredName)
{
	if (VALID_CHARSTAR(pszConfiguredUrl))
	{
		outUrl = pszConfiguredUrl;
		return true;
	}

	const char* const pszBaseUrl = sv_r5_api_base_url.GetString();
	if (!VALID_CHARSTAR(pszBaseUrl))
	{
		outMessage = Format("%s is empty and sv_r5_api_base_url is empty", pszConfiguredName);
		return false;
	}

	outUrl = pszBaseUrl;
	while (!outUrl.empty() && outUrl.back() == '/')
		outUrl.pop_back();

	if (VALID_CHARSTAR(pszDefaultPath))
	{
		if (pszDefaultPath[0] != '/')
			outUrl.append("/");

		outUrl.append(pszDefaultPath);
	}

	return true;
}
