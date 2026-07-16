#pragma once
#ifndef CLIENT_DLL
#ifndef TRACKER_LOGGER_H
#define TRACKER_LOGGER_H
#include <string>
#include <deque>
#include <queue>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <condition_variable>
#include <unordered_map>
#include <atomic>
#include <vector>
#include <filesystem>
#include "filesystem/ifilesystem.h"
#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <game/server/logger_websocket.h>

namespace TRACKER
{
    class Encryption
    {
    public:

        std::vector<uint8_t> hex2bytes(const std::string& hex);
        std::string bbase64Encode(std::vector<uint8_t> bytes_to_encode);
        std::string doEncrypt(const std::string& plainText, const std::vector<uint8_t>& keyBytes, const std::vector<uint8_t>& ivBytes);
        static const std::string base64_chars;
        ~Encryption() = default;
    };

    class Logger
    {
    public:

        static Logger& getInstance();
        std::atomic<bool> s_isShutdown{ false };
        void Shutdown();

        enum class LogState : uint8_t {
            None = 0,
            Ready = 1 << 0,
            Busy = 1 << 1,
            Safe = 1 << 2
        };

        LogState intToLogState(int flag);

        // Main Logging Functions
        bool GetLogState(LogState flag) const; //bitstate callable from sqvm
        void InitializeLogThread(bool encrypt); //callable from sqvm
        void LogEvent(const char* logString, bool encrypt); //callable from sqvm
        void StopLogging(bool sendToAPI); //callable from sqvm //wrapper
        bool IsLogging(); //callable from sqvm


        //utility
        std::string GetLatestFile(const std::string& directoryPath, std::string matchID);
        void StopLoggingThread();
        void StartLogging();
        void SendLogToAPI();
        void ThreadSleep(int ms);
        bool WaitForState(const LogState state, bool flag, const int timeout_in_ms);

        //file management
        void LogToFile();
        void CallClosure();
        bool OpenLogFile(const std::filesystem::path& filePath); //priv this
        void CloseLogFile(); //priv this

        std::ofstream logFile; // m_ this
        std::mutex fileMutex; // m_this

        //vars
        size_t CVAR_MAX_BUFFER = 50000; //m_this

    private:

        Logger();
        ~Logger();

        // ensures only one instance
        Logger(const Logger&) = delete;
        Logger& operator=(const Logger&) = delete;

        // pointers
        std::filesystem::path* pCurrentLogPath = nullptr;
        std::filesystem::path filePath;
        std::string currentMatchId;

        // aes encryption object
        Encryption eObj;

        std::vector<uint8_t> keyHex;
        std::vector<uint8_t> ivHex;

        //structs
        std::deque<std::string> buffer;

        // func
        void InitializeLogThread_Async(bool encrypt);
        void SetLogState(LogState flag, bool value); //bit state
        void StopLogging_Async(bool sendToAPI);
        void WriteBufferToFile(const std::deque<std::string>& q_buffer);
        void UpdateMatchId(const std::string& matchId);
        std::filesystem::path* InitializeAndGetLogPath();
        void ResetLogPath();
        void handleNewMatch(const char* matchID);
        std::vector<std::string> splitString(std::string str, const std::string& delimiter);

        // synchronization
        std::atomic<uint8_t> StateBits{ 0 };
        std::atomic<bool> finished{ true };
        std::mutex file_mtx;
        std::thread apiThread;
        std::mutex mtx;
        std::condition_variable cvLog;
        std::queue<std::string> logQueue;
        std::thread logThread;
        std::shared_mutex pathMutex;
    };

    class TaskManager
    {
    public:

        static TaskManager& getInstance();
        std::atomic<bool> s_isShutdown{ false };
        void Shutdown();

        void AddTask(const std::function<void()>& task);
        void RequestPlayerPersistenceData(std::string&& player_oid, std::vector<std::string>&& requestedStats, std::vector<std::string>&& requestedSettings);
        void ResetPlayerData(const char* player_oid);
        void RequestBatchPlayerPersistenceData( std::vector<std::string>&& player_oids, std::vector<std::string>&& requestedStats, std::vector<std::string>&& requestedSettings);

    private:
        TaskManager();
        ~TaskManager();
        TaskManager(const TaskManager&) = delete;
        TaskManager& operator=(const TaskManager&) = delete;

        void StartWorkerThread();
        void ProcessTasks();

        std::queue<std::function<void()>> taskQueue;
        std::mutex queueMutex;
        std::condition_variable condVar;
        std::thread workerThread;
        std::atomic<bool> stop_tasks_flag{ false };
    };

    class CURLConnectionPool
    {
    public:

        static CURLConnectionPool& GetInstance();
        std::atomic<bool> s_isShutdown{ false };
        void Shutdown();

        CURL* GetHandle();
        bool HandleCurlResult(CURL* handle, CURLcode res, const char* func);
        void DiscardHandle(CURL* handle);
        void ReturnHandle(CURL* handle);
        ~CURLConnectionPool();

    private:

        std::queue<CURL*> pool;
        std::mutex poolMutex;

        CURLConnectionPool();

        CURLConnectionPool(const CURLConnectionPool&) = delete;
        CURLConnectionPool& operator=(const CURLConnectionPool&) = delete;

        std::condition_variable poolCond;
        const size_t maxPoolSize = 5;
        const std::chrono::milliseconds handleWaitTimeout = std::chrono::milliseconds(5000);

        CURL* CreateHandle();
        std::atomic<bool> m_bShuttingDown{ false };
    };

    //maintenance
    void CleanupLogs(IFileSystem* pFileSystem);
    void SaveEndingMatchID();
    std::string GetEndingMatchID();


    //settings
    void AddToConfigMap(const rapidjson::Value& value, const std::string& parentKey, int depth);
    void LoadConfig(IFileSystem* pFileSystem, const char* configFileName);
    void ReloadConfig(const char* configFileName);
    std::string GetSetting(const char* key);
    int64_t GetMaxLogfileSize(const char* settingValue);

    // Functions for sendtoapi
    std::string url_encode(const std::string& value);

    //Funciton for verify
    const std::string VerifyEaAccount(const std::string& token, const std::string& OID, const std::string& ea_name);

    //Api call to player count
    void PlayerCountUpdate(std::string action, std::string player, std::string oid, std::string count, std::string DISCORD_HOOK);
    void UPDATE_PLAYER_COUNT(const char* action, const char* player, const char* OID, const char* count);

    //Api call for end game
    void EndMatchUpdate(std::string recap);
    void NOTIFY_END_OF_MATCH(const char* recap);

    //Api calls for stats
    static std::unordered_map<std::string, std::string> playerStatsMap;
    static std::shared_timed_mutex statsMutex;
    std::string FetchPlayerStats(const char* player_oid, const char* requestedStats, const char* requestedSettings); // on player connect if batch is complete only
    std::string FetchBatchPlayerStats(
        const std::vector<std::string>& player_oids,
        const std::string& requestedStats,
        const std::string& requestedSettings
    );
    std::string GetPlayerJsonData(const char* player_oid); //on startup / player connect
    void RunUpdateLiveStats(std::string stats_json); //onshutdown dispatch thread
    void UpdateLiveStats(std::string stats_json); //onshutdown
    std::string FetchGlobalSettings(const char* query);//on startup init
}

inline void Tracker_Shutdown()
{
    static std::atomic<bool> isShutdown{ false };
    if (isShutdown.exchange(true))
        return;

    TrackerSocketSystem()->Shutdown();
    TRACKER::TaskManager::getInstance().Shutdown();
    TRACKER::Logger::getInstance().Shutdown();
    TRACKER::CURLConnectionPool::GetInstance().Shutdown();
}
//-----------------------------------------------------------------------------
// CONSTANTS
//-----------------------------------------------------------------------------

const std::string TRACKER_SERVER_V = "rc_2.6.51";
const std::string TRACKER_API_KEY = "tMcLsTYqcraC7K2j"; //public
constexpr const char* TRACKER_CONFIG = "r5rdev_config.json";
constexpr const char* TRACKER_PLAYER_COUNT_ENDPOINT = "https://r5r.dev/api/playercount.php";
const std::string TRACKER_STATS_API_ENDPOINT = "https://r5r.dev/api/stats8.php";
constexpr const char* TRACKER_WS_ADDRESS = "r5r.dev";
constexpr int TRACKER_WS_PORT = 9705;

extern ConCommand tracker_reload_config;
extern ConCommand tracker_shutdown;
extern TRACKER::Logger* g_pTracker;

template< typename Fn >
inline void TrackerDispatch(Fn&& fn)
{
    TRACKER::TaskManager::getInstance().AddTask(std::forward< Fn >(fn));
}

inline void TrackerInit()
{
    TRACKER::Logger::getInstance();
    TRACKER::TaskManager::getInstance();
    TRACKER::CURLConnectionPool::GetInstance();
    g_pTracker = &TRACKER::Logger::getInstance();
}
#endif // TRACKER_LOGGER_H
#endif // !CLIENT_DLL
