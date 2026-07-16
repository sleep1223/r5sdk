#pragma once
#ifndef CLIENT_DLL
#include "core/stdafx.h"
#include "logger_websocket.h"
#include "logger.h"
#include "networksystem/bansystem.h"
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include "engine/server/server.h"
#include "engine/client/client.h"
#include "tier1/fmtstr.h"
#include "tier0/dbg.h"
#include "tier2/websocket.h"
#include <game/server/vscript_server.h>
#include "vscript/vscript.h"
#include "engine/host.h"

//CONSTS: /thirdparty/dirtysdk/include/DirtySDK/proto/protossl.h
constexpr int PROTOSSL_VERSION_TLS1_0 = (0x0301);
constexpr int PROTOSSL_VERSION_TLS1_1 = (0x0302);
constexpr int PROTOSSL_VERSION_TLS1_2 = (0x0303);
constexpr int PROTOSSL_VERSION_TLS1_3 = (0x0304);

namespace TRACKER
{
    //===========================================================================
    // Singleton Instance
    //===========================================================================

    WebSocketCommandHandler& WebSocketCommandHandler::getInstance()
    {
        static WebSocketCommandHandler instance;
        return instance;
    }

    //===========================================================================
    // Constructor / Destructor
    //===========================================================================

    WebSocketCommandHandler::WebSocketCommandHandler()
        : m_webSocket(nullptr)
        , m_connectedAddress(nullptr)
        , m_serverPort(0)
        , m_isConnected(false)
        , m_initialized(false)
        , m_authorized(false)
        , m_messageCount(0)
        , m_lastUpdateTime(0.0)
        , m_lastConnectAttempt(0.0)
        , m_throttleRate(0.10f)
        , m_forceLaxSSL(false)
        , m_cachedApiKey()
        , m_cachedIdentifier()
        , m_configDirty(false)
        , m_pendingReconnect(false)
        , m_loadedCaBundle(false)
        , m_receiveBuffer()
    {
    }

    WebSocketCommandHandler::~WebSocketCommandHandler()
    {
        Shutdown();
    }

    //===========================================================================
    // Connection Management
    //===========================================================================

    bool WebSocketCommandHandler::Connect(const char* trackerHostname, int port)
    {
        if (!tracker_ws_enable.GetBool())
        {
            Error(eDLL_T::SERVER, NO_ERROR, "TrackerSocket: Tried to connect to tracker websocket but tracker_ws_enable is 0/false");
            return false;
        }

        if (!trackerHostname || trackerHostname[0] == '\0')
        {
            Error(eDLL_T::SERVER, NO_ERROR, "TrackerSocket: Invalid address provided\n");
            return false;
        }

        if (tracker_ws_use_ssl.GetBool() && !m_loadedCaBundle.load())
            __CheckInstallCA();

        std::string configRelayChatValue = GetSetting("server.RELAY_CHAT_MESSAGES");
        if (!configRelayChatValue.empty())
            tracker_ws_relay_chat.SetValue(configRelayChatValue == "true" ? "1" : "0");

        m_serverHostname = trackerHostname;
        m_serverPort = port;
        m_throttleRate = ClampThrottleRate(tracker_ws_throttle_rate.GetFloat());
        m_cachedApiKey = GetSetting("apikey");
        m_cachedIdentifier = GetSetting("identifier");

        int32_t bufSize = tracker_ws_buffer_size.GetInt();

        //clamp
        bufSize = ClampBuffer(bufSize);

        tracker_ws_buffer_size.SetValue(bufSize);
        m_receiveBuffer.resize(static_cast<size_t>(bufSize));

        if (!m_initialized.load())
        {
            m_webSocket = std::make_unique<CWebSocket>();

            if (m_forceLaxSSL)
                tracker_ws_lax_ssl.SetValue("1");

            bool useSSL = tracker_ws_use_ssl.GetBool();

            CWebSocket::ConnParams_s params;
            params.bufSize = tracker_ws_buffer_size.GetInt();
            params.retryTime = tracker_ws_retry_time.GetFloat();
            params.maxRetries = tracker_ws_max_retries.GetInt();
            params.timeOut = tracker_ws_time_out.GetInt();
            params.keepAlive = tracker_ws_keep_alive.GetInt();
            params.laxSSL = !GetSetting("server.LAX_SSL").empty() ? static_cast<int32_t>(GetSetting("server.LAX_SSL") == "true") : tracker_ws_lax_ssl.GetInt();
            params.useTls = useSSL;

            tracker_ws_lax_ssl.SetValue(params.laxSSL);

            int32_t tlsVersion = tracker_ws_tls_version.GetInt();
            int32_t protocolVersion = 3;

            if (tlsVersion < 0)
                tlsVersion = 0;
            if (tlsVersion > 3)
                tlsVersion = 3;

            if (tlsVersion == 0)
                protocolVersion = PROTOSSL_VERSION_TLS1_0;
            else if (tlsVersion == 1)
                protocolVersion = PROTOSSL_VERSION_TLS1_1;
            else if (tlsVersion == 2)
                protocolVersion = PROTOSSL_VERSION_TLS1_2;
            else if (tlsVersion >= 3)
                protocolVersion = PROTOSSL_VERSION_TLS1_3;

            params.protocol = useSSL ? protocolVersion : 0;

            std::string addressStr;
            if (useSSL)
                addressStr = CFmtStr("wss://%s:%d", trackerHostname, port).Get();
            else
                addressStr = CFmtStr("ws://%s:%d", trackerHostname, port).Get();

            AllocateAddress(addressStr.c_str());

            const char* errorMsg = nullptr;
            if (!m_webSocket->Init(addressStr.c_str(), params, errorMsg))
            {
                Error(eDLL_T::SERVER, NO_ERROR, "TrackerSocket: Failed to initialize: %s\n", errorMsg ? errorMsg : "Unknown error");
                m_webSocket.reset();
                FreeAddress();
                return false;
            }

            m_initialized.store(true);
            Msg(eDLL_T::SERVER, "TrackerSocket: Initialized (hostname=%s, port=%d)\n", trackerHostname, port);
        }

        m_lastConnectAttempt = Plat_FloatTime();
        m_isConnected.store(false);

        if (tracker_ws_debug.GetBool())
            Msg(eDLL_T::SERVER, "TrackerSocket[DEBUG]: Connection attempt to %s:%d\n", trackerHostname, port);

        return true;
    }

    void WebSocketCommandHandler::Disconnect()
    {
        if (m_webSocket && m_connectedAddress && m_webSocket->Disconnect(m_connectedAddress))
            Msg(eDLL_T::SERVER, "TrackerSocket: Disconnected\n");

        if (tracker_ws_debug.GetBool())
            Msg(eDLL_T::SERVER, "TrackerSocket[DEBUG]: Setting states to false (m_isConnected|m_initialized|m_authorized)\n");

        m_isConnected.store(false);
        m_initialized.store(false);
        m_authorized.store(false);

        std::lock_guard<std::shared_timed_mutex> lock(m_queueMutex);
        std::queue<PendingMessage_t>().swap(m_messageQueue);
        std::queue<std::string>().swap(m_responseQueue);
    }

    bool WebSocketCommandHandler::IsConnected() const
    {
        return m_isConnected.load();
    }

    void WebSocketCommandHandler::Update()
    {
        if (!m_initialized.load() || !m_webSocket)
            return;

        m_webSocket->Update();

        bool nowListening = m_webSocket->IsListening(m_connectedAddress);
        bool wasConnected = m_isConnected.load();

        if (nowListening && !wasConnected)
        {
            m_isConnected.store(true);
            m_lastConnectAttempt = 0.0;
            Msg(eDLL_T::SERVER, "TrackerSocket: Successfully connected to %s:%d\n", m_serverHostname.c_str(), m_serverPort);
        }
        else if (!nowListening && wasConnected && !m_webSocket->IsActive(m_connectedAddress))
        {
            m_isConnected.store(false);
            m_authorized.store(false);
            Error(eDLL_T::SERVER, NO_ERROR, "TrackerSocket: Lost connection to %s:%d\n", m_serverHostname.c_str(), m_serverPort);
        }

        double timeSinceAttempt = Plat_FloatTime() - m_lastConnectAttempt;
        if (timeSinceAttempt > 15.0 && !m_webSocket->IsActive(m_connectedAddress) && m_lastConnectAttempt > 0.0)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "TrackerSocket: Failed to connect to %s:%d after 15 seconds\n", m_serverHostname.c_str(), m_serverPort);
            m_lastConnectAttempt = 0.0;
        }

        int32_t received = m_webSocket->ReceiveData
        (
            m_receiveBuffer.data(),
            static_cast<int32_t>(m_receiveBuffer.size())
        );

        if (received > 0)
        {
            std::string incomingMsg(m_receiveBuffer.data(), received);
            OnMessageReceived(incomingMsg);
        }

        if (m_webSocket->IsListening(m_connectedAddress))
        {
            std::lock_guard<std::shared_timed_mutex> lock(m_queueMutex);
            while (!m_responseQueue.empty())
            {
                const std::string& response = m_responseQueue.front();
                m_webSocket->SendData(response.c_str(), static_cast<int32_t>(response.length()));
                m_responseQueue.pop();
            }
        }

        if (m_webSocket->GetState(m_connectedAddress) == CWebSocket::CS_DESTROYED)
        {
            Msg(eDLL_T::SERVER, "TrackerSocket: Connection dropped. \n");
            Shutdown();
            return;
        }

        m_lastUpdateTime = Plat_FloatTime();
    }

    void WebSocketCommandHandler::Shutdown()
    {
        Disconnect();
        if (m_webSocket)
        {
            m_webSocket->Shutdown();
            m_webSocket.reset();
        }
        FreeAddress();
        m_initialized.store(false);
    }

    //===========================================================================
    // Message Reception
    //===========================================================================

    void WebSocketCommandHandler::OnMessageReceived(const std::string& rawMessage)
    {
        m_messageCount++;

        if (rawMessage.empty())
        {
            Error(eDLL_T::SERVER, NO_ERROR, "TrackerSocket: Received empty message\n");
            return;
        }

        TrackerDispatch
        (
            [this, rawMessage]
            {
                rapidjson::Document doc;
                doc.Parse(rawMessage.c_str());

                if (doc.HasParseError())
                {
                    Error(eDLL_T::SERVER, NO_ERROR, "TrackerSocket: Invalid JSON received (offset %zu): %s\n", doc.GetErrorOffset(), rapidjson::GetParseError_En(doc.GetParseError()));
                    return;
                }

                std::string validationError;

                if (!ValidateMessage(doc, validationError))
                {
                    Error(eDLL_T::SERVER, NO_ERROR, "TrackerSocket: Message validation failed: %s\n", validationError.c_str());
                    return;
                }

                if (!AuthenticateMessage(doc, validationError))
                {
                    const char* msgId = doc.HasMember("id") && doc["id"].IsString() ? doc["id"].GetString() : "unknown";

                    SendResponse(msgId, "error", nullptr, "Authentication failed");

                    Error(eDLL_T::SERVER, NO_ERROR, "TrackerSocket: Incoming message authentication failed: %s\n", validationError.c_str());
                    return;
                }

                PendingMessage_t msg;
                msg.id = doc["id"].GetString();
                msg.type = doc["type"].GetString();
                msg.receivedTime = Plat_FloatTime();
                msg.retryCount = 0;
                msg.doc = std::move(doc);

                if (tracker_ws_debug.GetBool())
                    Msg(eDLL_T::SERVER, "TrackerSocket[DEBUG]: Message queued (type=%s, id=%s)\n", msg.type.c_str(), msg.id.c_str());

                {
                    std::lock_guard<std::shared_timed_mutex> lock(m_queueMutex);
                    m_messageQueue.push(std::move(msg));
                }
            }
        );
    }


    void WebSocketCommandHandler::ProcessMessageQueue()
    {
        std::queue<PendingMessage_t> localQueue;

        {
            std::lock_guard<std::shared_timed_mutex> lock(m_queueMutex);
            localQueue.swap(m_messageQueue);
        }

        while (!localQueue.empty())
        {
            PendingMessage_t msg = std::move(localQueue.front());
            localQueue.pop();

            DispatchCommand(msg.doc, msg.id);
        }
    }


    //===========================================================================
    // Command Dispatching
    //===========================================================================

    WebSocketCommandHandler::CommandType_e WebSocketCommandHandler::GetCommandType(
        const std::string& typeStr)
    {
        // Todo: Use static hash map for O(1) lookup (if needed for many commands)
        if (typeStr == "kick_player")
            return CommandType_e::KICK_PLAYER;
        else if (typeStr == "ban_player")
            return CommandType_e::BAN_PLAYER;
        else if (typeStr == "unban_player")
            return CommandType_e::UNBAN_PLAYER;
        else if (typeStr == "get_banlist")
            return CommandType_e::GET_BANLIST;
        else if (typeStr == "get_players")
            return CommandType_e::GET_PLAYERS;
        else if (typeStr == "get_config")
            return CommandType_e::GET_CONFIG;
        else if (typeStr == "get_stats")
            return CommandType_e::GET_STATS;
        else if (typeStr == "reload_config")
            return CommandType_e::RELOAD_CONFIG;
        else if (typeStr == "update_config")
            return CommandType_e::UPDATE_CONFIG;
        else if (typeStr == "reload_banlist")
            return CommandType_e::RELOAD_BANLIST;
        else if (typeStr == "add_ban")
            return CommandType_e::ADD_BAN;
        else if (typeStr == "reload_server")
            return CommandType_e::RELOAD_SERVER;
        else if (typeStr == "handshake")
            return CommandType_e::HANDSHAKE;
        else if (typeStr == "toggle_mute")
            return CommandType_e::TOGGLE_PLAYER_MUTE;
        else if (typeStr == "send_message")
            return CommandType_e::SEND_MESSAGE;
        else if (typeStr == "timeout_player")
            return CommandType_e::TOGGLE_TIMEOUT;
        else

            return CommandType_e::UNKNOWN;
    }

    void WebSocketCommandHandler::DispatchCommand(const rapidjson::Document& doc, const std::string& requestId)
    {
        const std::string& typeStr = doc["type"].GetString();
        const auto& params = doc["params"];

        CommandType_e cmdType = GetCommandType(typeStr);

        switch (cmdType)
        {
        case CommandType_e::HANDSHAKE:
            HandleHandshakeCommand(doc, requestId);
            break;

        case CommandType_e::KICK_PLAYER:
            HandleKickCommand(params, requestId);
            break;

        case CommandType_e::BAN_PLAYER:
            HandleBanCommand(params, requestId);
            break;

        case CommandType_e::UNBAN_PLAYER:
            HandleUnbanCommand(params, requestId);
            break;

        case CommandType_e::GET_BANLIST:
            HandleGetBanlistCommand(requestId);
            break;

        case CommandType_e::GET_PLAYERS:
            HandleGetPlayersCommand(requestId);
            break;

        case CommandType_e::GET_CONFIG:
            HandleGetConfigCommand(params, requestId);
            break;

        case CommandType_e::GET_STATS:
            HandleGetStatsCommand(requestId);
            break;

        case CommandType_e::RELOAD_CONFIG:
            HandleReloadConfigCommand(requestId);
            break;

        case CommandType_e::UPDATE_CONFIG:
            HandleUpdateConfigCommand(params, requestId);
            break;

        case CommandType_e::RELOAD_BANLIST:
            HandleReloadBanlistCommand(requestId);
            break;

        case CommandType_e::ADD_BAN:
            HandleAddBanCommand(params, requestId);
            break;

        case CommandType_e::RELOAD_SERVER:
            HandleReloadServerCommand(requestId);
            break;

        case CommandType_e::TOGGLE_PLAYER_MUTE:
            HandleToggleMute(params, requestId);
            break;

        case CommandType_e::SEND_MESSAGE:
            HandleSendMessage(params, requestId);
            break;

        case CommandType_e::TOGGLE_TIMEOUT:
            HandleTimeoutCommand(params, requestId);
            break;

        case CommandType_e::UNKNOWN:
        default:
            SendResponse(requestId, "error", nullptr, "Unknown command type");
            Error(eDLL_T::SERVER, NO_ERROR, "TrackerSocket: Unknown command type: %s\n", typeStr.c_str());
            break;
        }
    }

    //===========================================================================
    // Command Handlers
    //===========================================================================

    void WebSocketCommandHandler::HandleKickCommand(const rapidjson::Value& params, const std::string& requestId)
    {
        if (!g_pServer || !g_pServer->IsActive())
        {
            SendResponse(requestId, "error", nullptr, "Game is not running");
            return;
        }

        if (!params.HasMember("player_criteria") || !params["player_criteria"].IsString())
        {
            SendResponse(requestId, "error", nullptr, "Missing required field: player_criteria");
            return;
        }

        std::string playerCriteria = params["player_criteria"].GetString();
        std::string reason = (params.HasMember("reason") && params["reason"].IsString()) ?
            params["reason"].GetString() :
            "";

        if (!g_BanSystem.IsPlayerInServer(playerCriteria.c_str()))
        {
            SendResponse(requestId, "error", nullptr, "Player is not in server.");
            return;
        }

        const char* reasonPtr = reason.empty() ? nullptr : reason.c_str();

        if (V_IsAllDigit(playerCriteria.c_str()))
            g_BanSystem.KickPlayerById(playerCriteria.c_str(), reasonPtr);
        else
            g_BanSystem.KickPlayerByName(playerCriteria.c_str(), reasonPtr);

        rapidjson::Document response;
        response.SetObject();
        auto& alloc = response.GetAllocator();

        rapidjson::Value data(rapidjson::kObjectType);
        data.AddMember("player_kicked", true, alloc);
        data.AddMember("criteria", rapidjson::Value(playerCriteria.c_str(), alloc), alloc);

        SendResponse(requestId, "success", &data, "Player kicked successfully");

        if (tracker_ws_debug.GetBool())
            Msg(eDLL_T::SERVER, "TrackerSocket[DEBUG]: Kick command queued for %s\n", playerCriteria.c_str());
    }

    void WebSocketCommandHandler::HandleBanCommand(const rapidjson::Value& params, const std::string& requestId)
    {
        if (!g_pServer || !g_pServer->IsActive())
        {
            SendResponse(requestId, "error", nullptr, "Game is not running");
            return;
        }

        if (!params.HasMember("player_criteria") || !params["player_criteria"].IsString())
        {
            SendResponse(requestId, "error", nullptr, "Missing required field: player_name");
            return;
        }

        const char* bannedByIdOrName = nullptr;
        if (params.HasMember("bannedByIdOrName") && params["bannedByIdOrName"].IsString())
            bannedByIdOrName = params["bannedByIdOrName"].GetString();

        std::string playerNameOrUID = params["player_criteria"].GetString();
        std::string reason = (params.HasMember("reason") && params["reason"].IsString()) ?
            params["reason"].GetString() :
            "";


        if (!g_BanSystem.IsPlayerInServer(playerNameOrUID.c_str()))
        {
            SendResponse(requestId, "error", nullptr, "Player is not in server.");
            return;
        }

        if (V_IsAllDigit(playerNameOrUID.c_str()))
            g_BanSystem.BanPlayerById(playerNameOrUID.c_str(), bannedByIdOrName ? bannedByIdOrName : nullptr, reason.empty() ? nullptr : reason.c_str());
        else
            g_BanSystem.BanPlayerByName(playerNameOrUID.c_str(), bannedByIdOrName ? bannedByIdOrName : nullptr, reason.empty() ? nullptr : reason.c_str());

        char context[256] = {};
        bool bStatus = g_BanSystem.IsBannedInMetaData(playerNameOrUID.c_str(), nullptr, context); //check if actually banned successfully

        rapidjson::Document response;
        response.SetObject();
        auto& alloc = response.GetAllocator();

        rapidjson::Value data(rapidjson::kObjectType);
        data.AddMember("player_banned", bStatus, alloc);
        data.AddMember("player_criteria", rapidjson::Value(playerNameOrUID.c_str(), alloc), alloc);
        data.AddMember("context", rapidjson::Value(context, alloc), alloc);

        SendResponse(requestId, bStatus ? "success" : "failed", &data, bStatus ? "Player banned successfully" : "Player was unavailable in server to ban. Did you mean to use AddBan instead?");
        Msg(eDLL_T::SERVER, bStatus ? "TrackerSocket: Player %s banned\n" : "TrackerSocket: Player %s was unavailable to ban\n", playerNameOrUID.c_str());

        if (tracker_ws_debug.GetBool())
            Msg(eDLL_T::SERVER, "TrackerSocket[DEBUG]: Ban command queued for %s\n", playerNameOrUID.c_str());
    }

    void WebSocketCommandHandler::HandleUnbanCommand(const rapidjson::Value& params, const std::string& requestId)
    {
        if (!params.HasMember("player_criteria") || !params["player_criteria"].IsString())
        {
            SendResponse(requestId, "error", nullptr, "Missing/invalid required field: player_criteria");
            return;
        }

        std::string criteria = params["player_criteria"].GetString();

        bool bWasBanned = g_BanSystem.IsBannedInMetaData(criteria.c_str(), nullptr, nullptr);
        if (bWasBanned)
            g_BanSystem.UnbanPlayer(criteria.c_str());

        rapidjson::Document response;
        response.SetObject();
        auto& alloc = response.GetAllocator();

        rapidjson::Value data(rapidjson::kObjectType);
        data.AddMember("player_unbanned", bWasBanned, alloc);
        data.AddMember("criteria", rapidjson::Value(criteria.c_str(), alloc), alloc);

        SendResponse(requestId, bWasBanned ? "success" : "failed", &data, bWasBanned ? "Player unbanned successfully" : "Player was not banned to be unbanned.");
        Msg(eDLL_T::SERVER, bWasBanned ? "TrackerSocket: Player '%s' unbanned\n" : "TrackerSocket: Player '%s' was not unbanned, because they were not banned.", criteria.c_str());

        if (tracker_ws_debug.GetBool())
            Msg(eDLL_T::SERVER, "TrackerSocket[DEBUG]: Unban command queued for %s\n", criteria.c_str());
    }

    void WebSocketCommandHandler::HandleGetBanlistCommand(const std::string& requestId)
    {
        TrackerDispatch
        (
            [this, requestId]() //don't process heavy operation on server thread
            {
                try
                {
                    FileHandle_t pFile = FileSystem()->Open("banlist.json", "rb", "PLATFORM");
                    if (!pFile)
                    {
                        // no banlist..
                        rapidjson::Document response;
                        response.SetObject();
                        rapidjson::Value entries(rapidjson::kArrayType);
                        SendResponse(requestId, "success", &entries, "Banlist retrieved (empty)");
                        return;
                    }

                    const ssize_t nFileSize = FileSystem()->Size(pFile);
                    if (nFileSize <= 0)
                    {
                        FileSystem()->Close(pFile);
                        rapidjson::Document response;
                        response.SetObject();
                        rapidjson::Value entries(rapidjson::kArrayType);
                        SendResponse(requestId, "success", &entries, "Banlist retrieved (empty)");
                        return;
                    }

                    const u64 nBufSize = FileSystem()->GetOptimalReadSize(pFile, nFileSize + 2);
                    char* const pBuf = (char*)FileSystem()->AllocOptimalReadBuffer(pFile, nBufSize, 0);

                    if (!pBuf)
                    {
                        FileSystem()->Close(pFile);
                        SendResponse(requestId, "error", nullptr, "Failed to allocate ban list buffer");
                        return;
                    }

                    const ssize_t nRead = FileSystem()->ReadEx(pBuf, nBufSize, nFileSize, pFile);
                    FileSystem()->Close(pFile);

                    if (nRead <= 0)
                    {
                        FileSystem()->FreeOptimalReadBuffer(pBuf);
                        rapidjson::Document response;
                        response.SetObject();
                        rapidjson::Value entries(rapidjson::kArrayType);
                        SendResponse(requestId, "success", &entries, "Banlist retrieved (empty)");
                        return;
                    }

                    pBuf[nRead] = '\0';

                    rapidjson::Document banlistDoc;
                    banlistDoc.Parse(pBuf, nRead);
                    FileSystem()->FreeOptimalReadBuffer(pBuf);

                    if (banlistDoc.HasParseError())
                    {
                        Error(eDLL_T::SERVER, NO_ERROR, "TrackerSocket: Failed to parse banlist.json at offset %zu\n", banlistDoc.GetErrorOffset());
                        SendResponse(requestId, "error", nullptr, "Failed to parse ban list file");
                        return;
                    }

                    rapidjson::Document response;
                    response.SetObject();
                    auto& alloc = response.GetAllocator();
                    rapidjson::Value entries(rapidjson::kArrayType);

                    if (banlistDoc.IsObject() && banlistDoc.HasMember("entries") && banlistDoc["entries"].IsArray())
                    {
                        for (const auto& entry : banlistDoc["entries"].GetArray())
                        {
                            if (entry.IsObject())
                            {
                                rapidjson::Value newEntry(rapidjson::kObjectType);

                                if (entry.HasMember("nucleusId") && entry["nucleusId"].IsUint64())
                                    newEntry.AddMember("nucleusId", entry["nucleusId"].GetUint64(), alloc);

                                if (entry.HasMember("playerName") && entry["playerName"].IsString())
                                    newEntry.AddMember("playerName", rapidjson::Value(entry["playerName"].GetString(), alloc), alloc);

                                if (entry.HasMember("ipAddress") && entry["ipAddress"].IsString())
                                    newEntry.AddMember("ipAddress", rapidjson::Value(entry["ipAddress"].GetString(), alloc), alloc);

                                if (entry.HasMember("banReason") && entry["banReason"].IsString())
                                    newEntry.AddMember("banReason", rapidjson::Value(entry["banReason"].GetString(), alloc), alloc);

                                if (entry.HasMember("banTimestamp") && entry["banTimestamp"].IsInt64())
                                    newEntry.AddMember("banTimestamp", entry["banTimestamp"].GetInt64(), alloc);

                                if (entry.HasMember("bannedByID") && entry["bannedByID"].IsString())
                                    newEntry.AddMember("bannedByID", rapidjson::Value(entry["bannedByID"].GetString(), alloc), alloc);

                                if (entry.HasMember("banExpiryTimestamp") && entry["banExpiryTimestamp"].IsInt64())
                                    newEntry.AddMember("banExpiryTimestamp", entry["banExpiryTimestamp"].GetInt64(), alloc);

                                if (entry.HasMember("banType") && entry["banType"].IsInt())
                                    newEntry.AddMember("banType", entry["banType"].GetInt(), alloc);

                                entries.PushBack(newEntry, alloc);
                            }
                        }
                    }

                    SendResponse(requestId, "success", &entries,
                        CFmtStr("Banlist retrieved (%zu bans)", entries.Size()).Get());

                    Msg(eDLL_T::SERVER, "TrackerSocket: Banlist sent with %zu entries\n", entries.Size());
                }
                catch (const std::exception& e)
                {
                    SendResponse(requestId, "error", nullptr, std::string("Failed to get banlist: ") + e.what());
                    Error(eDLL_T::SERVER, NO_ERROR, "TrackerSocket: Exception in HandleGetBanlistCommand: %s\n", e.what());
                }
            }
        );

        if (tracker_ws_debug.GetBool())
            Msg(eDLL_T::SERVER, "TrackerSocket[DEBUG]: Get banlist command queued\n");
    }

    void WebSocketCommandHandler::HandleGetPlayersCommand(const std::string& requestId)
    {
        if (!g_pServer || !g_pServer->IsActive())
        {
            SendResponse(requestId, "error", nullptr, "Game is not running");
            return;
        }

        rapidjson::Document response;
        response.SetObject();
        auto& alloc = response.GetAllocator();

        rapidjson::Value players(rapidjson::kArrayType);

        // Get active player list from server
        if (!g_pServer)
        {
            SendResponse(requestId, "success", &players, "No server running");
            return;
        }

        const int nMaxClients = g_pServer->GetMaxClients();

        // Iterate through all client slots
        for (int i = 0; i < nMaxClients; i++)
        {
            CClient* const pClient = g_pServer->GetClient(i);
            if (!pClient)
                continue;

            // Only include connected/spawned players
            if (!pClient->IsConnected())
                continue;

            const CNetChan* const pNetChan = pClient->GetNetChan();
            if (!pNetChan)
                continue;

            // Create player entry
            rapidjson::Value playerEntry(rapidjson::kObjectType);

            // Add player information
            playerEntry.AddMember("user_id", pClient->GetUserID(), alloc);
            playerEntry.AddMember("nucleus_id", pClient->GetNucleusID(), alloc);
            playerEntry.AddMember("handle", pClient->GetHandle(), alloc);
            playerEntry.AddMember("team", pClient->GetTeamNum(), alloc);
            playerEntry.AddMember("player_name", rapidjson::Value(pNetChan->GetName(), alloc), alloc);
            playerEntry.AddMember("is_bot", pClient->IsFakeClient(), alloc);
            playerEntry.AddMember("is_active", pClient->IsActive(), alloc);
            playerEntry.AddMember("is_spawned", pClient->IsSpawned(), alloc);

            // Add network address (if available)
            const netadr_t& remoteAddr = pNetChan->GetRemoteAddress();
            char adrStr[64];
            remoteAddr.ToString(adrStr, sizeof(adrStr));
            playerEntry.AddMember("address", rapidjson::Value(adrStr, alloc), alloc);

            // Add signon state
            int nSignonState = static_cast<int>(pClient->GetSignonState());
            playerEntry.AddMember("signon_state", nSignonState, alloc);

            players.PushBack(playerEntry, alloc);
        }

        SendResponse(requestId, "success", &players, CFmtStr("Player list retrieved (%zu players)", players.Size()).Get());

        Msg(eDLL_T::SERVER, "TrackerSocket: Player list sent with %zu players\n", players.Size());

        if (tracker_ws_debug.GetBool())
            Msg(eDLL_T::SERVER, "TrackerSocket[DEBUG]: Get players command queued\n");
    }

    void WebSocketCommandHandler::HandleGetConfigCommand(const rapidjson::Value& params, const std::string& requestId)
    {
        std::vector<std::string> keys;

        if (params.HasMember("keys") && params["keys"].IsArray())
        {
            for (const auto& key : params["keys"].GetArray())
            {
                if (key.IsString())
                    keys.emplace_back(key.GetString());
            }
        }

        Msg(eDLL_T::SERVER, "TrackerSocket: Config request with %zu keys\n", keys.size());

        TrackerDispatch
        (
            [this, keys = std::move(keys) , requestId]()
            {
                rapidjson::Document response;
                response.SetObject();
                auto& alloc = response.GetAllocator();

                rapidjson::Value config(rapidjson::kObjectType);

                for (const auto& key : keys)
                {
                    std::string value = GetSetting(key.c_str());
                    config.AddMember(rapidjson::Value(key.c_str(), alloc), rapidjson::Value(value.c_str(), alloc), alloc);
                }

                SendResponse(requestId, "success", &config, "Config values retrieved");
            }
        );

        if (tracker_ws_debug.GetBool())
            Msg(eDLL_T::SERVER, "TrackerSocket[DEBUG]: Get config command queued\n");
    }

    void WebSocketCommandHandler::HandleGetStatsCommand(const std::string& requestId)
    {
        TrackerDispatch
        (
            [this, requestId]()
            {
                rapidjson::Document response;
                response.SetObject();
                auto& alloc = response.GetAllocator();

                rapidjson::Value stats(rapidjson::kObjectType);
                stats.AddMember("socket_msg_count", m_messageCount.load(), alloc);
                stats.AddMember("uptime_seconds", Plat_FloatTime(), alloc);
                stats.AddMember("match_id", getMatchID(), alloc);

                SendResponse(requestId, "success", &stats, "Stats retrieved");
            }
        );

        if (tracker_ws_debug.GetBool())
            Msg(eDLL_T::SERVER, "TrackerSocket[DEBUG]: Get stats command queued\n");
    }

    void WebSocketCommandHandler::HandleReloadConfigCommand(const std::string& requestId)
    {
        TrackerDispatch
        (
            [this, requestId]()
            {
                ReloadConfig("r5rdev_config.json");

                rapidjson::Document response;
                response.SetObject();
                auto& alloc = response.GetAllocator();

                rapidjson::Value data(rapidjson::kObjectType);
                data.AddMember("config_reloaded", true, alloc);

                SendResponse(requestId, "success", &data, "Config reloaded successfully");

                Msg(eDLL_T::SERVER, "TrackerSocket: Config reloaded\n");
            }
        );

        if (tracker_ws_debug.GetBool())
            Msg(eDLL_T::SERVER, "TrackerSocket[DEBUG]: Reload config command queued\n");
    }

    void WebSocketCommandHandler::HandleUpdateConfigCommand(const rapidjson::Value& params, const std::string& requestId)
    {
        std::unordered_map<std::string, std::string> updates;
        std::unordered_set<std::string> deletes;

        if (params.HasMember("updates") && params["updates"].IsObject())
        {
            for (rapidjson::Value::ConstMemberIterator itr = params["updates"].MemberBegin(); itr != params["updates"].MemberEnd(); ++itr)
            {
                std::string key = itr->name.GetString();

                // Only REAL JSON null triggers delete. A string "null" is treated as a normal string.
                if (itr->value.IsNull())
                {
                    deletes.insert(key);
                    continue;
                }

                std::string value;

                if (itr->value.IsString())
                {
                    value = itr->value.GetString();
                }
                else if (itr->value.IsInt())
                {
                    value = std::to_string(itr->value.GetInt());
                }
                else if (itr->value.IsBool())
                {
                    value = itr->value.GetBool() ? "true" : "false";
                }
                else if (itr->value.IsDouble())
                {
                    value = std::to_string(itr->value.GetDouble());
                }
                else
                {
                    continue;
                }

                updates[key] = value;
            }
        }

        Msg
        (
            eDLL_T::SERVER,
            "TrackerSocket: Config update request with %zu set keys and %zu delete keys\n",
            updates.size(),
            deletes.size()
        );

        TrackerDispatch
        (
            [this, updates = std::move(updates), deletes = std::move(deletes), requestId]()
            {
                try
                {
                    // load servers tracker config from file
                    FileHandle_t configFile = FileSystem()->Open("r5rdev_config.json", "rb", "PLATFORM");
                    if (!configFile)
                    {
                        SendResponse(requestId, "error", nullptr, "Failed to open config file for reading");
                        return;
                    }

                    ssize_t fileSize = FileSystem()->Size(configFile);
                    if (fileSize <= 0)
                    {
                        FileSystem()->Close(configFile);
                        SendResponse(requestId, "error", nullptr, "Config file is empty or invalid size");
                        return;
                    }

                    char* buffer = new char[fileSize + 1];
                    ssize_t bytesRead = FileSystem()->Read(buffer, fileSize, configFile);
                    FileSystem()->Close(configFile);

                    if (bytesRead != fileSize)
                    {
                        delete[] buffer;
                        SendResponse(requestId, "error", nullptr, "Failed to read complete config file");
                        return;
                    }

                    buffer[fileSize] = '\0';

                    // parse existing config on server
                    rapidjson::Document doc;
                    doc.Parse(buffer);
                    delete[] buffer;

                    if (doc.HasParseError())
                    {
                        SendResponse(requestId, "error", nullptr, "Failed to parse existing config file");
                        return;
                    }

                    // Ensure root is an object before we try to treat it like one
                    if (!doc.IsObject())
                    {
                        SendResponse(requestId, "error", nullptr, "Config root must be a JSON object");
                        return;
                    }

                    auto& docAlloc = doc.GetAllocator();

                    int appliedSet = 0;
                    int appliedDelete = 0;

                    int changedSet = 0;
                    int changedDelete = 0;

                    // -------------------------
                    // Deletes first
                    // -------------------------
                    for (const std::string& key : deletes)
                    {
                        size_t dotPos = key.find('.');

                        if (dotPos != std::string::npos)
                        {
                            std::string parentKey = key.substr(0, dotPos);
                            std::string childKey = key.substr(dotPos + 1);

                            rapidjson::Value::MemberIterator pItr = doc.FindMember(parentKey.c_str());
                            if (pItr == doc.MemberEnd())
                                continue;

                            if (!pItr->value.IsObject())
                                continue;

                            rapidjson::Value& parentObj = pItr->value;

                            rapidjson::Value::MemberIterator cItr = parentObj.FindMember(childKey.c_str());
                            if (cItr == parentObj.MemberEnd())
                                continue;

                            parentObj.RemoveMember(cItr);

                            appliedDelete++;
                            changedDelete++;

                            if (tracker_ws_debug.GetBool())
                                Msg(eDLL_T::SERVER, "TrackerSocket[DEBUG]: Deleted: %s.%s\n", parentKey.c_str(), childKey.c_str());

                            if (parentObj.MemberCount() == 0)
                            {
                                doc.RemoveMember(pItr);
                                appliedDelete++;
                                changedDelete++;
                                if (tracker_ws_debug.GetBool())
                                    Msg(eDLL_T::SERVER, "TrackerSocket[DEBUG]: Deleted empty parent: %s\n", parentKey.c_str());
                            }
                        }
                        else
                        {
                            rapidjson::Value::MemberIterator itr = doc.FindMember(key.c_str());
                            if (itr == doc.MemberEnd())
                                continue;

                            doc.RemoveMember(itr);

                            appliedDelete++;
                            changedDelete++;

                            if (tracker_ws_debug.GetBool())
                                Msg(eDLL_T::SERVER, "TrackerSocket[DEBUG]: Deleted: %s\n", key.c_str());
                        }
                    }

                    // -------------------------
                    // Sets / updates
                    // -------------------------
                    for (const auto& [key, value] : updates)
                    {
                        size_t dotPos = key.find('.');

                        if (dotPos != std::string::npos)
                        {
                            std::string parentKey = key.substr(0, dotPos);
                            std::string childKey = key.substr(dotPos + 1);

                            bool didChange = false;

                            // Parent: create if missing, force to object if present but wrong type
                            if (!doc.HasMember(parentKey.c_str()))
                            {
                                rapidjson::Value pKey(parentKey.c_str(), docAlloc);
                                rapidjson::Value pVal(rapidjson::kObjectType);
                                doc.AddMember(pKey.Move(), pVal.Move(), docAlloc);

                                didChange = true;
                            }
                            else
                            {
                                rapidjson::Value& parentVal = doc[parentKey.c_str()];
                                if (!parentVal.IsObject())
                                {
                                    parentVal.SetObject();
                                    didChange = true;
                                }
                            }

                            rapidjson::Value& parentObj = doc[parentKey.c_str()];

                            rapidjson::Value::MemberIterator childItr = parentObj.FindMember(childKey.c_str());
                            if (childItr != parentObj.MemberEnd())
                            {
                                bool same = false;

                                if (childItr->value.IsString())
                                {
                                    const char* cur = childItr->value.GetString();
                                    rapidjson::SizeType curLen = childItr->value.GetStringLength();

                                    if (curLen == static_cast<rapidjson::SizeType>(value.size()))
                                    {
                                        if (std::memcmp(cur, value.c_str(), curLen) == 0)
                                            same = true;
                                    }
                                }

                                if (!same)
                                    didChange = true;

                                childItr->value.SetString
                                (
                                    value.c_str(),
                                    static_cast<rapidjson::SizeType>(value.size()),
                                    docAlloc
                                );
                            }
                            else
                            {
                                rapidjson::Value cKey(childKey.c_str(), docAlloc);
                                rapidjson::Value cVal;
                                cVal.SetString
                                (
                                    value.c_str(),
                                    static_cast<rapidjson::SizeType>(value.size()),
                                    docAlloc
                                );

                                parentObj.AddMember(cKey.Move(), cVal.Move(), docAlloc);
                                didChange = true;
                            }

                            appliedSet++;
                            if (didChange)
                                changedSet++;

                            if (tracker_ws_debug.GetBool())
                                Msg(eDLL_T::SERVER, "TrackerSocket[DEBUG]:  Updated: %s.%s = %s\n", parentKey.c_str(), childKey.c_str(), value.c_str());
                        }
                        else
                        {
                            bool didChange = false;

                            rapidjson::Value::MemberIterator topItr = doc.FindMember(key.c_str());
                            if (topItr != doc.MemberEnd())
                            {
                                bool same = false;

                                if (topItr->value.IsString())
                                {
                                    const char* cur = topItr->value.GetString();
                                    rapidjson::SizeType curLen = topItr->value.GetStringLength();

                                    if (curLen == static_cast<rapidjson::SizeType>(value.size()))
                                    {
                                        if (std::memcmp(cur, value.c_str(), curLen) == 0)
                                            same = true;
                                    }
                                }

                                if (!same)
                                    didChange = true;

                                topItr->value.SetString
                                (
                                    value.c_str(),
                                    static_cast<rapidjson::SizeType>(value.size()),
                                    docAlloc
                                );
                            }
                            else
                            {
                                rapidjson::Value k(key.c_str(), docAlloc);
                                rapidjson::Value v;
                                v.SetString
                                (
                                    value.c_str(),
                                    static_cast<rapidjson::SizeType>(value.size()),
                                    docAlloc
                                );

                                doc.AddMember(k.Move(), v.Move(), docAlloc);
                                didChange = true;
                            }

                            appliedSet++;
                            if (didChange)
                                changedSet++;

                            if (tracker_ws_debug.GetBool())
                                Msg(eDLL_T::SERVER, "TrackerSocket[DEBUG]: Updated: %s = %s\n", key.c_str(), value.c_str());
                        }
                    }

                    // serialize back to same
                    rapidjson::StringBuffer buf;
                    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buf);
                    writer.SetIndent(' ', 4);
                    doc.Accept(writer);

                    std::string jsonStr = buf.GetString();

                    // use sdk FileSystem() to write
                    FileHandle_t writeFile = FileSystem()->Open("r5rdev_config.json", "wb", "PLATFORM");
                    if (!writeFile)
                    {
                        SendResponse(requestId, "error", nullptr, "Failed to open config file for writing");
                        return;
                    }

                    ssize_t bytesWritten = FileSystem()->Write((const void*)jsonStr.c_str(), jsonStr.length(), writeFile);
                    FileSystem()->Close(writeFile);

                    if (bytesWritten != static_cast<ssize_t>(jsonStr.length()))
                    {
                        SendResponse(requestId, "error", nullptr, "Failed to write complete config file");
                        return;
                    }

                    rapidjson::Document response;
                    response.SetObject();
                    auto& alloc = response.GetAllocator();

                    rapidjson::Value data(rapidjson::kObjectType);

                    data.AddMember("requested_set", static_cast<int>(updates.size()), alloc);
                    data.AddMember("requested_delete", static_cast<int>(deletes.size()), alloc);

                    data.AddMember("applied_set", appliedSet, alloc);
                    data.AddMember("applied_delete", appliedDelete, alloc);

                    data.AddMember("changed_set", changedSet, alloc);
                    data.AddMember("changed_delete", changedDelete, alloc);

                    data.AddMember("message", rapidjson::Value("Config updated on disk. Call reload_config to apply.", alloc), alloc);

                    SendResponse
                    (
                        requestId,
                        "success",
                        &data,
                        CFmtStr
                        (
                            "Config written (requested: %zu set / %zu delete, applied: %d set / %d delete, changed: %d set / %d delete)",
                            updates.size(),
                            deletes.size(),
                            appliedSet,
                            appliedDelete,
                            changedSet,
                            changedDelete
                        ).Get()
                    );

                    Msg(eDLL_T::SERVER, "TrackerSocket: Config saved to disk. Call reload_config to apply.\n");
                }
                catch (const std::exception& e)
                {
                    SendResponse(requestId, "error", nullptr, std::string("Failed to update config: ") + e.what());
                    Error(eDLL_T::SERVER, NO_ERROR, "TrackerSocket: Exception in HandleUpdateConfigCommand: %s\n", e.what());
                }
            }
        );

        if (tracker_ws_debug.GetBool())
            Msg(eDLL_T::SERVER, "TrackerSocket[DEBUG]: Update config command queued\n");
    }



    void WebSocketCommandHandler::HandleReloadBanlistCommand(const std::string& requestId)
    {
        g_BanSystem.Clear();
        g_BanSystem.LoadList();

        SendResponse(requestId, "success", nullptr, "Banlist reloaded successfully");

        Msg(eDLL_T::SERVER, "TrackerSocket: Banlist reloaded\n");
    }

    void WebSocketCommandHandler::HandleAddBanCommand(const rapidjson::Value& params, const std::string& requestId)
    {
        if (!params.HasMember("player_id") || !params["player_id"].IsString())
        {
            SendResponse(requestId, "error", nullptr, "Missing required field: player_id");
            return;
        }

        netadr_t address;
        bool hasAddress = false;
        if (params.HasMember("ip_address") && params["ip_address"].IsString())
        {
            if (!address.SetFromString(params["ip_address"].GetString(), true))
            {
                SendResponse(requestId, "error", nullptr, "Invalid ip address provided.");
                return;
            }

            hasAddress = true;
        }

        const char* const playerId =
            params["player_id"].GetString();

        NucleusID_t empty = 0;
        if (!g_BanSystem.Bansystem_ValidateInputID(playerId, empty))
        {
            SendResponse(requestId, "error", nullptr, "player_id was an invalid uid.");
            return;
        }

        const char* const bannedById =
            (params.HasMember("banned_by_id") && params["banned_by_id"].IsString())
            ? params["banned_by_id"].GetString()
            : "";

        const char* const reason =
            (params.HasMember("reason") && params["reason"].IsString())
            ? params["reason"].GetString()
            : "";

        if (g_BanSystem.IsPlayerInServer(playerId))
            g_BanSystem.BanPlayerById(playerId, bannedById, reason);
        else
            g_BanSystem.AddIdToBanlist(playerId, bannedById, reason, hasAddress ? &address : nullptr);

        rapidjson::Document response;
        response.SetObject();
        auto& alloc = response.GetAllocator();

        rapidjson::Value data(rapidjson::kObjectType);
        data.AddMember("player_banned", true, alloc);
        data.AddMember("player_id", rapidjson::Value(playerId, alloc), alloc);

        SendResponse(requestId, "success", &data, "Player banned successfully");

        if (tracker_ws_debug.GetBool())
            Msg(eDLL_T::SERVER, "TrackerSocket[DEBUG]: Add Ban command queued for %s\n", playerId);
    }


    void WebSocketCommandHandler::HandleReloadServerCommand(const std::string& requestId)
    {
        if (!g_pServer || !g_pServer->IsActive())
        {
            SendResponse(requestId, "error", nullptr, "Game is not running");
            return;
        }

        SendResponse(requestId, "success", nullptr, "Reloading server.");
        Msg(eDLL_T::SERVER, "TrackerSocket: Request to reload server dispatched.");

        g_TaskQueue.Dispatch(Host_Reload, 0);
    }

    void WebSocketCommandHandler::HandleHandshakeCommand(const rapidjson::Document& doc, const std::string& requestId)
    {
        if (doc.HasMember("status") && doc["status"].IsString())
        {
            const char* status = doc["status"].GetString();
            const char* reason = doc.HasMember("reason") && doc["reason"].IsString() ? doc["reason"].GetString() : "";
            const bool accepted = strcmp(status, "accepted") == 0;

            rapidjson::Document dataDoc;
            dataDoc.SetObject();
            auto& alloc = dataDoc.GetAllocator();
            dataDoc.AddMember("type", rapidjson::Value(accepted ? "connection.success" : "connection.failed", alloc), alloc);
            dataDoc.AddMember("identifier", rapidjson::Value(m_cachedIdentifier.c_str(), alloc), alloc);
            if (reason[0])
                dataDoc.AddMember("reason", rapidjson::Value(reason, alloc), alloc);

            m_authorized.store(accepted);

            std::string message = accepted ? "Handshake accepted. Authenticated." : "Handshake declined. Disconnected.";
            if (reason[0] && !accepted)
                message += std::string(" Reason: ") + reason;

            const char* responseStatus = accepted ? "success" : "error";
            SendResponse(requestId, responseStatus, &dataDoc, message, "engine.ack");

            Msg(eDLL_T::SERVER, "TrackerSocket: Handshake %s%s%s\n", status, reason[0] ? " reason=" : "", reason);

            if (!accepted && m_webSocket)
                Disconnect();

            return;
        }

        if (m_cachedApiKey.empty() || m_cachedIdentifier.empty())
        {
            Error(eDLL_T::SERVER, NO_ERROR, "TrackerSocket: Missing api key or identifier for handshake. Disconnecting. \n");
            Disconnect();
            return;
        }

        rapidjson::Document update;
        update.SetObject();
        auto& alloc = update.GetAllocator();

        update.AddMember("id", rapidjson::Value(requestId.c_str(), alloc), alloc);
        update.AddMember("type", rapidjson::Value("connection.update", alloc), alloc);
        update.AddMember("identifier", rapidjson::Value(m_cachedIdentifier.c_str(), alloc), alloc);
        update.AddMember("api_key", rapidjson::Value(m_cachedApiKey.c_str(), alloc), alloc);

        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        update.Accept(writer);

        {
            std::lock_guard<std::shared_timed_mutex> lock(m_queueMutex);
            m_responseQueue.push(buffer.GetString());
        }

        if (tracker_ws_debug.GetBool())
            Msg(eDLL_T::SERVER, "TrackerSocket[DEBUG]: Handshake response queued (id=%s)\n", requestId.c_str());
    }

    void WebSocketCommandHandler::HandleToggleMute(const rapidjson::Value& params, const std::string& requestId)
    {
        if (!g_pServer || !g_pServer->IsActive())
        {
            SendResponse(requestId, "error", nullptr, "Game is not running");
            return;
        }

        if (!params.IsObject())
        {
            SendResponse(requestId, "error", nullptr, "params must be an object");
            return;
        }

        if (!params.HasMember("player_criteria") || !params["player_criteria"].IsString())
        {
            SendResponse(requestId, "error", nullptr, "Missing required field: player_criteria");
            return;
        }

        std::string criteria = params["player_criteria"].GetString();

        std::string reason =
            (params.HasMember("reason") && params["reason"].IsString())
            ? params["reason"].GetString()
            : "";

        std::string expiry =
            (params.HasMember("expiry") && params["expiry"].IsString())
            ? params["expiry"].GetString()
            : "";

        std::string mutedBy =
            (params.HasMember("muted_by") && params["muted_by"].IsString())
            ? params["muted_by"].GetString()
            : "";

        int timeoutAmount =
            (params.HasMember("timeout_amount") && params["timeout_amount"].IsInt())
            ? params["timeout_amount"].GetInt()
            : 0;

        bool toggle = true;

        if (params.HasMember("toggle"))
        {
            if (params["toggle"].IsBool())
                toggle = params["toggle"].GetBool();
            else if (params["toggle"].IsInt())
                toggle = (params["toggle"].GetInt() != 0);
            else
            {
                SendResponse(requestId, "error", nullptr, "Invalid toggle parameter");
                return;
            }
        }

        if (!g_BanSystem.IsPlayerInServer(criteria.c_str()))
        {
            if (V_IsAllDigit(criteria.c_str()))
            {
                const std::string responseMsg = CFmtStr("Player is not in server, scheduled to be %s automatically.", toggle ? "muted" : "unmuted").Get();
                SendResponse(requestId, "success", nullptr, responseMsg );
            }
            else
            {
                SendResponse(requestId, "error", nullptr, "Player is not in server, resend with UID to schedule mute action.");
                return;
            }

            g_TaskQueue.Dispatch
            (
                [criteria, reason, toggle, timeoutAmount, mutedBy, requestId]
                {
                    bool success = CALL_SERVER_SCRIPT_FUNC("CodeCallback_MuteFromRemote", MakeNoCopyStr(criteria.c_str()), MakeNoCopyStr(reason.c_str()), toggle, timeoutAmount, MakeNoCopyStr(mutedBy.c_str()), "void functionref( string uid, string reason, bool toggle, int timeoutAmount, string byPlayerUID )");
                    if (!success)
                        Error(eDLL_T::SERVER, NO_ERROR, "Failed to execute CodeCallback_MuteFromRemote for '%s'.\n", criteria.c_str());

                    TrackerSocketSystem()->SendResponse(requestId, success ? "success" : "error", nullptr, success ? "Player mute toggled" : "Failed to toggle player mute");
                }
                , 0
            );

            return;
        }

        g_TaskQueue.Dispatch
        (
            [criteria, reason, expiry, mutedBy, toggle, timeoutAmount, requestId]
            {
                bool success = CALL_SERVER_SCRIPT_FUNC
                (
                    "CodeCallback_MuteFromRemote",
                    MakeNoCopyStr(criteria.c_str()),
                    MakeNoCopyStr(reason.c_str()),
                    toggle,
                    timeoutAmount,
                    MakeNoCopyStr(mutedBy.c_str()),
                    "void functionref( string uid, string reason, bool toggle, int timeoutAmount, string byPlayerUID )"
                );

                if (!success)
                    Error(eDLL_T::SERVER, NO_ERROR, "Failed to execute CodeCallback_MuteFromRemote for '%s'.\n", criteria.c_str());

                // fire response from tracker task queue
                TRACKER::TaskManager::getInstance().AddTask
                (
                    [success, requestId, criteria, reason, expiry, mutedBy, toggle, timeoutAmount]
                    {
                        rapidjson::Document response;
                        response.SetObject();
                        auto& alloc = response.GetAllocator();

                        rapidjson::Value data(rapidjson::kObjectType);
                        data.AddMember("criteria", rapidjson::Value(criteria.c_str(), alloc), alloc);
                        data.AddMember("toggle", toggle, alloc);
                        data.AddMember("timeout_amount", timeoutAmount, alloc);
                        data.AddMember("success", success, alloc);

                        if (!reason.empty())
                            data.AddMember("reason", rapidjson::Value(reason.c_str(), alloc), alloc);

                        if (!expiry.empty())
                            data.AddMember("expiry", rapidjson::Value(expiry.c_str(), alloc), alloc);

                        if (!mutedBy.empty())
                            data.AddMember("muted_by", rapidjson::Value(mutedBy.c_str(), alloc), alloc);

                        TrackerSocketSystem()->SendResponse
                        (
                            requestId,
                            success ? "success" : "failed",
                            &data,
                            success ? "Mute toggled successfully" : "Failed to toggle mute"
                        );
                    }
                );
            }
            , 0
        );

        if (tracker_ws_debug.GetBool())
            Msg(eDLL_T::SERVER, "TrackerSocket[DEBUG]: Toggle mute command queued for %s\n", criteria.c_str());
    }

    void WebSocketCommandHandler::HandleSendMessage(const rapidjson::Value& params, const std::string& requestId)
    {
        if (!g_pServer || !g_pServer->IsActive())
        {
            SendResponse(requestId, "error", nullptr, "Game is not running");
            return;
        }

        if (!params.IsObject())
        {
            SendResponse(requestId, "error", nullptr, "params must be an object");
            return;
        }

        if (!params.HasMember("player_criteria") || !params["player_criteria"].IsString())
        {
            SendResponse(requestId, "error", nullptr, "Missing required field: player_criteria");
            return;
        }

        if (!params.HasMember("from_user") || !params["from_user"].IsString())
        {
            SendResponse(requestId, "error", nullptr, "Invalid or missing field: from_user");
            return;
        }

        if (!params.HasMember("message") || !params["message"].IsString())
        {
            SendResponse(requestId, "error", nullptr, "Invalid or missing field: message");
            return;
        }

        if (!params.HasMember("bool_to_all") || !params["bool_to_all"].IsBool())
        {
            SendResponse(requestId, "error", nullptr, "Invalid or missing field: bool_to_all");
            return;
        }

        const char* criteria = params["player_criteria"].GetString();
        bool bToAll = params["bool_to_all"].GetBool();
        if (bToAll == false)
        {
            if (!g_BanSystem.IsPlayerInServer(criteria))
            {
                SendResponse(requestId, "error", nullptr, "Player is not in server to message");
                return;
            }
        }

        const char* message = params["message"].GetString();
        if (!V_IsValidUTF8(message))
        {
            SendResponse(requestId, "error", nullptr, "'message' contains invalid characters");
            return;
        }

        const char* fromUser = params["from_user"].GetString();
        if (!V_IsValidUTF8(fromUser))
        {
            SendResponse(requestId, "error", nullptr, "'from_user' user contains invalid characters");
            return;
        }

        bool success = CALL_SERVER_SCRIPT_FUNC( "CodeCallback_SendMessage", criteria, fromUser, message, bToAll, "void functionref( string, string, string, bool )" );

        if (!success)
        {
            SendResponse(requestId, "error", nullptr, "Could not call CodeCallback_SendMessage");
            return;
        }

        SendResponse(requestId, "success", nullptr, "Message sent");
    }

    void WebSocketCommandHandler::HandleTimeoutCommand(const rapidjson::Value& params, const std::string& requestId)
    {
        if (!g_pServer || !g_pServer->IsActive())
        {
            SendResponse(requestId, "error", nullptr, "Game is not running");
            return;
        }

        if (!params.IsObject())
        {
            SendResponse(requestId, "error", nullptr, "params must be an object");
            return;
        }

        if (!params.HasMember("player_criteria") || !params["player_criteria"].IsString())
        {
            SendResponse(requestId, "error", nullptr, "Missing required field: player_criteria");
            return;
        }

        if (!params.HasMember("from_user") || !params["from_user"].IsString())
        {
            SendResponse(requestId, "error", nullptr, "Invalid or missing field: from_user");
            return;
        }

        if (!params.HasMember("reason") || !params["reason"].IsString())
        {
            SendResponse(requestId, "error", nullptr, "Invalid or missing field: reason");
            return;
        }

        if (!params.HasMember("bool_toggle") || !params["bool_toggle"].IsBool())
        {
            SendResponse(requestId, "error", nullptr, "Invalid or missing field: bool_toggle");
            return;
        }

        if (!params.HasMember("timeout_amount") || !params["timeout_amount"].IsInt())
        {
            SendResponse(requestId, "error", nullptr, "Invalid or missing field: timeout_amount");
            return;
        }

        const char* criteria = params["player_criteria"].GetString();
        if (!V_IsValidUTF8(criteria))
        {
            SendResponse(requestId, "error", nullptr, "'player_criteria' contains invalid characters");
            return;
        }

        if (!g_BanSystem.IsPlayerInServer(criteria))
        {
            SendResponse(requestId, "error", nullptr, "Player is not in server to timeout");
            return;
        }

        const char* reason = params["reason"].GetString();
        if (!V_IsValidUTF8(reason))
        {
            SendResponse(requestId, "error", nullptr, "'reason' contains invalid characters");
            return;
        }

        const char* fromUser = params["from_user"].GetString();
        if (!V_IsValidUTF8(fromUser))
        {
            SendResponse(requestId, "error", nullptr, "'from_user' user contains invalid characters");
            return;
        }

        bool toggle = params["bool_toggle"].GetBool();
        int timeoutAmount = params["timeout_amount"].GetInt();

        bool success = CALL_SERVER_SCRIPT_FUNC("CodeCallback_TimeoutCommand", toggle, criteria, timeoutAmount, fromUser, reason, "void functionref( bool, string, int, string, string )");

        if (!success)
        {
            SendResponse(requestId, "error", nullptr, "Could not call CodeCallback_TimeoutCommand");
            return;
        }

        SendResponse(requestId, "success", nullptr, "Timeout player requested");
    }


    //===========================================================================
    // Message Validation & Authentication
    //===========================================================================

    bool WebSocketCommandHandler::ValidateMessage(const rapidjson::Document& doc, std::string& outError)
    {
        if (!doc.IsObject())
        {
            outError = "Message must be a JSON object";
            return false;
        }

        if (!doc.HasMember("id") || !doc["id"].IsString())
        {
            outError = "Missing or invalid field: id";
            return false;
        }

        if (!doc.HasMember("type") || !doc["type"].IsString())
        {
            outError = "Missing or invalid field: type";
            return false;
        }

        if (!doc.HasMember("params") || !doc["params"].IsObject())
        {
            outError = "Missing or invalid field: params";
            return false;
        }

        return true;
    }

    bool WebSocketCommandHandler::AuthenticateMessage(const rapidjson::Document& doc, std::string& outError)
    {
        const std::string type = doc.HasMember("type") && doc["type"].IsString() ? doc["type"].GetString() : "";

        if (type == "handshake")
            return true;

        if (!m_authorized.load())
        {
            outError = "Handshake required";
            return false;
        }

        const auto& params = doc["params"];

        // Validate API key
        if (!params.HasMember("api_key") || !params["api_key"].IsString())
        {
            outError = "Missing API key";
            return false;
        }

        const char* providedKey = params["api_key"].GetString();

        size_t expectedLen = m_cachedApiKey.length();
        size_t providedLen = strlen(providedKey);

        if (providedLen != expectedLen)
        {
            outError = "Invalid API key";
            return false;  // Fast rejection for wrong length
        }

        // Constant-time comparison (only if lengths match)
        unsigned char result = 0;
        for (size_t i = 0; i < expectedLen; ++i)
        {
            result |= (providedKey[i] ^ m_cachedApiKey[i]);
        }

        if (result != 0)
        {
            outError = "Invalid API key";
            return false;
        }

        // Validate server identifier
        if (!params.HasMember("identifier") || !params["identifier"].IsString())
        {
            outError = "Missing server identifier";
            return false;
        }

        std::string providedIdentifier = params["identifier"].GetString();
        std::string expectedIdentifier = m_cachedIdentifier;

        size_t expectedIdLen = expectedIdentifier.length();
        size_t providedIdLen = providedIdentifier.length();

        if (providedIdLen != expectedIdLen)
        {
            outError = "Invalid server identifier";
            return false;
        }

        // Constant-time comparison for identifier
        unsigned char idResult = 0;
        for (size_t i = 0; i < expectedIdLen; ++i)
        {
            idResult |= (providedIdentifier[i] ^ expectedIdentifier[i]);
        }

        if (idResult != 0)
        {
            outError = "Invalid server identifier";
            return false;
        }

        return true;
    }

    void WebSocketCommandHandler::OnWebSocketConVarChanged(IConVar* var, const char* pOldValue, float flOldValue, const char* newValue)
    {
        if (!var)
            return;

        if (!pOldValue || !newValue)
            return;

        if (strcmp(pOldValue, newValue) == 0)
        {
            if (tracker_ws_debug.GetBool())
                Msg(eDLL_T::SERVER, "TrackerSocket[DEBUG]: Nothing changed for '%s' \n", var->GetName());

            return;
        }

        ConVar* const cvar = static_cast<ConVar*>(var);

        const bool needsReconnect =
        (
            cvar == &tracker_ws_hostname ||
            cvar == &tracker_ws_port ||
            cvar == &tracker_ws_use_ssl ||
            cvar == &tracker_ws_lax_ssl ||
            cvar == &tracker_ws_tls_version ||
            cvar == &tracker_ws_max_retries ||
            cvar == &tracker_ws_retry_time ||
            cvar == &tracker_ws_time_out ||
            cvar == &tracker_ws_keep_alive
       );

        if (needsReconnect && tracker_ws_reconnect_on_change.GetBool())
        {
            m_pendingReconnect.store(true);
            return;
        }

        // apply live changes if not needing a reconnect.
        m_configDirty.store(true);

        if (cvar == &tracker_ws_enable && tracker_ws_enable.GetBool() && !m_initialized.load())
            m_pendingReconnect.store(true);
    }




    //===========================================================================
    // Response Sending
    //===========================================================================

    void WebSocketCommandHandler::SendResponse(const std::string& requestId,
        const char* status,
        const rapidjson::Value* data,
        const std::string& message,
        const char* type)
    {
        rapidjson::Document response;
        response.SetObject();
        auto& alloc = response.GetAllocator();

        // Build response
        if (type && strcmp(type, "") != 0)
            response.AddMember("type", rapidjson::Value(type, alloc), alloc);
        response.AddMember("id", rapidjson::Value(requestId.c_str(), alloc), alloc);
        response.AddMember("status", rapidjson::Value(status, alloc), alloc);
        response.AddMember("timestamp", Plat_FloatTime(), alloc);

        if (data)
        {
            rapidjson::Value dataCopy(*data, alloc);
            response.AddMember("data", dataCopy, alloc);
        }

        response.AddMember("message", rapidjson::Value(message.c_str(), alloc), alloc);

        // Serialize
        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        response.Accept(writer);

        // Queue for sending
        {
            std::lock_guard<std::shared_timed_mutex> lock(m_queueMutex);
            m_responseQueue.push(buffer.GetString());
        }

        if (tracker_ws_debug.GetBool())
            Msg(eDLL_T::SERVER, "TrackerSocket[DEBUG]: Response queued (id=%s, status=%s)\n", requestId.c_str(), status);
    }

    void WebSocketCommandHandler::RelayChatMessage(unsigned __int64 senderNucleus, const char* name, const char* message)
    {
        if (!m_initialized.load() || !m_webSocket)
            return;
        if (!m_authorized.load())
            return;
        if (!name || !message || message[0] == '\0')
            return;
        if (m_cachedApiKey.empty() || m_cachedIdentifier.empty())
            return;

        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();

        CFmtStrN<21> msgIdFmt("%llu", static_cast<unsigned long long>(Plat_FloatTime() * 1000.0));

        doc.AddMember("id", rapidjson::Value(msgIdFmt.Get(), alloc), alloc);
        doc.AddMember("type", rapidjson::Value("chat.message", alloc), alloc);
        doc.AddMember("identifier", rapidjson::Value(m_cachedIdentifier.c_str(), alloc), alloc);
        doc.AddMember("api_key", rapidjson::Value(m_cachedApiKey.c_str(), alloc), alloc);
        doc.AddMember("message", rapidjson::Value(message, alloc), alloc);
        doc.AddMember("player", rapidjson::Value(name, alloc), alloc);
        doc.AddMember("nucleus_id", senderNucleus, alloc);
        doc.AddMember("timestamp", Plat_FloatTime(), alloc);
        doc.AddMember("match_id", getMatchID(), alloc);

        rapidjson::StringBuffer buffer;
        rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
        doc.Accept(writer);

        {
            std::lock_guard<std::shared_timed_mutex> lock(m_queueMutex);
            m_responseQueue.push(buffer.GetString());
        }
    }

    void WebSocketCommandHandler::RelayChatMute(const char* const pszPlayerName, NucleusID_t nucleusId, const char* const pszReason, const char* const pszExpiry, const char* const pszMutedBy, bool toggle, int expiryUnixTimestamp)
    {
        if (!tracker_ws_relay_chat.GetBool())
            return;

        if (!m_initialized.load() || !m_webSocket)
            return;

        if (!m_authorized.load())
            return;

        if (!VALID_CHARSTAR(pszPlayerName))
            return;

        if (nucleusId <= 0)
            return;

        if (m_cachedApiKey.empty() || m_cachedIdentifier.empty())
            return;

        const char* reason = VALID_CHARSTAR(pszReason) ? pszReason : "";
        const char* expiry = VALID_CHARSTAR(pszExpiry) ? pszExpiry : "";
        const char* mutedBy = VALID_CHARSTAR(pszMutedBy) ? pszMutedBy : "";

        rapidjson::Document doc;
        doc.SetObject();
        auto& alloc = doc.GetAllocator();

        CFmtStrN<21> msgIdFmt("%llu", static_cast<unsigned long long>(Plat_FloatTime() * 1000.0));

        doc.AddMember("id", rapidjson::Value(msgIdFmt.Get(), alloc), alloc);
        doc.AddMember("type", rapidjson::Value("chat.mute", alloc), alloc);
        doc.AddMember("identifier", rapidjson::Value(m_cachedIdentifier.c_str(), alloc), alloc);
        doc.AddMember("api_key", rapidjson::Value(m_cachedApiKey.c_str(), alloc), alloc);

        doc.AddMember("player_name", rapidjson::Value(pszPlayerName, alloc), alloc);

        CFmtStrN<32> uidFmt("%llu", static_cast<unsigned long long>(nucleusId));
        doc.AddMember("player_uid", rapidjson::Value(uidFmt.Get(), alloc), alloc);

        doc.AddMember("reason", rapidjson::Value(reason, alloc), alloc);
        doc.AddMember("expiry_text", rapidjson::Value(expiry, alloc), alloc);
        doc.AddMember("muted_by", rapidjson::Value(mutedBy, alloc), alloc);

        doc.AddMember("toggle", toggle, alloc);
        doc.AddMember("timestamp", Plat_FloatTime(), alloc);
        doc.AddMember("match_id", getMatchID(), alloc);
        doc.AddMember("expiry_timestamp", expiryUnixTimestamp, alloc);

        rapidjson::StringBuffer buffer;
        rapidjson::Writer< rapidjson::StringBuffer > writer(buffer);
        doc.Accept(writer);

        {
            std::lock_guard< std::shared_timed_mutex > lock(m_queueMutex);
            m_responseQueue.push(buffer.GetString());
        }
    }

    void WebSocketCommandHandler::RunFrame()
    {
        if (!tracker_ws_enable.GetBool())
        {
            m_configDirty.store(false);
            m_pendingReconnect.store(false);

            if (m_initialized.load() || m_webSocket || m_connectedAddress)
                Shutdown();

            return;
        }

        if (m_configDirty.exchange(false))
            ApplyConVars();

        if (m_pendingReconnect.exchange(false))
            Reconnect();

        if (!m_initialized.load())
            return;

        static double lastWebSocketUpdate = 0.0;
        double currentTime = Plat_FloatTime();

        if (currentTime - lastWebSocketUpdate >= m_throttleRate) // 100ms throttle default
        {
            Update();
            ProcessMessageQueue();
            lastWebSocketUpdate = currentTime;
        }
    }

    void WebSocketCommandHandler::Reconnect()
    {
        TrackerSocketSystem()->Disconnect();

        std::string trackerHostStr = TRACKER::GetSetting("server.TRACKER_HOST");
        const char* trackerHost = trackerHostStr.empty() ? tracker_ws_hostname.GetString() : trackerHostStr.c_str();
        int trackerPort = tracker_ws_port.GetInt();

        size_t hostLen = strlen(trackerHost);

        if (hostLen <= 0)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "TrackerSocket: Cannot connect to empty hostname."); //don't even try
            return;
        }

        Msg(eDLL_T::SERVER, "TrackerSocket: Reconnection attempt -- TRACKER_HOST=[%s] (len=%zu) \n", trackerHost, hostLen);
        TrackerSocketSystem()->Connect(trackerHost, trackerPort > 0 ? trackerPort : TRACKER_WS_PORT);
    }

    void WebSocketCommandHandler::AllocateAddress(const char* address)
    {
        FreeAddress();
        if (address && address[0] != '\0')
        {
            size_t len = strlen(address) + 1;
            m_connectedAddress = new char[len];
            memcpy(m_connectedAddress, address, len);

            if (tracker_ws_debug.GetBool())
                Msg(eDLL_T::SERVER, "TrackerSocket[DEBUG]: AllocateAddress stored=[%s] (len=%zu)\n", m_connectedAddress, len);
        }
    }

    void WebSocketCommandHandler::FreeAddress()
    {
        if (m_connectedAddress)
        {
            delete[] m_connectedAddress;
            m_connectedAddress = nullptr;
        }
    }

    void WebSocketCommandHandler::Status()
    {
        const char* state = "Unknown";
        if (!m_connectedAddress || !m_webSocket)
            state = "Disconnected";
        else
            state = m_webSocket->GetStateString(m_webSocket->GetState(m_connectedAddress));

        Msg(eDLL_T::SERVER, "TrackerSocket: State: %s | IncomingMessageCount: %" PRIu64 " |  LastUpdate: %.2f | LastConnectAttempt: %.2f | InternalConnectedState: %s \n", state, m_messageCount.load(), m_lastUpdateTime, m_lastConnectAttempt, m_isConnected.load() ? "true" : "false");
    }

    void WebSocketCommandHandler::ApplyConVars()
    {
        if (!tracker_ws_enable.GetBool())
        {
            if (m_initialized.load())
                Shutdown();

            return;
        }

        m_throttleRate = ClampThrottleRate(tracker_ws_throttle_rate.GetFloat());
        m_cachedApiKey = GetSetting("apikey");
        m_cachedIdentifier = GetSetting("identifier");

        // Buffer size (startup config only): clamp + store back to convar
        int32_t bufSize = tracker_ws_buffer_size.GetInt();

        bufSize = ClampBuffer(bufSize);

        if (bufSize != tracker_ws_buffer_size.GetInt())
            tracker_ws_buffer_size.SetValue(bufSize);


        // Only resize the receive buffer if we aren't initialized yet
        if (!m_initialized.load())
            m_receiveBuffer.resize(static_cast<size_t>(bufSize));
    }

    bool WebSocketCommandHandler::InstallCaBundleFromPlatform(const char* pPlatformFile)
    {
        if (pPlatformFile == nullptr || pPlatformFile[0] == '\0')
        {
            Error(eDLL_T::SERVER, NO_ERROR, "TrackerSocket: CA bundle file not specified. Aborting..\n");
            return false;
        }

        if (!FileSystem() || !FileSystem()->FileExists(pPlatformFile, "PLATFORM"))
        {
            Error(eDLL_T::SERVER, NO_ERROR, "TrackerSocket: Could not find CA bundle file '%s' in /platform. Aborting.\n", pPlatformFile);
            return false;
        }

        FileHandle_t pFile = FileSystem()->Open(pPlatformFile, "rb", "PLATFORM");
        if (!pFile)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "TrackerSocket: Invalid file handle for '%s'. Aborting.\n", pPlatformFile);
            return false;
        }

        const ssize_t nFileSize = FileSystem()->Size(pFile);
        if (nFileSize <= 0)
        {
            FileSystem()->Close(pFile);
            Error(eDLL_T::SERVER, NO_ERROR, "TrackerSocket: CA bundle file '%s' was empty. Aborting..\n", pPlatformFile);
            return false;
        }

        const u64 nBufSize = FileSystem()->GetOptimalReadSize(pFile, nFileSize + 1);
        char* const pBuf = (char*)FileSystem()->AllocOptimalReadBuffer(pFile, nBufSize, 0);

        if (!pBuf)
        {
            FileSystem()->Close(pFile);
            Error(eDLL_T::SERVER, NO_ERROR, "TrackerSocket: Failed to allocate CA bundle buffer.\n");
            return false;
        }

        const ssize_t nRead = FileSystem()->ReadEx(pBuf, nBufSize, nFileSize, pFile);
        FileSystem()->Close(pFile);

        if (nRead <= 0)
        {
            FileSystem()->FreeOptimalReadBuffer(pBuf);
            return false;
        }

        pBuf[nRead] = '\0';

        const int32_t iResult = CWebSocket::SetCaCert((uint8_t*)pBuf, (int32_t)nRead);

        FileSystem()->FreeOptimalReadBuffer(pBuf);
        if (iResult <= 0)
            Error(eDLL_T::SERVER, NO_ERROR, "TrackerSocket: CA bundle file '%s' failed to set. Aborting..\n", pPlatformFile);
        else
        {
            m_loadedCaBundle.store(true);
            Msg(eDLL_T::SERVER, "TrackerSocket: Successfully installed CA bundle file: '%s'\n", pPlatformFile);
        }

        return iResult > 0;
    }

    void WebSocketCommandHandler::__CheckInstallCA()
    {
        if (m_loadedCaBundle.load())
        {
            Error(eDLL_T::SERVER, NO_ERROR, "Cannot load CA file, already installed.");
            return;
        }

        const char* bundleFile = tracker_ws_ca_bundle_file.GetString();
        if (!VALID_CHARSTAR(bundleFile))
            return;

        Msg(eDLL_T::SERVER, "TrackerSocket: Installing CA Bundle file: %s\n", bundleFile);
        if (!InstallCaBundleFromPlatform(bundleFile))
        {
            Warning(eDLL_T::SERVER, "TrackerSocket: CA bundle was not installed; tracker_ws_lax_ssl will be forced to 1 on connect.\n");
            m_forceLaxSSL = true;
        }
    }

    int32_t WebSocketCommandHandler::ClampBuffer(int32_t bufSize)
    {
        if (bufSize < 1024)
            bufSize = 1024;

        if (bufSize > (2 * 1024 * 1024)) //not sure why anyone would want 2mb, but here you go.
            bufSize = (2 * 1024 * 1024);

        return bufSize;
    }

    float WebSocketCommandHandler::ClampThrottleRate(float throttleValue)
    {
        if (throttleValue < 0.0f) //non negative
            throttleValue = 0.0f;

        return throttleValue;
    }

    bool WebSocketCommandHandler::IsInitialized()
    {
        return m_initialized.load();
    }
} // namespace TRACKER

//-----------------------------------------------------------------------------
// Singleton accessor
//-----------------------------------------------------------------------------
TRACKER::WebSocketCommandHandler* TrackerSocketSystem()
{
    return &TRACKER::WebSocketCommandHandler::getInstance();
}

//--------------------------------------------------------------------------
// ConVar callback
//--------------------------------------------------------------------------
static void TrackerWs_OnConVarChanged(IConVar* var, const char* pOldValue, float flOldValue, ChangeUserData_t pUserData)
{
    if (!var)
    {
        Error(eDLL_T::SERVER, NO_ERROR, "TrackerSocket: *var was nullptr");
        return;
    }

    ConVar* pConVar = static_cast<ConVar*>(var);
    const char* newValue = pConVar->GetString();

    if (tracker_ws_debug.GetBool())
        Msg(eDLL_T::SERVER, "TrackerSocket[DEBUG]: Var changed: '%s'; old:'%s' new:'%s' \n", var->GetName(), pOldValue, newValue);

    bool initialized = TrackerSocketSystem()->IsInitialized();

    if (pConVar == &tracker_ws_ca_bundle_file)
    {
        if (initialized)
        {
            Error(eDLL_T::SERVER, NO_ERROR, "TrackerSocket: Cannot load CA file after TrackerSocketSystem has been initialized. Changes have no effect.");
            return;
        }

        TrackerSocketSystem()->__CheckInstallCA();
    }

    TrackerSocketSystem()->OnWebSocketConVarChanged(var, pOldValue, flOldValue, newValue);
}

//--------------------------------------------------------------------------
// ConVars
//--------------------------------------------------------------------------
ConVar tracker_ws_enable("tracker_ws_enable", "1", FCVAR_RELEASE, "Enable WebSocket remote command interface (0 = disabled, 1 = enabled)", &TrackerWs_OnConVarChanged);
ConVar tracker_ws_port("tracker_ws_port", "9705", FCVAR_RELEASE, "WebSocket server port", &TrackerWs_OnConVarChanged);
ConVar tracker_ws_debug("tracker_ws_debug", "0", FCVAR_RELEASE, "Enable WebSocket debug logging (0 = disabled, 1 = enabled)", &TrackerWs_OnConVarChanged);
ConVar tracker_ws_use_ssl("tracker_ws_use_ssl", "1", FCVAR_RELEASE, "Use SSL for WebSocket connection (0 = disabled, 1 = enabled)", &TrackerWs_OnConVarChanged);
ConVar tracker_ws_lax_ssl("tracker_ws_lax_ssl", "0", FCVAR_RELEASE, "Lax SSL certificate validation (0 = strict, 1 = lax)", &TrackerWs_OnConVarChanged);
ConVar tracker_ws_buffer_size("tracker_ws_buffer_size", "261120", FCVAR_RELEASE, "WebSocket buffer size in bytes. Startup config only.", &TrackerWs_OnConVarChanged);
ConVar tracker_ws_max_retries("tracker_ws_max_retries", "3", FCVAR_RELEASE, "Maximum number of WebSocket connection retries", &TrackerWs_OnConVarChanged);
ConVar tracker_ws_retry_time("tracker_ws_retry_time", "5.0", FCVAR_RELEASE, "Time in seconds between WebSocket connection retries. float", &TrackerWs_OnConVarChanged);
ConVar tracker_ws_time_out("tracker_ws_time_out", "125", FCVAR_RELEASE, "WebSocket connection timeout in seconds", &TrackerWs_OnConVarChanged);
ConVar tracker_ws_keep_alive("tracker_ws_keep_alive", "60", FCVAR_RELEASE, "WebSocket keep-alive interval in seconds", &TrackerWs_OnConVarChanged);
ConVar tracker_ws_throttle_rate("tracker_ws_throttle_rate", "0.10", FCVAR_RELEASE, "WebSocket message processing throttle rate in seconds. Default 100ms (0 = no throttling)", &TrackerWs_OnConVarChanged);
ConVar tracker_ws_hostname("tracker_ws_hostname", "r5r.dev", FCVAR_RELEASE, "WebSocket server hostname", &TrackerWs_OnConVarChanged);
ConVar tracker_ws_tls_version("tracker_ws_tls_version", "3", FCVAR_RELEASE, "Forces SSL to Transport Layer Security version  ( 0: [1.0] | 1: [1.1] | 2: [1.2] | 3: [1.3][default] )", &TrackerWs_OnConVarChanged);
ConVar tracker_ws_relay_chat("tracker_ws_relay_chat", "0", FCVAR_RELEASE, "Relays chat messages to qualified clients via web panel. (0 = disabled, 1 = enabled)", &TrackerWs_OnConVarChanged);
ConVar tracker_ws_reconnect_on_change("tracker_ws_reconnect_on_change", "1", FCVAR_RELEASE, "Reconnect to remote socket when qualified convars are changed. (0 = disabled, 1 = enabled )");
ConVar tracker_ws_ca_bundle_file("tracker_ws_ca_bundle_file", "Sectigobundle.pem", FCVAR_RELEASE, "Required to validate Sectigo certificate chains.", &TrackerWs_OnConVarChanged);
ConVar tracker_ws_reconnect_on_newgame("tracker_ws_reconnect_on_newgame", "0", FCVAR_RELEASE, "Reconnect the socket for each new game.");

//--------------------------------------------------------------------------
// ConCommands
//--------------------------------------------------------------------------
static void TrackerWs_Reconnect() { TrackerSocketSystem()->Reconnect(); }
static void TrackerWs_Shutdown() { TrackerSocketSystem()->Shutdown(); }
static void TrackerWs_Status() { TrackerSocketSystem()->Status(); }

ConCommand tracker_ws_reconnect("tracker_ws_reconnect", TrackerWs_Reconnect, "Restart the WebSocket connection to the remote server.", FCVAR_RELEASE);
ConCommand tracker_ws_shutdown("tracker_ws_shutdown", TrackerWs_Shutdown, "Shutdown the WebSocket connection to the remote server.", FCVAR_RELEASE);
ConCommand tracker_ws_status("tracker_ws_status", TrackerWs_Status, "Display the current status of the WebSocket connection to the remote server.", FCVAR_RELEASE);
#endif // CLIENT_DLL
