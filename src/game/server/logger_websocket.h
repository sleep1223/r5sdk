#pragma once
#ifndef CLIENT_DLL
#ifndef TRACKER_LOGGER_WEBSOCKET_H
#define TRACKER_LOGGER_WEBSOCKET_H

#include <string>
#include <queue>
#include <memory>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <functional>
#include <vector>
#include <rapidjson/document.h>
#include <filesystem>
#include "filesystem/ifilesystem.h"
#include "tier1/cvar.h"
#include "networksystem/bansystem.h"

//forward declarations
class CWebSocket;

namespace TRACKER
{
    //===========================================================================
    // WebSocketCommandHandler - Handles WebSocket server control commands
    //===========================================================================

    class WebSocketCommandHandler
    {
    public:
        // Singleton access
        static WebSocketCommandHandler& getInstance(); //use TrackerSocketSystem()

        // ===== Connection Management =====
        bool InstallCaBundleFromPlatform(const char* pPlatformFile);
        bool IsInitialized();
        bool Connect(const char* trackerHostname, int port);
        void Reconnect();
        void Disconnect();
        bool IsConnected() const;
        void RunFrame();
        void Update();  // Call from main update loop
        void Status();
        void Shutdown();

        // ===== Message Reception =====
        void OnMessageReceived(const std::string& rawMessage);
        void ProcessMessageQueue();

        // ===== Message Transmission =====
        void SendResponse(const std::string& requestId, const char* status, const rapidjson::Value* data, const std::string& message, const char* type = "");
        void RelayChatMessage(unsigned __int64 senderNucleus, const char* name, const char* message);
        void RelayChatMute(const char* const pszPlayerName, NucleusID_t nucleusId, const char* const pszReason, const char* const pszExpiry, const char* const mutedBy, bool toggle, int expiryUnixTimestamp);

        // ===== Command Handlers =====
        void HandleHandshakeCommand(const rapidjson::Document& doc, const std::string& requestId);
        void HandleKickCommand(const rapidjson::Value& params, const std::string& requestId);
        void HandleBanCommand(const rapidjson::Value& params, const std::string& requestId);
        void HandleUnbanCommand(const rapidjson::Value& params, const std::string& requestId);
        void HandleGetBanlistCommand(const std::string& requestId);
        void HandleGetPlayersCommand(const std::string& requestId);
        void HandleGetConfigCommand(const rapidjson::Value& params, const std::string& requestId);
        void HandleGetStatsCommand(const std::string& requestId);
        void HandleReloadConfigCommand(const std::string& requestId);
        void HandleUpdateConfigCommand(const rapidjson::Value& params, const std::string& requestId);
        void HandleReloadBanlistCommand(const std::string& requestId);
        void HandleAddBanCommand(const rapidjson::Value& params, const std::string& requestId);
        void HandleReloadServerCommand(const std::string& requestId);
        void HandleToggleMute(const rapidjson::Value& params, const std::string& requestId);
        void HandleSendMessage(const rapidjson::Value& params, const std::string& requestId);
        void HandleTimeoutCommand(const rapidjson::Value& params, const std::string& requestId);


        // ===== Validation & Utilities =====
        bool ValidateMessage(const rapidjson::Document& doc, std::string& outError);
        bool AuthenticateMessage(const rapidjson::Document& doc, std::string& outError);

        // ===== Configuration =====
        void OnWebSocketConVarChanged(IConVar* var, const char* pOldValue, float flOldValue, const char* newValue);
        void __CheckInstallCA();

    private:
        // Private constructor (singleton)
        WebSocketCommandHandler();
        ~WebSocketCommandHandler();

        // Config
        void ApplyConVars();

        // Delete copy/move constructors
        WebSocketCommandHandler(const WebSocketCommandHandler&) = delete;
        WebSocketCommandHandler& operator=(const WebSocketCommandHandler&) = delete;
        WebSocketCommandHandler(WebSocketCommandHandler&&) = delete;
        WebSocketCommandHandler& operator=(WebSocketCommandHandler&&) = delete;

        // Memory Allocation for performance
        void AllocateAddress(const char* connectionAddress);
        void FreeAddress();

        // ===== Message Queue Structures =====
        struct PendingMessage_t
        {
            std::string id;
            std::string type;
            rapidjson::Document doc;
            double receivedTime;
            int retryCount;
        };

        // Command type enumeration for switch dispatch
        enum class CommandType_e
        {
            UNKNOWN = -1,
            KICK_PLAYER = 0,
            BAN_PLAYER = 1,
            UNBAN_PLAYER = 2,
            GET_BANLIST = 3,
            GET_PLAYERS = 4,
            GET_CONFIG = 5,
            GET_STATS = 6,
            RELOAD_CONFIG = 7,
            UPDATE_CONFIG = 8,
            RELOAD_BANLIST = 9,
            ADD_BAN = 10,
            RELOAD_SERVER = 11,
            HANDSHAKE = 12,
            TOGGLE_PLAYER_MUTE = 13,
            SEND_MESSAGE = 14,
            TOGGLE_TIMEOUT = 15
        };

        // ===== Internal Helpers =====
        CommandType_e GetCommandType(const std::string& typeStr);
        void DispatchCommand(const rapidjson::Document& doc, const std::string& requestId);

        // ===== Utility =====
        int32_t ClampBuffer(int32_t bufSize);
        float ClampThrottleRate(float throttleValue);

        // ===== Data Members =====
        std::unique_ptr<CWebSocket> m_webSocket;
        std::queue<PendingMessage_t> m_messageQueue;
        std::queue<std::string> m_responseQueue;
        std::shared_timed_mutex m_queueMutex;

        std::string m_serverHostname;
        char* m_connectedAddress = nullptr; //manually allocated for performance.
        int m_serverPort;
        std::atomic<bool> m_isConnected;
        std::atomic<bool> m_initialized;
        std::atomic<bool> m_authorized;
        std::atomic<uint64_t> m_messageCount{ 0 };


        double m_lastUpdateTime;
        double m_lastConnectAttempt;
        float m_throttleRate;
        bool m_forceLaxSSL;

        // Configuration caching
        std::string m_cachedApiKey;
        std::string m_cachedIdentifier;
        std::atomic<bool> m_configDirty{ false };
        std::atomic<bool> m_pendingReconnect{ false };
        std::atomic<bool> m_loadedCaBundle{ false };
        std::vector<char> m_receiveBuffer;
    };
}//namespace TRACKER

extern ConVar tracker_ws_enable;
extern ConVar tracker_ws_port;
extern ConVar tracker_ws_debug;
extern ConVar tracker_ws_use_ssl;
extern ConVar tracker_ws_lax_ssl;
extern ConVar tracker_ws_buffer_size;
extern ConVar tracker_ws_max_retries;
extern ConVar tracker_ws_retry_time;
extern ConVar tracker_ws_time_out;
extern ConVar tracker_ws_keep_alive;
extern ConVar tracker_ws_throttle_rate;
extern ConVar tracker_ws_hostname;
extern ConVar tracker_ws_tls_version;
extern ConVar tracker_ws_relay_chat;
extern ConVar tracker_ws_reconnect_on_change;
extern ConVar tracker_ws_ca_bundle_file;
extern ConVar tracker_ws_reconnect_on_newgame;

extern ConCommand tracker_ws_reconnect;
extern ConCommand tracker_ws_shutdown;
extern ConCommand tracker_ws_status;

TRACKER::WebSocketCommandHandler* TrackerSocketSystem();

#endif // TRACKER_LOGGER_WEBSOCKET_H
#endif // !CLIENT_DLL
