#pragma once
#ifndef CLIENT_DLL

#include <mbedtls/aes.h>
#include "engine/server/server.h"
#include <engine/host_state.h>
#include <game/server/vscript_server.h>
#include "logger.h"
#include "logger_websocket.h"
#include <networksystem/hostmanager.h>
#include "vscript/vscript.h"
#include "engine/cmd.h"
#include <stack>
#include "rtech/playlists/playlists.h"
#include <string_view>
#include <tier1\fmtstr.h>

//-----------------------------------------------------------------------------
// POINTERS
//-----------------------------------------------------------------------------
TRACKER::Logger* g_pTracker = nullptr;

//-----------------------------------------------------------------------------
// string manipulation sanitize function
//-----------------------------------------------------------------------------

static void Sanitize_AlphaNumHyphenUnderscore(std::string& input)
{
    input.erase
    (
        std::remove_if
        (
            input.begin(), input.end(), [](unsigned char c)
            {
                return !std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_';
            }
        ),

        input.end()
    );
}

static std::string Sanitize_NumbersOnly(const std::string& input)
{
    std::string sanitized = input;
    sanitized.erase
    (
        std::remove_if
        (
            sanitized.begin(), sanitized.end(), [](unsigned char c)
            {
                return !std::isdigit(c);
            }
        ),

        sanitized.end()
    );

    return sanitized;
}

static void Script_CodeCallback_BatchStatsLoaded()
{
    g_TaskQueue.Dispatch
    (
        []
        {
            if (g_pServer && g_pServerScript && g_pServer->IsActive())
                g_pServerScript->ExecuteCodeCallback("CodeCallback_BatchStatsLoaded");
        }
        , 0
    );
}

inline static std::string join_fast(const std::vector<std::string>& vec, const std::string& delimiter)
{
    if (vec.empty()) return "";

    size_t total_size = 0;
    for (const auto& s : vec) {
        total_size += s.size();
    }
    total_size += delimiter.size() * (vec.size() - 1);

    std::string result;
    result.resize(total_size);
    char* dest = &result[0];
    for (size_t i = 0; i < vec.size(); ++i)
    {
        const std::string& s = vec[i];
        memcpy(dest, s.data(), s.size());
        dest += s.size();
        if (i != vec.size() - 1)
        {
            memcpy(dest, delimiter.data(), delimiter.size());
            dest += delimiter.size();
        }
    }
    return result;
}



namespace TRACKER
{

    std::unordered_map<std::string, std::string> g_configMap;
    std::shared_timed_mutex g_configMapMutex;
    std::atomic<int64_t> last_match_id{ 0 };

    //-----------------------------------------------------------------------------
    // r5rdev_config.json config management
    //-----------------------------------------------------------------------------


    void CleanupLogs(IFileSystem* pFileSystem)
    {
        if (pFileSystem == nullptr)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "FileSystem pointer is null\n");
            return;
        }

        std::string subDir = GetSetting("logfolder");
        std::string delete_all = GetSetting("server.DELETE_ALL_LOGS");
        bool DEL_ALL = false;

        if (delete_all.empty() || delete_all == "")
        {
            DEL_ALL = false;
        }
        else
        {
            DEL_ALL = (delete_all == "true");
        }

        if (subDir.empty() || subDir == "")
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Attempted to load an invalid setting value.\n");
            return;
        }

        if (!pFileSystem->IsDirectory(subDir.c_str(), "PLATFORM"))
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Directory does not exist: %s\n", subDir.c_str());
            return;
        }

        std::string maxLogSizeSetting = GetSetting("server.MAX_LOGFILE_DIR_SIZE");
        int64_t sv_maxdir_size_t = GetMaxLogfileSize(maxLogSizeSetting.c_str());

        sv_maxdir_size_t = sv_maxdir_size_t < 3 ? 20LL : sv_maxdir_size_t;

        char searchPath[MAX_PATH];
        snprintf(searchPath, sizeof(searchPath), "%s/*.json", subDir.c_str());

        struct FileInfo {
            std::string fullPath = "";
            ssize_t fileSize = 0;
            long long fileTime = 20LL;
        };

        std::vector<FileInfo> fileInfos;

        ssize_t dirSize = 0;

        FileFindHandle_t findHandle;

        const char* fn = pFileSystem->FindFirstEx(searchPath, "PLATFORM", &findHandle);

        while (fn != nullptr)
        {
            if (!pFileSystem->FindIsDirectory(findHandle))
            {
                FileInfo info;
                info.fullPath = std::string(subDir) + "/" + fn;
                info.fileSize = pFileSystem->FSize(info.fullPath.c_str(), "PLATFORM");
                info.fileTime = pFileSystem->GetFileTime(info.fullPath.c_str(), "PLATFORM");
                dirSize += info.fileSize;
                fileInfos.push_back(info);
            }
            fn = pFileSystem->FindNext(findHandle);
        }

        pFileSystem->FindClose(findHandle);

        std::sort(fileInfos.begin(), fileInfos.end(), [](const FileInfo& a, const FileInfo& b) {
            return a.fileTime < b.fileTime;
            });

        for (const auto& fileInfo : fileInfos)
        {
            if (!DEL_ALL && dirSize <= sv_maxdir_size_t * 1024 * 1024)
            {
                break;
            }

            pFileSystem->RemoveFile(fileInfo.fullPath.c_str(), "PLATFORM");
            dirSize -= fileInfo.fileSize;
            Msg(eDLL_T::SERVER, "Tracker: Removing log: %s\n", fileInfo.fullPath.c_str());
        }

        Msg(eDLL_T::SERVER, "Tracker: Final statlog directory size: %zd bytes\n", dirSize);
    }




    void AddToConfigMap(const rapidjson::Value& value, const std::string& parentKey = "", int depth = 0)
    {
        if (depth > 30)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Object depth exceeded allocated recursion limit.. \n");
            return;
        }

        for (rapidjson::Value::ConstMemberIterator itr = value.MemberBegin(); itr != value.MemberEnd(); ++itr)
        {
            std::string key;
            if (parentKey.empty())
                key = itr->name.GetString();
            else
                key = CFmtStrN<128>("%s.%s", parentKey.c_str(), itr->name.GetString()).Get();

            if (itr->value.IsObject())
            {
                AddToConfigMap(itr->value, key, depth + 1);
            }
            else
            {
                std::string valueStr;
                if (itr->value.IsString())
                {
                    valueStr = itr->value.GetString();
                }
                else if (itr->value.IsInt())
                {
                    valueStr = std::to_string(itr->value.GetInt());
                }
                else if (itr->value.IsBool())
                {
                    valueStr = itr->value.GetBool() ? "true" : "false";
                }
                else if (itr->value.IsDouble())
                {
                    valueStr = std::to_string(itr->value.GetDouble());
                }

                if (!valueStr.empty())
                {
                    std::lock_guard<std::shared_timed_mutex> lock(g_configMapMutex);
                    g_configMap[key] = std::move(valueStr); //don't make a copy
                }
            }
        }
    }




    void LoadConfig(IFileSystem* pFileSystem, const char* configFileName)
    {
        if (!pFileSystem || !VALID_CHARSTAR(configFileName))
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Invalid filesystem or config filename in LoadConfig.\n");
            return;
        }


        if (!pFileSystem->FileExists(configFileName))
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Missing stats file: %s\n", configFileName);
            return;
        }

        FileHandle_t configFile = pFileSystem->Open(configFileName, "rt");
        if (!configFile)
        {
            Msg(eDLL_T::SERVER, "Tracker: Failed to open config file: %s\n", configFileName);
            return;
        }

        ssize_t fileLength = pFileSystem->Size(configFile);

        if (fileLength <= 0)
        {
            pFileSystem->Close(configFile);
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Config file is empty: %s\n", configFileName);
            return;
        }

        std::unique_ptr<char[]> buffer(new char[fileLength + 1]);
        ssize_t bytesRead = pFileSystem->Read(buffer.get(), fileLength, configFile);
        pFileSystem->Close(configFile);

        if (bytesRead <= 0)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Failed to read config file: %s\n", configFileName);
            return;
        }

        buffer[bytesRead] = '\0';

        rapidjson::Document document;
        if (document.Parse(buffer.get()).HasParseError())
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: JSON parse error: %s\n", rapidjson::GetParseError_En(document.GetParseError()));
            return;
        }

        if (document.IsObject())
        {
            AddToConfigMap(document);
            Msg(eDLL_T::SERVER, "Tracker: Loaded R5R.DEV config file: %s \n", configFileName);
        }
        else
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Failed to load stats config file: File was not a valid json object. \n");
        }
    }



    // get setting value by key
    std::string GetSetting(const char* key)
    {
        if (!key)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Error in [GetSetting] : key was nullptr \n");
            return "";
        }

        std::shared_lock<std::shared_timed_mutex> lock(g_configMapMutex);
        std::unordered_map<std::string, std::string>::const_iterator itr = g_configMap.find(key);

        if (itr != g_configMap.end())
        {
            return itr->second;
        }

        return "";
    }




    void ReloadConfig(const char* configFileName)
    {
        {
            std::lock_guard<std::shared_timed_mutex> lock(g_configMapMutex);
            g_configMap.clear();
        }

        LoadConfig(FileSystem(), configFileName);

        g_TaskQueue.Dispatch
        (
            []
            {
                if (g_pServer && g_pServerScript && g_pServer->IsActive())
                {
                    if (!g_pServerScript->ExecuteCodeCallback("CodeCallback_SettingsUpdate"))
                        Error(eDLL_T::SERVER, NO_ERROR, "Failed to call CodeCallback_SettingsUpdate");
                }
            },
            0
        );
    }



    //-----------------------------------------------------------------------------
    // class CURLConnectionPool
    //-----------------------------------------------------------------------------

    CURLConnectionPool::CURLConnectionPool()
    {
        for (int i = 0; i < 5; ++i)
        {
            CURL* handle = curl_easy_init();
            if (handle)
            {
                curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
                curl_easy_setopt(handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
                curl_easy_setopt(handle, CURLOPT_TCP_KEEPALIVE, 1L);
                curl_easy_setopt(handle, CURLOPT_TCP_KEEPINTVL, 60L);
                curl_easy_setopt(handle, CURLOPT_TCP_KEEPIDLE, 30L);
                pool.push(handle);
            }
            else
            {
                Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Failed to initialize CURL handle. \n");
            }
        }
    }

    CURLConnectionPool& CURLConnectionPool::GetInstance()
    {
        static CURLConnectionPool instance;
        return instance;
    }

    CURL* CURLConnectionPool::GetHandle()
    {
        std::unique_lock<std::mutex> lock(poolMutex);

        if (m_bShuttingDown.load())
            return nullptr;

        while (pool.empty())
        {
            if (!poolCond.wait_for(lock, handleWaitTimeout, [this] { return !pool.empty() || m_bShuttingDown.load(); }))
            {
                if (pool.size() < maxPoolSize)
                {
                    return CreateHandle();
                }
                else
                {
                    Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Timeout waiting for CURL handle and pool is full. \n");
                    return CreateHandle();
                }
            }

            if (m_bShuttingDown.load())
                return nullptr;
        }

        CURL* handle = pool.front();
        pool.pop();

        curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
        curl_easy_setopt(handle, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(handle, CURLOPT_TCP_KEEPINTVL, 60L);
        curl_easy_setopt(handle, CURLOPT_TCP_KEEPIDLE, 30L);

        return handle;
    }

    CURL* CURLConnectionPool::CreateHandle()
    {
        if (m_bShuttingDown.load())
            return nullptr;

        CURL* handle = curl_easy_init();

        if (handle)
        {
            curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
            curl_easy_setopt(handle, CURLOPT_TCP_KEEPALIVE, 1L);
            curl_easy_setopt(handle, CURLOPT_TCP_KEEPINTVL, 60L);
            curl_easy_setopt(handle, CURLOPT_TCP_KEEPIDLE, 30L);
        }
        else
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Failed to initialize CURL handle. \n");
        }

        return handle;
    }

    bool CURLConnectionPool::HandleCurlResult(CURL* handle, CURLcode res, const char* func)
    {
        if (res == CURLE_OK)
        {
            ReturnHandle(handle);
            return true;
        }
        else
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: CURL failed in [%s] : %s\n", func, curl_easy_strerror(res));

            switch (res)
            {
            case CURLE_COULDNT_CONNECT:
            case CURLE_COULDNT_RESOLVE_HOST:
            case CURLE_COULDNT_RESOLVE_PROXY:

                ReturnHandle(handle);
                return false;

            case CURLE_SSL_CONNECT_ERROR:
            case CURLE_SSL_CIPHER:
            case CURLE_SSL_CACERT:

                DiscardHandle(handle);
                return false;

            default:
                Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Unspecified reason: %s", curl_easy_strerror(res));
                DiscardHandle(handle);
            }
        }

        return false;
    }

    void CURLConnectionPool::ReturnHandle(CURL* handle)
    {
        //Msg(eDLL_T::SERVER, ":: Handle return called \n");

        std::lock_guard<std::mutex> lock(poolMutex);

        if (m_bShuttingDown.load())
        {
            if (handle)
                curl_easy_cleanup(handle);
        }
        else if (handle && pool.size() < maxPoolSize)
        {
            //Msg(eDLL_T::SERVER, ":: resetting \n");
            curl_easy_reset(handle);
            pool.push(handle);
            poolCond.notify_one();
        }
        else
        {
            if (handle)
            {
                //Msg(eDLL_T::SERVER, ":: Handle discarded as pool is full \n");
                curl_easy_cleanup(handle);
            }
            else
            {
                Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Attempted to return an invalid CURL handle to the pool. \n");
            }
        }
    }

    void CURLConnectionPool::DiscardHandle(CURL* handle)
    {
        std::lock_guard<std::mutex> lock(poolMutex);
        if (handle)
        {
            curl_easy_cleanup(handle);
            handle = nullptr;
        }
    }

    void CURLConnectionPool::Shutdown()
    {
        if (s_isShutdown.exchange(true))
            return;

        m_bShuttingDown.store(true);
        poolCond.notify_all();

        std::lock_guard<std::mutex> lock(poolMutex);
        while (!pool.empty())
        {
            CURL* handle = pool.front();
            if (handle)
            {
                curl_easy_cleanup(handle);
                handle = nullptr;
            }
            pool.pop();
        }
    }


    CURLConnectionPool::~CURLConnectionPool()
    {
        Shutdown();
    }
    //END CLASS CURLConnectionPool





    //UTILITY
    int64_t GetMaxLogfileSize(const char* settingValue)
    {
        if (!settingValue || settingValue[0] == '\0')
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Max log file size setting is null or empty.\n");
            return -1;
        }

        char* endPtr;
        int64_t maxSize64_t = std::strtoll(settingValue, &endPtr, 10);

        if (*endPtr != '\0' || maxSize64_t <= 0)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Invalid value for max log file size setting: %s\n", settingValue);
            return -1;
        }

        return maxSize64_t;
    }




    std::string TRACKER::GetEndingMatchID()
    {
        return std::to_string(last_match_id.load());
    }




    void TRACKER::SaveEndingMatchID()
    {
        last_match_id.store(getMatchID());
    }




    inline static std::string replace_all(std::string_view str, std::string_view from, std::string_view to)
    {
        std::string result(str);
        size_t start_pos = 0;
        while ((start_pos = result.find(from, start_pos)) != std::string::npos) {
            result.replace(start_pos, from.size(), to);
            start_pos += to.size();
        }
        return result;
    }


    std::string url_encode(const std::string& value)
    {
        return replace_all(value, "\n", "<br/>");
    }


    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp)
    {
        size_t totalSize = size * nmemb;
        userp->append((char*)contents, totalSize);
        return totalSize;
    }


    //////////////////
    /// TASKMANAGER /////////////////////////////////////////////////////////////
    //////////////////


    TaskManager& TaskManager::getInstance()
    {
        static TaskManager instance;
        return instance;
    }


    TaskManager::TaskManager()
    {
        StartWorkerThread();
    }


    TaskManager::~TaskManager()
    {
        Shutdown();
    }


    void TaskManager::Shutdown()
    {
        if (s_isShutdown.exchange(true))
            return;

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            stop_tasks_flag = true;
        }

        condVar.notify_all();
        if (workerThread.joinable())
            workerThread.join();
    }

    void TaskManager::StartWorkerThread()
    {
        workerThread = std::thread(&TaskManager::ProcessTasks, this);
    }


    void TaskManager::AddTask(const std::function<void()>& task)
    {
        std::lock_guard<std::mutex> lock(queueMutex);

        if (stop_tasks_flag.load())
            return;

        taskQueue.push(task);
        condVar.notify_one();
    }


    //keeps map lean by removing map slot for player after assignment in squirrel
    void TRACKER::TaskManager::ResetPlayerData(const char* player_oid)
    {
        if (player_oid == nullptr || std::strlen(player_oid) == 0)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: ResetPlayerData called with empty or null player_oid. \n");
            return;
        }

        std::string playerOidStr(player_oid);

        TrackerDispatch
        (
            [playerOidStr]()
            {
                std::unique_lock<std::shared_timed_mutex> lock(statsMutex, std::defer_lock);
                if (lock.try_lock_for(std::chrono::milliseconds(10000)))
                {
                    std::unordered_map<std::string, std::string>::iterator it = playerStatsMap.find(playerOidStr);
                    if (it != playerStatsMap.end())
                    {
                        playerStatsMap.erase(it);
                    }
                }
                else
                {
                    Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Could not aquire lock to remove player slot in map. \n");
                }
            }
        );
    }




    void TaskManager::ProcessTasks()
    {
        while (true)
        {
            std::function<void()> task;
            bool execute = false;

            {
                std::unique_lock<std::mutex> lock(queueMutex);
                condVar.wait(lock, [this] { return stop_tasks_flag || !taskQueue.empty(); });

                if (stop_tasks_flag && taskQueue.empty())
                {
                    lock.unlock();
                    break;
                }

                task = taskQueue.front();
                taskQueue.pop();
                execute = true;

            }

            if (execute)
            {
                try {
                    task();
                }
                catch (const std::exception& e) {

                    Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Exception in ProcessTasks: %s \n", e.what());
                }
                catch (...) {

                    Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Unknown exception in ProcessTasks \n");
                }
            }
        }
    }




    //////////////////////////////
    /// STAT FUNCTIONS FOR SQVM ///////////////////////////////////////////////////////////
    //////////////////////////////

    //input: oid, output: player stats if available in the playerstatsmap
    std::string GetPlayerJsonData(const char* player_oid)
    {
        if (!player_oid)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: [GetPlayerJsonData] Error: player_oid nullptr \n");
            return "NA";
        }

        std::shared_lock<std::shared_timed_mutex> lock(statsMutex, std::defer_lock);
        std::string safeOid(player_oid);

        if (lock.try_lock_for(std::chrono::milliseconds(500)))
        {
            std::unordered_map<std::string, std::string>::iterator it = playerStatsMap.find(safeOid);

            if (it != playerStatsMap.end())
                return it->second;
        }
        else
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Unable to acquire lock while reading stats for player: %s \n", player_oid);
        }

        return "";
    }




    /*********************************
    /     API PLAYER STATS
    /********************************/

    //called by TaskManager::RequestBatchPlayerPersistenceData
    std::string FetchBatchPlayerStats(
        const std::vector<std::string>& player_oids,
        const std::string& requestedStats,
        const std::string& requestedSettings
    )
    {
        if (player_oids.empty())
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Error: player_oid empty \n");
            return "NA";
        }

        CURL* easy_handle = CURLConnectionPool::GetInstance().GetHandle();

        if (!easy_handle)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Error: Failed to acquire curl handle from pool\n");
            return "NA";
        }

        std::string readBuffer;
        std::string identifier = GetSetting("identifier");
        Sanitize_AlphaNumHyphenUnderscore(identifier);

        CFmtStrMax urlBase("%s?TYPE=batch&KEY=%s&identifier=%s&requestedStats=%s&requestedSettings=%s&HOST_API_KEY=%s",
            TRACKER_STATS_API_ENDPOINT.c_str(), TRACKER_API_KEY.c_str(), identifier.c_str(), requestedStats.c_str(), requestedSettings.c_str(), GetSetting("apikey").c_str());
        std::string url = urlBase.Get();

        for (const std::string& oid : player_oids)
        {
            CFmtStrN<32> extra("&player_oid[]=%s", oid.c_str());
            url.append(extra.Get());
        }

        curl_easy_setopt(easy_handle, CURLOPT_URL, url.c_str());
        curl_easy_setopt(easy_handle, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(easy_handle, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(easy_handle, CURLOPT_TIMEOUT, 4L);

        CURLcode res;
        int retry = 0;
        const char* info;
        const int max_attempts = 3;

        do
        {
            res = curl_easy_perform(easy_handle);

            if (res == CURLE_OK)
            {
                break;
            }

            info = (retry >= max_attempts) ? "Tracker: Connection failed. Stats not loaded." : "Tracker: - Retrying connection...";
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Error: Batch stats-fetch: curl_easy_perform() failed: %s %s\n", curl_easy_strerror(res), info);
            retry++;

        } while (retry < max_attempts);


        bool check = CURLConnectionPool::GetInstance().HandleCurlResult(easy_handle, res, "FetchBatchPlayerStats");
        return check == true ? readBuffer : "NA";
    }

    //input: stores json struct in the map, later used to build stats table when fetched from scripts.
    void TaskManager::RequestBatchPlayerPersistenceData(
        std::vector<std::string>&& player_oids,
        std::vector<std::string>&& requestedStats,
        std::vector<std::string>&& requestedSettings)
    {
        AddTask([player_oids = std::move(player_oids), requestedStats = std::move(requestedStats), requestedSettings = std::move(requestedSettings), this]()
            {
                if (player_oids.empty())
                {
                    Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Error in [RequestBatchPlayerPersistenceData] : player_oids was empty \n");
                    Script_CodeCallback_BatchStatsLoaded();
                    return;
                }

                std::string statsJoined = join_fast(requestedStats, ",");
                std::string settingsJoined = join_fast(requestedSettings, ",");
                std::string stats_json = FetchBatchPlayerStats(player_oids, statsJoined, settingsJoined);

                rapidjson::Document document;
                if (document.Parse(stats_json.c_str()).HasParseError())
                {
                    Error(eDLL_T::SERVER, NO_ERROR, "Tracker: JSON parsing failed: %s\n", rapidjson::GetParseError_En(document.GetParseError()));
                    Script_CodeCallback_BatchStatsLoaded();
                    return;
                }

                if (!document.IsObject())
                {
                    Error(eDLL_T::SERVER, NO_ERROR, "Tracker: JSON root is not an object\n");
                    Script_CodeCallback_BatchStatsLoaded();
                    return;
                }

                if (document.HasMember("error"))
                {
                    const char* errorMsg = "";
                    if (document["error"].IsString())
                        errorMsg = document["error"].GetString();

                    Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Error fetching stats -- %s", errorMsg ? errorMsg : "");
                    Script_CodeCallback_BatchStatsLoaded();
                    return;
                }

                for (rapidjson::Value::ConstMemberIterator itr = document.MemberBegin(); itr != document.MemberEnd(); ++itr)
                {
                    std::string player_oid = itr->name.GetString();

                    if (!itr->value.IsObject())
                        continue;

                    rapidjson::StringBuffer buffer;
                    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
                    itr->value.Accept(writer);

                    std::string player_stats_json = buffer.GetString();

                    bool has_lock = false;
                    std::unique_lock<std::shared_timed_mutex> lock(statsMutex, std::defer_lock);
                    if (lock.try_lock_for(std::chrono::milliseconds(3000)))
                    {
                        playerStatsMap[player_oid] = player_stats_json;
                        has_lock = true;
                    }

                    if (!has_lock)
                        Error(eDLL_T::SERVER, NO_ERROR, "Tracker: failed to acquire lock to write player stats into map\n");
                }

                Script_CodeCallback_BatchStatsLoaded();
            });
    }



    //called by TaskManager::RequestPlayerPersistenceData
    std::string FetchPlayerStats(const char* player_oid, const char* requestedStats, const char* requestedSettings)
    {
        if (!player_oid)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Error: player_oid nullptr \n");
            return "NA";
        }

        CURL* easy_handle = CURLConnectionPool::GetInstance().GetHandle();

        if (!easy_handle)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Failed to acquire curl handle from pool\n");
            return "NA";
        }

        std::string readBuffer;
        std::string identifier = GetSetting("identifier");
        Sanitize_AlphaNumHyphenUnderscore(identifier);

        CFmtStrMax urlStr("%s?KEY=%s&requestedStats=%s&player_oid=%s&identifier=%s&requestedSettings=%s&HOST_API_KEY=%s",
            TRACKER_STATS_API_ENDPOINT.c_str(), TRACKER_API_KEY.c_str(), requestedStats, player_oid, identifier.c_str(), requestedSettings, GetSetting("apikey").c_str());

        curl_easy_setopt(easy_handle, CURLOPT_URL, urlStr.Get());
        curl_easy_setopt(easy_handle, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(easy_handle, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(easy_handle, CURLOPT_TIMEOUT, 3L);

        CURLcode res = curl_easy_perform(easy_handle);

        if (res != CURLE_OK) {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Stat lookup: curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        }

        bool check = CURLConnectionPool::GetInstance().HandleCurlResult(easy_handle, res, "FetchPlayerStats");
        return check == true ? readBuffer : "NA";
    }




    //for individual players, stores data in playerStatsMap
    void TaskManager::RequestPlayerPersistenceData(std::string&& player_oid, std::vector<std::string>&& requestedStats, std::vector<std::string>&& requestedSettings)
    {
        if (player_oid.empty())
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Error in [RequestPlayerPersistenceData] : empty player oid\n");
            return;
        }

        AddTask
        (
            [player_oid = std::move(player_oid), requestedStats = std::move(requestedStats), requestedSettings = std::move(requestedSettings), this]()
            {
                std::string playerOidStr(player_oid);
                std::string statsJoined = join_fast(requestedStats, ",");
                std::string settingsJoined = join_fast(requestedSettings, ",");
                std::string stats = FetchPlayerStats(playerOidStr.c_str(), statsJoined.c_str(), settingsJoined.c_str());
                bool has_lock = false;

                {
                    std::unique_lock<std::shared_timed_mutex> lock(statsMutex, std::defer_lock);
                    if (lock.try_lock_for(std::chrono::milliseconds(3000)))
                    {
                        playerStatsMap[playerOidStr] = stats;
                        has_lock = true;
                    }
                    else
                    {
                        Error(eDLL_T::SERVER, NO_ERROR, "Tracker: failed to aquire lock to write player stats into map for: %s\n", playerOidStr.c_str());
                    }
                }

                g_TaskQueue.Dispatch
                (
                    [playerOid = Sanitize_NumbersOnly(playerOidStr)]
                    {
                        const char* const oid = playerOid.c_str();
                        bool success = CALL_SERVER_SCRIPT_FUNC("CodeCallback_PlayerStatsReady", MakeNoCopyStr(oid), "void functionref( string )");
                        if (!success)
                            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Failed to execute CodeCallback_PlayerStatsReady for '%s'.\n", oid);
                    },
                    0
                );

            }
        );
    }




    /*********************************
    /       API LIVE STATS
    /********************************/

    //separate thread
    void RunUpdateLiveStats(std::string stats_json_copy)
    {
        const char* stats_json = stats_json_copy.c_str();

        if (!stats_json)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: [RunUpdateLiveStats] Failed: Nullptr\n");
            return;
        }

        CURL* curl = CURLConnectionPool::GetInstance().GetHandle();

        if (!curl)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: [RunUpdateLiveStats] Failed to acquire curl handle from pool\n");
            return;
        }

        rapidjson::Document doc;
        doc.SetObject();
        rapidjson::Document::AllocatorType& allocator = doc.GetAllocator();

        rapidjson::Document stats;
        stats.Parse(stats_json);
        if (stats.HasParseError())
        {
            size_t offset = stats.GetErrorOffset();
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: [RunUpdateLiveStats] Failed to parse stats JSON at offset %zu: %s\n", offset, rapidjson::GetParseError_En(stats.GetParseError()));
            CURLConnectionPool::GetInstance().ReturnHandle(curl);
            return;
        }

        std::string uniquekey = GetSetting("apikey");
        Sanitize_AlphaNumHyphenUnderscore(uniquekey);

        std::string identifier = GetSetting("identifier");
        Sanitize_AlphaNumHyphenUnderscore(identifier);

        std::string serverName = hostname->GetString();

        doc.AddMember("stats", stats, allocator);
        doc.AddMember("servername", rapidjson::Value(serverName.c_str(), allocator), allocator);
        doc.AddMember("KEY", rapidjson::Value(TRACKER_API_KEY.c_str(), allocator), allocator);
        doc.AddMember("HOST_API_KEY", rapidjson::Value(uniquekey.c_str(), allocator), allocator);
        doc.AddMember("identifier", rapidjson::Value(identifier.c_str(), allocator), allocator);

        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        doc.Accept(writer);

        std::string jsonData = buffer.GetString();

        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, TRACKER_STATS_API_ENDPOINT.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonData.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

        CURLcode res = curl_easy_perform(curl);

        curl_slist_free_all(headers);

        CURLConnectionPool::GetInstance().HandleCurlResult(curl, res, "RunUpdateLiveStats");
    }




    //used to keep livestats up to date for matchmaking
    void UpdateLiveStats(std::string stats_json)
    {
        if (stats_json.empty())
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: [UpdateLiveStats] failed: stats_json empty\n");
            return;
        }

        TrackerDispatch([stats_json = std::move(stats_json)]() {RunUpdateLiveStats(stats_json); });
    }





    /*********************************
    /     API PLAYER COUNT UPDATES
    /********************************/

    // called by UPDATE_PLAYER_COUNT and dispatched to task manager
    void PlayerCountUpdate(std::string action, std::string player, std::string oid, std::string count, std::string discordHook)
    {
        CURL* curl = CURLConnectionPool::GetInstance().GetHandle();

        if (!curl)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Failed to acquire curl handle from pool\n");
            return;
        }


        CFmtStr postData("servername=%s&action=%s&player_name=%s&OID=%s&current_count=%s&DISCORD_HOOK=%s&KEY=%s",
            hostname->GetString(), action.c_str(), player.c_str(), oid.c_str(), count.c_str(), discordHook.c_str(), TRACKER_API_KEY.c_str());

        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData.Get());
        curl_easy_setopt(curl, CURLOPT_URL, TRACKER_PLAYER_COUNT_ENDPOINT);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);

        CURLcode res = curl_easy_perform(curl);

        CURLConnectionPool::GetInstance().HandleCurlResult(curl, res, "PlayerCountUpdate");
    }

    //needs re worked - dont allow setting discord hooks in playlist file
   // Sends join/leave data to api for various use. replace DISCORD_HOOK with internal r5r.dev functions via string starting with __apicall_
    void UPDATE_PLAYER_COUNT(const char* action, const char* player, const char* OID, const char* count)
    {
        std::string webhookUrl = GetSetting("webhooks.PLAYERS_WEBHOOK");
        if (webhookUrl.empty())
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Tried to send player count update from scripts but setting webhooks.PLAYERS_WEBHOOK is undefined.");
            return;
        }

        std::string actionStr(action);
        std::string playerStr(player);
        std::string OIDStr(OID);
        std::string countStr(count);

        TrackerDispatch
        (
            [actionStr = std::move(actionStr), playerStr = std::move(playerStr), OIDStr = std::move(OIDStr), countStr = std::move(countStr) , webhookUrl = std::move(webhookUrl)]()
            {
                PlayerCountUpdate(actionStr, playerStr, OIDStr, countStr, webhookUrl);
            }
        );
    }




    /*********************************
   /       API END GAME UPDATES
   /********************************/

   //by ref
    void EndMatchUpdate(std::string recap)
    {
        if (recap.empty())
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: [EndMatchUpdate] Recap was empty...\n");
            return;
        }

        const std::string matchID = GetEndingMatchID();

        if (matchID.empty())
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: [EndMatchUpdate] matchID was empty...\n");

        std::string discord_hook = GetSetting("webhooks.MATCHES_WEBHOOK");
        if (discord_hook.empty())
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Tried to send end match recap update to discord from scripts, but webhooks.MATCHES_WEBHOOK was undefined.\n");
            return;
        }

        TRACKER::Logger& logger = TRACKER::Logger::getInstance();

        const std::string dir_setting = GetSetting("logfolder");
        const std::string filename = logger.GetLatestFile("platform/" + dir_setting, GetEndingMatchID());

        if (filename.empty())
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: No log found. Aborting API connection [EndMatchUpdate]...\n");
            return;
        }

        const std::string serverName = hostname->GetString();

        size_t recap_len = recap.length();
        if (recap_len > static_cast<size_t>(INT_MAX))
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Failed to cast recap string size to int, result would be truncated. \n");
            return;
        }

        CURL* curl = CURLConnectionPool::GetInstance().GetHandle();

        if (!curl)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Failed to acquire curl handle from pool\n");
            return;
        }

        //the length should never exceed int.
        char* escaped_recap = curl_easy_escape(curl, recap.c_str(), static_cast<int>(recap_len));

        if (!escaped_recap)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Failed to encode recap string\n");
            CURLConnectionPool::GetInstance().ReturnHandle(curl);
            return;
        }

        CFmtStrN<648> postData("servername=%s&matchID=%s&recap=%s&DISCORD_HOOK=%s&KEY=%s",
            serverName.c_str(), matchID.c_str(), escaped_recap, discord_hook.c_str(), TRACKER_API_KEY.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData.Get());
        curl_easy_setopt(curl, CURLOPT_URL, "https://r5r.dev/api/endmatch.php");
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

        CURLcode res = curl_easy_perform(curl);

        curl_free(escaped_recap); //must free

        CURLConnectionPool::GetInstance().HandleCurlResult(curl, res, "EndMatchUpdate");
    }




    //sends recap data to discord channel
    void NOTIFY_END_OF_MATCH(const char* recap)
    {
        if (!recap)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Error in [NOTIFY_END_OF_MATCH] : recap or DISCORD_HOOK was nullptr");
            return;
        }

        //making a copy of recap/discord which can potentially be modified in sqvm
        std::string recap_string(recap);

        TrackerDispatch([recap_string = std::move(recap_string)] { EndMatchUpdate(recap_string); });
    }



    /*********************************
    /     API VERIFY EA ACCOUNT
    /********************************/

    // returns api response status for website ea account verification from r5r.dev
    const std::string VerifyEaAccount(const std::string& token, const std::string& OID, const std::string& ea_name)
    {
        if (token.empty() || ea_name.empty())
            return "0";

        CURL* curl = CURLConnectionPool::GetInstance().GetHandle();

        if (!curl)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Failed to acquire curl handle from pool\n");
            return "8";
        }

        CFmtStr postData("token=%s&ea_acc=%s&OID=%s&KEY=%s", token.c_str(), ea_name.c_str(), OID.c_str(), TRACKER_API_KEY.c_str());

        std::string readBuffer;
        curl_easy_setopt(curl, CURLOPT_URL, "https://r5r.dev/api/verify.php");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData.Get());
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

        CURLcode res = curl_easy_perform(curl);

        bool check = CURLConnectionPool::GetInstance().HandleCurlResult(curl, res, "VERIFY_EA_ACCOUNT");

        Warning(eDLL_T::SERVER, "Tracker: Response for %s: %s\n", ea_name.c_str(), readBuffer.c_str());

        return check == true ? readBuffer : "8";
    }




    /*********************************
   /     API FETCH GLOBAL SETTINGS
   /********************************/

   // returns response settings based on query
    std::string FetchGlobalSettings(const char* query)
    {
        if (!query || strcmp(query, "") == 0)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Error: query parameter is null or empty\n");
            return "";
        }

        CURL* curl = CURLConnectionPool::GetInstance().GetHandle();

        if (!curl)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Failed to acquire curl handle from pool\n");
            return "";
        }

        std::string readBuffer;
        CFmtStr postfields("KEY=%s&query=%s&identifier=%s&HOST_API_KEY=%s", TRACKER_API_KEY.c_str(), query, GetSetting("identifier").c_str(), GetSetting("apikey").c_str());

        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postfields.Get());
        curl_easy_setopt(curl, CURLOPT_URL, "https://r5r.dev/api/globalsettings.php");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

        CURLcode res = curl_easy_perform(curl);

        bool check = CURLConnectionPool::GetInstance().HandleCurlResult(curl, res, "FetchGlobalStats");

        return check == true ? readBuffer : "";
    }




    //////////////////////////////
    /// ENCRYPTION OBJECT ///////////////////////////////////////////////////////////
    //////////////////////////////


    const std::string TRACKER::Encryption::base64_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"
        "0123456789+/";




    // created with eObj && only encryption bool true
    std::vector<uint8_t> TRACKER::Encryption::hex2bytes(const std::string& hex) {
        if (hex.length() % 2 != 0) {
            throw std::invalid_argument("Tracker: Hex string has an odd length");
        }

        std::vector<uint8_t> bytes;

        for (unsigned int i = 0; i < hex.length(); i += 2) {
            std::string byteString = hex.substr(i, 2);
            char* end;
            long byte = std::strtol(byteString.c_str(), &end, 16);

            if (*end != '\0' || byte < 0 || byte > 0xFF) {
                throw std::invalid_argument("Tracker: Invalid hex value: " + byteString);
            }

            bytes.push_back(static_cast<uint8_t>(byte));
        }

        return bytes;
    }




    // created with eObj && only encryption bool true
    std::string TRACKER::Encryption::bbase64Encode(std::vector<uint8_t> bytes_to_encode) {
        std::string ret;
        int i = 0;
        int j = 0;
        uint8_t char_array_3[3] = { 0 };
        uint8_t char_array_4[4] = { 0 };

        for (unsigned int in_len = 0; in_len < bytes_to_encode.size(); ++in_len) {
            char_array_3[i++] = bytes_to_encode[in_len];
            if (i == 3) {
                char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
                char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
                char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
                char_array_4[3] = char_array_3[2] & 0x3f;

                for (i = 0; (i < 4); i++)
                    ret += base64_chars[char_array_4[i]];
                i = 0;
            }
        }

        if (i) {
            for (j = i; j < 3; j++)
                char_array_3[j] = '\0';

            char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
            char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
            char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
            char_array_4[3] = char_array_3[2] & 0x3f;

            for (j = 0; (j < i + 1); j++)
                ret += base64_chars[char_array_4[j]];

            while ((i++ < 3))
                ret += '=';
        }

        return ret;
    }



    // created with eObj && only encryption bool true
    std::string TRACKER::Encryption::doEncrypt(const std::string& plainText, const std::vector<uint8_t>& keyBytes, const std::vector<uint8_t>& ivBytes)
    {
        std::vector<uint8_t> plainTextBytes(plainText.begin(), plainText.end());
        size_t length = plainTextBytes.size();
        const size_t AES_BLOCKLEN = 16;
        size_t padded_length = length + AES_BLOCKLEN - (length % AES_BLOCKLEN);
        plainTextBytes.resize(padded_length);

        int padding = static_cast<int>(padded_length - length);
        for (size_t i = length; i < padded_length; i++)
        {
            plainTextBytes[i] = static_cast<uint8_t>(padding);
        }

        std::vector<uint8_t> cipherTextBytes(padded_length);

        mbedtls_aes_context aes;
        mbedtls_aes_init(&aes);

        int ret = mbedtls_aes_setkey_enc(&aes, keyBytes.data(), static_cast<unsigned int>(keyBytes.size() * 8));
        if (ret != 0)
        {
            mbedtls_aes_free(&aes);
            throw std::runtime_error("Tracker: Failed to set encryption key, error code: " + std::to_string(ret));
        }

        unsigned char iv[16];
        if (ivBytes.size() < 16)
        {
            mbedtls_aes_free(&aes);
            throw std::runtime_error("Tracker: IV is too short");
        }

        std::copy(ivBytes.begin(), ivBytes.begin() + 16, iv);

        ret = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_ENCRYPT, padded_length, iv, plainTextBytes.data(), cipherTextBytes.data());
        mbedtls_aes_free(&aes);
        if (ret != 0)
        {
            throw std::runtime_error("Tracker: AES CBC encryption failed, error code: " + std::to_string(ret));
        }

        std::string cipherTextBase64 = bbase64Encode(cipherTextBytes);
        return cipherTextBase64;
    }




    //////////////////////////////
    /// TRACKER OBJECT ///////////////////////////////////////////////////////////
    //////////////////////////////


   /*********************************
   /     Logger instance
   /********************************/

    Logger::Logger() : filePath("")
    {
        if (FileSystem() != nullptr)
            LoadConfig(FileSystem(), TRACKER_CONFIG);
        else
        {
            Error(eDLL_T::SERVER, NOERROR, "Tracker: Filesystem not initialized, aborting.");
            return;
        }

        keyHex = this->eObj.hex2bytes("c7abf6c3574e60bb7e8c2945ff21ec53");
        ivHex = this->eObj.hex2bytes("ac6fe374229715e475928b70ab53648e");
        SetLogState(LogState::Busy, false);
        SetLogState(LogState::Ready, true);

        std::string max_buffer = GetSetting("settings.CVAR_MAX_BUFFER");

        if (!max_buffer.empty())
        {
            try
            {
                CVAR_MAX_BUFFER = std::stoi(max_buffer);
            }
            catch (...)
            {
                Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Invalid data for setting CVAR_MAX_BUFFER\n");
            }
        }

        // Initialize WebSocket connection to tracker server if enabled
        bool bConfigSet = !GetSetting("server.USE_WEB_SOCKETS").empty();
        bool bUseWebsockets = GetSetting("server.USE_WEB_SOCKETS") == "true";
        if (bConfigSet)
            tracker_ws_enable.SetValue(bUseWebsockets);

        bool bReconnectOnNewGame = GetSetting("server.RECONNECT_ON_NEWGAME") == "true";
        tracker_ws_reconnect_on_newgame.SetValue(bReconnectOnNewGame ? "1" : "0");

        if (tracker_ws_enable.GetBool())
        {
            std::string trackerHostStr = GetSetting("server.TRACKER_HOST");
            const char* trackerHost = trackerHostStr.empty() ? tracker_ws_hostname.GetString() : trackerHostStr.c_str();
            int trackerPort = tracker_ws_port.GetInt();

            TrackerSocketSystem()->Connect(trackerHost, trackerPort > 0 ? trackerPort : TRACKER_WS_PORT);
        }
        else
        {
            Msg(eDLL_T::SERVER, "TrackerSocket: WebSocket disabled by config (%s)\n", bConfigSet ? "\"server.USE_WEB_SOCKETS\": \"false\"" : "Convar tracker_ws_enable is false");
        }
    }

    Logger::~Logger()
    {
        Shutdown();
    }

    void Logger::Shutdown()
    {
        if (s_isShutdown.exchange(true))
            return;

        StopLoggingThread();
        if (apiThread.joinable())
            apiThread.join();

        CloseLogFile();
        g_pTracker = nullptr;
    }

    Logger& Logger::getInstance()
    {
        static Logger instance;
        return instance;
    }

    /*********************************
    /     Utility Functions
    /********************************/

    void Logger::SetLogState(LogState flag, bool value)
    {
        if (value)
            StateBits.fetch_or(static_cast<uint8_t>(flag));
        else
            StateBits.fetch_and(static_cast<uint8_t>(~static_cast<uint8_t>(flag)));
    }




    bool Logger::GetLogState(LogState flag) const
    {
        return (StateBits.load() & static_cast<uint8_t>(flag)) != 0;
    }




    Logger::LogState Logger::intToLogState(int flag)
    {
        switch (flag)
        {
        case 1: return Logger::LogState::Ready;
        case 2: return Logger::LogState::Busy;
        case 3: return Logger::LogState::Safe;
        default: return Logger::LogState::None;
        }
    }



    //TODO: add state check
    void TRACKER::Logger::StartLogging()
    {
        finished = false;

        try {
            logThread = std::thread(&Logger::LogToFile, this);
        }
        catch (const std::exception& e) {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Exception when starting LogToFile thread: %s\n", e.what());
        }
        catch (...) {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Unknown exception when starting LogToFile thread.\n");
        }
    }




    std::string TRACKER::Logger::GetLatestFile(const std::string& directoryPath, std::string matchID = "")
    {
        if (directoryPath.empty())
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: [GetLatestFile] : empty.\n");
            return "";
        }

        IFileSystem* pFileSystem = FileSystem();

        if (!matchID.empty())
        {
            std::string Path = directoryPath + "/" + matchID + ".json";
            if (pFileSystem->FileExists(Path.c_str(), "GAME"))
            {
                return Path;
            }
            else
            {
                Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Specified matchID file %s does not exist.\n", Path.c_str());
                return "";
            }
        }

        Error(eDLL_T::SERVER, NO_ERROR, "Tracker: MatchID was empty.\n");
        return "";
    }


    // function to split a string
    std::vector<std::string> Logger::splitString(std::string str, const std::string& delimiter)
    {
        std::vector<std::string> lines;
        size_t pos = 0;
        std::string token;
        while ((pos = str.find(delimiter)) != std::string::npos) {
            token = str.substr(0, pos);
            lines.push_back(token);
            str.erase(0, pos + delimiter.length());
        }
        lines.push_back(str);
        return lines;
    }


    //atomic
    bool Logger::IsLogging()
    {
        return !finished.load();
    }



    /*********************************
    /     Tracker & LogEvent Thread
    /********************************/

    // ship to stats server
    void TRACKER::Logger::SendLogToAPI() //This should be compressed
    {
        std::string matchID = GetEndingMatchID();

        if (matchID.empty() || matchID == "0")
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: No log found. Aborting API connection...\n");
            CallClosure();
            return;
        }

        std::string dir_setting = GetSetting("logfolder");
        std::string logFilePath = GetLatestFile("platform/" + dir_setting, matchID);

        if (logFilePath.empty())
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: No log found. Aborting API connection...\n");
            CallClosure();
            return;
        }

        std::string logdata;
        {
            std::lock_guard<std::mutex> flock(file_mtx);
            std::ifstream lastlog(logFilePath);
            if (lastlog.is_open())
            {
                std::stringstream ss;
                ss << lastlog.rdbuf();
                logdata = ss.str();
                lastlog.close();
                Warning(eDLL_T::SERVER, "Tracker: Attempting to send...\n");
            }
            else
            {
                Error(eDLL_T::SERVER, NO_ERROR, "Tracker: !!! Couldn't open logfile for shipping.\n");
                CallClosure();
                return;
            }
        }

        CURL* curl = CURLConnectionPool::GetInstance().GetHandle();

        if (!curl)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Failed to acquire curl handle from pool\n");
            CallClosure();
            return;
        }

        //DevMsg(eDLL_T::SERVER, "Curl initialized...\n");
        std::string trackerEndpoint = GetSetting("server.TRACKER_ENDPOINT");
        if (!trackerEndpoint.empty())
            curl_easy_setopt(curl, CURLOPT_URL, trackerEndpoint.c_str());
        else
            curl_easy_setopt(curl, CURLOPT_URL, "https://r5r.dev/api/tracker.php");

        std::string serverName = hostname->GetString();
        std::string serverMap = g_pHostState->m_levelName;
        std::string gameType = v_Playlists_GetCurrent();
        std::string identifier = GetSetting("identifier");
        std::string uniquekey = GetSetting("apikey");
        //std::string gameMode = mp_gamemode->GetString(); //not used yet

        Sanitize_AlphaNumHyphenUnderscore(identifier);
        Sanitize_AlphaNumHyphenUnderscore(uniquekey);

        if (identifier.empty())
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: empty identifier. \n");

        if (uniquekey.empty())
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Invalid characters in uniquekey. \n");

        //DevMsg(eDLL_T::SERVER, "Server name: %s \n", serverName.c_str());
        //DevMsg(eDLL_T::SERVER, "Server map: %s \n", serverMap.c_str());
        //DevMsg(eDLL_T::SERVER, "MatchID to ship: %s \n", matchID.c_str());

        std::string logdata_url_encoded = url_encode(logdata);

        std::string boundary = "----10KEWLBEANS0101017MA4YWxkTrZu0gW132";
        std::ostringstream body;


        body << "--" << boundary << "\r\n";
        body << "Content-Disposition: form-data; name=\"ServerNAME\"\r\n\r\n";
        body << serverName << "\r\n";

        body << "--" << boundary << "\r\n";
        body << "Content-Disposition: form-data; name=\"GameTYPE\"\r\n\r\n";
        body << gameType << "\r\n";

        body << "--" << boundary << "\r\n";
        body << "Content-Disposition: form-data; name=\"MatchID\"\r\n\r\n";
        body << matchID << "\r\n";

        body << "--" << boundary << "\r\n";
        body << "Content-Disposition: form-data; name=\"Map\"\r\n\r\n";
        body << serverMap << "\r\n";

        body << "--" << boundary << "\r\n";
        body << "Content-Disposition: form-data; name=\"CODE\"\r\n\r\n";
        body << TRACKER_API_KEY << "\r\n";

        body << "--" << boundary << "\r\n";
        body << "Content-Disposition: form-data; name=\"identifier\"\r\n\r\n";
        body << identifier << "\r\n";

        body << "--" << boundary << "\r\n";
        body << "Content-Disposition: form-data; name=\"HOST_API_KEY\"\r\n\r\n";
        body << uniquekey << "\r\n";

        body << "--" << boundary << "\r\n";
        body << "Content-Disposition: form-data; name=\"Logdata\"; filename=\"logdata.json\"\r\n";
        body << "Content-Type: application/json\r\n\r\n";
        body << logdata_url_encoded << "\r\n";

        body << "--" << boundary << "--\r\n";


        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, ("Content-Type: multipart/form-data; boundary=" + boundary).c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        // Set POST fields
        std::string postBody = body.str();
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postBody.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(postBody.length()));

        std::string readBuffer;
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

        CURLcode res = curl_easy_perform(curl);

        if (res != CURLE_OK)
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: curl failed: %s\n", curl_easy_strerror(res));
        else
            Warning(eDLL_T::SERVER, "Tracker: Response: %s\n", readBuffer.c_str());

        curl_slist_free_all(headers);

        CURLConnectionPool::GetInstance().HandleCurlResult(curl, res, "SendLogToAPI");

        std::string autoDeleteSetting = GetSetting("server.AUTO_DELETE_STATLOGS");
        if (autoDeleteSetting == "true")
        {
            TRACKER::CleanupLogs(FileSystem());
        }

        CallClosure();
    }




    //-----------------------------------------------------------------------------
    // File operations
    //-----------------------------------------------------------------------------


    bool TRACKER::Logger::OpenLogFile(const std::filesystem::path& Path)
    {
        if (Path.empty())
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Error [openLogFile] : Path empty\n");
            return false;
        }

        std::lock_guard<std::mutex> fileLock(fileMutex);
        logFile.open(Path, std::ios_base::app);

        if (!logFile)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Unable to open log file: '%s'. Check permissions and disk space.\n", Path.string().c_str());
            return false;
        }

        return true;
    }




    void TRACKER::Logger::CloseLogFile()
    {
        std::lock_guard<std::mutex> fileLock(fileMutex);
        if (logFile.is_open())
        {
            logFile.close();
        }
    }




    void TRACKER::Logger::WriteBufferToFile(const std::deque<std::string>& q_buffer)
    {
        std::lock_guard<std::mutex> guard(fileMutex);

        if (!logFile.good())
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: The log file stream is in a bad state before writing.\n");
            return;
        }

        for (const std::string& logstring : q_buffer)
        {
            logFile << logstring << "\n";
        }

        logFile.flush();
    }



    void TRACKER::Logger::LogToFile()
    {
        std::deque<std::string> writeBuffer;

        this->SetLogState(LogState::Busy, true);
        this->SetLogState(LogState::Ready, true);

        try
        {
            for (; ; )
            {
                {
                    std::unique_lock<std::mutex> lock(mtx);
                    cvLog.wait(lock, [this] { return !logQueue.empty() || finished; });

                    if (buffer.size() >= CVAR_MAX_BUFFER || (finished && !buffer.empty()))
                    {
                        std::swap(buffer, writeBuffer);
                    }

                    while (!logQueue.empty())
                    {
                        buffer.push_back(logQueue.front());
                        logQueue.pop();
                    }
                }

                if (!writeBuffer.empty())
                {
                    WriteBufferToFile(writeBuffer);
                    writeBuffer.clear();
                }

                if (finished && buffer.empty() && writeBuffer.empty())
                {
                    break;
                }
            }
        }
        catch (const std::exception& e)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Exception in LogToFile thread: %s\n", e.what());
        }
        catch (...)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Unknown exception in LogToFile thread.\n");
        }

        this->SetLogState(LogState::Safe, false);
        this->SetLogState(LogState::Busy, false);
        this->SetLogState(LogState::Ready, false);
    }





    //-----------------------------------------------------------------------------
    // Thread management
    //-----------------------------------------------------------------------------


    //Log controlled stop logging
    void Logger::StopLoggingThread()
    {
        if (IsLogging())
        {
            finished = true;
            setMatchID(0);

            {
                std::unique_lock<std::mutex> lock(mtx);
                cvLog.notify_all();
            }

            Warning(eDLL_T::SERVER, "Tracker: Stopping logging thread...\n\n");

            if (logThread.joinable())
            {
                logThread.join();
            }
            else
            {
                SetLogState(LogState::Busy, false);
            }

            bool wait = WaitForState(LogState::Busy, false, 5000);
            if (!wait)
            {
                Warning(eDLL_T::SERVER, "Tracker: BUSY STATE FAILED TO RELEASE IN STOPLOGGING\n\n");
                return;
            }

            CallClosure();

        }
        else
        {
            //DevMsg(eDLL_T::RTECH, "No active logging thread.\n\n");
        }
    }




    void TRACKER::Logger::handleNewMatch(const char* matchID)
    {
        if (!matchID)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: matchID was nullptr at handlenewmatch\n");
            return;
        }

        if (IsLogging())
        {
            Warning(eDLL_T::SERVER, "Tracker: WARNING: LOG THREAD WAS RUNNING DURING HANDLE NEW MATCH\n");
            StopLoggingThread();
        }

        if (tracker_ws_reconnect_on_newgame.GetBool())
            TrackerSocketSystem()->Reconnect();

        StartLogging();
        Warning(eDLL_T::SERVER, ":::::::::::::::::::::::::::::::::::::: Logging thread started :::::::\n");
        Warning(eDLL_T::SERVER, ":::::: Match started with MatchID:  [  %s  ] :::::::\n", matchID);
    }




    void TRACKER::Logger::CallClosure()
    {
        CloseLogFile();
        ResetLogPath();
        SetLogState(LogState::Busy, false);
        SetLogState(LogState::Ready, true);
    }


    void TRACKER::Logger::StopLogging(bool sendToAPI)
    {
        StopLogging_Async(sendToAPI);
    }


    // SQVM controlled-Stop logging true/false for api stat send
    void TRACKER::Logger::StopLogging_Async(bool sendToAPI) //
    {
        SaveEndingMatchID();

        if (!logThread.joinable())
        {
            //DevMsg(eDLL_T::RTECH, "No active logging thread to stop.\n");
            return;
        }

        Warning(eDLL_T::SERVER, "Tracker: Logger thread stopping...");

        {
            std::unique_lock<std::mutex> lock(mtx);
            finished = true;
            setMatchID(0);
            cvLog.notify_all();
        }

        bool close = WaitForState(LogState::Busy, false, 5000);

        if (!close)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Logger wait time expired during shutdown...\n");
            return;
        }
        else
        {
            Warning(eDLL_T::SERVER, "done..\n");
        }

        if (logThread.joinable())
        {
            logThread.join();
        }

        if (sendToAPI)
        {
            //DevMsg(eDLL_T::SERVER, " .. Shipping logfile to tracker https://r5r.dev \n");

            {
                std::lock_guard<std::mutex> apilock(file_mtx);
                if (apiThread.joinable())
                {
                    apiThread.join();
                }
                apiThread = std::thread(&Logger::SendLogToAPI, this);
            }
        }
        else
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Stats shipping omitted;\n");
            CallClosure();
        }

    }




    void TRACKER::Logger::UpdateMatchId(const std::string& matchId)
    {
        std::unique_lock<std::shared_mutex> lock(pathMutex);
        currentMatchId = matchId;
        pCurrentLogPath = nullptr;
    }




    void TRACKER::Logger::ResetLogPath()
    {
        std::unique_lock<std::shared_mutex> writeLock(pathMutex);
        filePath.clear();
        pCurrentLogPath = nullptr;
    }



    std::filesystem::path* TRACKER::Logger::InitializeAndGetLogPath()
    {
        std::unique_lock<std::shared_mutex> lock(pathMutex);

        if (pCurrentLogPath == nullptr)
        {
            std::string folder_setting = GetSetting("logfolder");
            std::string folderPathStr = !folder_setting.empty() ? folder_setting : "eventlogs";
            std::filesystem::path dirPath = std::filesystem::path("platform") / folderPathStr;

            bool dir_created = false;

            try
            {
                dir_created = std::filesystem::create_directories(dirPath);

                if (dir_created)
                {
                    Warning(eDLL_T::SERVER, "Tracker: Created logging directory: '%s'\n", dirPath.string().c_str());
                }
            }

            catch (const std::filesystem::filesystem_error& e)
            {
                Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Failed to create logging directory: '%s'. Error: %s\n", dirPath.string().c_str(), e.what());
                return nullptr;
            }

            CFmtStrN<128> filename("%s.json", currentMatchId.c_str());
            filePath = dirPath / filename.Get();
            pCurrentLogPath = &filePath;
        }
        return pCurrentLogPath;
    }





    bool Logger::WaitForState(const LogState state, bool flag, const int timeout_in_ms)
    {
        std::chrono::milliseconds timeout(timeout_in_ms);
        std::chrono::time_point<std::chrono::steady_clock> start = std::chrono::steady_clock::now();

        while (GetLogState(state) != flag)
        {
            if (std::chrono::steady_clock::now() - start > timeout)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        return true;
    }




    void TRACKER::Logger::ThreadSleep(int ms)
    {
        long long duration = static_cast<std::chrono::milliseconds::rep>(ms);
        std::this_thread::sleep_for(std::chrono::milliseconds(duration));
    }




    void TRACKER::Logger::InitializeLogThread(bool encrypt)
    {
        SetLogState(LogState::Safe, false);
        TrackerDispatch([this, encrypt] { InitializeLogThread_Async(encrypt); });
    }




    void TRACKER::Logger::InitializeLogThread_Async(bool encrypt)
    {
        bool success = WaitForState(LogState::Busy, false, 5000);

        if (!success)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Logger exceeded busy state limit. Aborting logthread start...");
            return;
        }

        if (IsLogging())
        {
            StopLoggingThread();
        }

        const std::string matchID = std::to_string(getMatchID());
        UpdateMatchId(matchID);
        std::filesystem::path* pFilePath = InitializeAndGetLogPath();

        try
        {

            if (!pFilePath)
            {
                Error(eDLL_T::SERVER, NO_ERROR, "Tracker: [InitializeLogThread_Async] file path nullptr \n");
                return;
            }

            if (matchID.empty())
            {
                Error(eDLL_T::SERVER, NO_ERROR, "Tracker: [InitializeLogThread_Async] matchID was empty.. failed to start logging\n");
                return;
            }

            handleNewMatch(matchID.c_str());

            if (pFilePath && !OpenLogFile(*pFilePath))
            {
                Error(eDLL_T::SERVER, NO_ERROR, "Tracker: CRITICAL ERROR: Could not open file to write (nullptr?)\n");
                StopLoggingThread();
                return;
            }
        }
        catch (const std::runtime_error& e)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Runtime Error starting logging thread: %s\n", e.what());
            StopLoggingThread();
            return;
        }
        catch (const std::exception& e)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Exception starting logging thread: %s\n", e.what());
            StopLoggingThread();
            return;
        }
        catch (...)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Unknown Exception starting logging thread.\n");
            StopLoggingThread();
            return;
        }

        std::vector<std::string> startLines;

        std::string serverName = hostname->GetString();
        std::string serverMap = g_pHostState->m_levelName;
        std::string gameType = v_Playlists_GetCurrent();
        std::string gameMode = mp_gamemode->GetString();

        startLines.push_back(CFmtStrN<128>("|#MatchID:%s", matchID.c_str()).Get());
        startLines.push_back(CFmtStrN<128>("|#Gameversion:%s", TRACKER_SERVER_V.c_str()).Get());
        startLines.push_back(CFmtStrN<128>("|#Gametype:%s", gameType.c_str()).Get());
        startLines.push_back(CFmtStrN<128>("|#Gamemode:%s", gameMode.c_str()).Get());
        startLines.push_back(CFmtStr("|#ServerName:%s", serverName.c_str()).Get());
        startLines.push_back(CFmtStrN<128>("|#ServerMAP:%s", serverMap.c_str()).Get());

        if (encrypt)
        {
            std::lock_guard<std::mutex> lock(mtx);
            for (std::string& line : startLines)
            {
                line = this->eObj.doEncrypt(line, keyHex, ivHex);
                logQueue.push(std::move(line));
            }
        }
        else
        {
            std::lock_guard<std::mutex> lock(mtx);
            for (std::string& line : startLines)
            {
                logQueue.push(std::move(line));
            }
        }

        {
            std::unique_lock<std::mutex> lock(mtx);
            cvLog.notify_one();
        }

        SetLogState(LogState::Safe, true);
    }




    //-----------------------------------------------------------------------------
    // Called by SQVM for each log event. flags: encrypt true/false
    //-----------------------------------------------------------------------------


    void TRACKER::Logger::LogEvent(const char* logString, bool encrypt)
    {
        if (!GetLogState(LogState::Safe) || finished)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Tracker: Tried to queue to log but logthread is not fully initialized. \n");
            return;
        }

        if (!logString)
        {
            Warning(eDLL_T::SERVER, "Tracker: Logstring @ logevent is: NULLPTR \n");
            return;
        }

        //without this, we would manipulate a squirrel stack object.
        std::vector<std::string> lines = splitString(logString, "\n");

        if (encrypt)
        {
            std::lock_guard<std::mutex> lock(mtx);
            for (std::string& line : lines)
            {
                line = this->eObj.doEncrypt(line, keyHex, ivHex);
                logQueue.push(std::move(line));
            }
        }
        else
        {
            std::lock_guard<std::mutex> lock(mtx);
            for (std::string& line : lines)
            {
                logQueue.push(std::move(line));
            }
        }

        {
            std::unique_lock<std::mutex> lock(mtx);
            cvLog.notify_one();
        }
    }

} // namespace TRACKER

ConCommand tracker_reload_config("tracker_reload_config", [](const CCommand& args) {TRACKER::ReloadConfig(TRACKER_CONFIG); }, "Reloads the r5r.dev tracker configuration file.", FCVAR_RELEASE);
ConCommand tracker_shutdown("tracker_shutdown", [](const CCommand& args) {Tracker_Shutdown(); }, "Shuts down the r5r.dev tracker systems.", FCVAR_RELEASE);
#endif
