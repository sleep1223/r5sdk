//===========================================================================//
// 
// Purpose: WebSocket implementation
// 
//===========================================================================//
#include "tier2/websocket.h"

#include "DirtySDK/dirtysock.h"
#include "DirtySDK/dirtysock/netconn.h"
#include "DirtySDK/proto/protossl.h"
#include "DirtySDK/proto/protowebsocket.h"

//-----------------------------------------------------------------------------
// constructors/destructors
//-----------------------------------------------------------------------------
CWebSocket::CWebSocket()
{
	m_initialized = false;
}

//-----------------------------------------------------------------------------
// Purpose: initialization of the socket system
//-----------------------------------------------------------------------------
bool CWebSocket::Init(const char* const addressList, const ConnParams_s& params, const char*& initError)
{
	Assert(addressList);

	if (m_initialized)
	{
		initError = "Already initialized";
		return false;
	}

	if (!NetConnStatus('open', 0, NULL, 0))
	{
		initError = "Network connection module not initialized";
		return false;
	}

	if (!UpdateAddressList(addressList))
	{
		initError = (*addressList)
			? "Address list is invalid"
			: "Address list is empty";

		return false;
	}

	m_connParams = params;
	m_initialized = true;

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: shutdown of the socket system
//-----------------------------------------------------------------------------
void CWebSocket::Shutdown()
{
	if (!m_initialized)
		return;

	m_initialized = false;
	ClearAll();
}

//-----------------------------------------------------------------------------
// Purpose: adds comma separated addresses to connection list, returns false if
// connection list is empty
//-----------------------------------------------------------------------------
bool CWebSocket::UpdateAddressList(const char* const addressList)
{
	Assert(addressList);
	const CUtlStringList addresses(addressList, ",");

	FOR_EACH_VEC(addresses, i)
	{
		const ConnContext_s conn(addresses[i]);
		m_addressList.AddToTail(conn);
	}

	return addresses.Count() != 0;
}

//-----------------------------------------------------------------------------
// Purpose: update parameters for each connection
//-----------------------------------------------------------------------------
void CWebSocket::UpdateParams(const ConnParams_s& params)
{
	m_connParams = params;

	for (ConnContext_s& conn : m_addressList)
	{
		if (conn.webSocket)
			conn.SetParams(params);
	}
}

//-----------------------------------------------------------------------------
// Purpose: socket state machine
//-----------------------------------------------------------------------------
void CWebSocket::Update()
{
	if (!IsInitialized())
		return;

	const double queryTime = Plat_FloatTime();

	for (ConnContext_s& conn : m_addressList)
	{
		if (conn.webSocket)
			ProtoWebSocketUpdate(conn.webSocket);

		if (conn.state == CS_CREATE || conn.state == CS_RETRY)
		{
			conn.Connect(queryTime, m_connParams);
			continue;
		}

		if (conn.state == CS_CONNECTED || conn.state == CS_LISTENING)
		{
			conn.Process(queryTime);
			continue;
		}

		if (conn.state == CS_DESTROYED)
		{
			if (conn.tryCount > m_connParams.maxRetries)
			{
				// All retry attempts have been used; mark unavailable for deletion
				conn.state = CS_UNAVAIL;
			}
			else
			{
				// Mark as retry, this will recreate the socket and reattempt
				// the connection
				conn.state = CS_RETRY;
			}
		}
	}

	DeleteUnavailable();
}

//-----------------------------------------------------------------------------
// Purpose: delete all connections marked unavailable
//-----------------------------------------------------------------------------
void CWebSocket::DeleteUnavailable()
{
	FOR_EACH_VEC_BACK(m_addressList, i)
	{
		if (m_addressList[i].state == CS_UNAVAIL)
			m_addressList.FastRemove(i);
	}
}

//-----------------------------------------------------------------------------
// Purpose: disconnect all connections
//-----------------------------------------------------------------------------
void CWebSocket::DisconnectAll()
{
	for (ConnContext_s& conn : m_addressList)
	{
		conn.Disconnect();
	}
}

//-----------------------------------------------------------------------------
// Purpose: reconnect all connections
//-----------------------------------------------------------------------------
void CWebSocket::ReconnectAll()
{
	for (ConnContext_s& conn : m_addressList)
	{
		conn.Reconnect();
	}
}

//-----------------------------------------------------------------------------
// Purpose: destroy and purge all connections
//-----------------------------------------------------------------------------
void CWebSocket::ClearAll()
{
	DisconnectAll();
	m_addressList.Purge();
}

//-----------------------------------------------------------------------------
// Purpose: send data to all sockets
//-----------------------------------------------------------------------------
void CWebSocket::SendData(const char* const dataBuf, const int32_t dataSize)
{
	Assert(dataBuf);
	Assert(dataSize);

	if (!IsInitialized())
		return;

	for (ConnContext_s& conn : m_addressList)
	{
		if (conn.state != CS_LISTENING)
			continue;

		if (ProtoWebSocketSend(conn.webSocket, dataBuf, dataSize) < 0)
			conn.Destroy(); // Reattempt the connection for this socket
	}
}

//-----------------------------------------------------------------------------
// Purpose: returns whether the socket system is enabled and able to run
//-----------------------------------------------------------------------------
bool CWebSocket::IsInitialized() const
{
	return m_initialized;
}

//-----------------------------------------------------------------------------
// Purpose: connect to a socket
//-----------------------------------------------------------------------------
bool CWebSocket::ConnContext_s::Connect(const double queryTime, const ConnParams_s& params)
{
	if (state == CS_RETRY)
	{
		const double retryTimeTotal = lastQueryTime + params.retryTime;
		const double currTime = Plat_FloatTime();

		if (retryTimeTotal >= currTime)
			return false; // Still within retry period
	}

	tryCount++;
	webSocket = ProtoWebSocketCreate(params.bufSize);

	if (!webSocket)
	{
		state = CS_UNAVAIL;
		return false;
	}

	SetParams(params);

	const int32_t connectResult = ProtoWebSocketConnect(webSocket, address.String());
	if (connectResult != 0)
	{
		// Failure
		Warning(eDLL_T::COMMON, "WebSocketConnect failed for '%s': %d\n", address.String(), connectResult);
		Destroy();
		return false;
	}

	state = CS_CONNECTED;
	lastQueryTime = queryTime;

	return true;
}

//-----------------------------------------------------------------------------
// Purpose: check the connection status and destroy if not connected (-1)
//-----------------------------------------------------------------------------
bool CWebSocket::ConnContext_s::Process(const double queryTime)
{
	const int32_t status = ProtoWebSocketStatus(webSocket, 'stat', NULL, 0);

	if (status == -1)
	{
		int32_t failCode = ProtoWebSocketStatus(webSocket, 'fail', NULL, 0);

		const char* errorMsg = "UNKNOWN";
		switch (failCode)
		{
			case -1:  errorMsg = "DNS_FAILURE"; break;
			case -10: errorMsg = "TCP_CONNECTION_FAILURE"; break;
			case -20: errorMsg = "CERT_INVALID"; break;
			case -21: errorMsg = "CERT_HOST_MISMATCH"; break;
			case -22: errorMsg = "CERT_NOT_TRUSTED"; break;
			case -30: errorMsg = "SECURE_SETUP_FAILURE"; break;
			case -31: errorMsg = "SECURE_FAILURE"; break;
			case 0: errorMsg = "NO ERROR"; break;
		}

		ProtoSSLAlertDescT alertInfo{};
		ProtoWebSocketStatus(webSocket, 'alrt', &alertInfo, sizeof(alertInfo));

		Warning(eDLL_T::COMMON,
			"WebSocket '%s' failed: code=%d (%s), alert_type=%d, alert='%s', state=%s\n",
			address.String(), failCode, errorMsg, alertInfo.iAlertType,
			alertInfo.pAlertDesc ? alertInfo.pAlertDesc : "null", GetStateString(state));

		Destroy();
		lastQueryTime = queryTime;
		return false;
	}
	else if (!status)
	{
		lastQueryTime = queryTime;
		return false;  // Still handshaking
	}

	tryCount = 0;
	state = CS_LISTENING;

	return true;
}


//-----------------------------------------------------------------------------
// Purpose: set parameters for this socket
//-----------------------------------------------------------------------------
void CWebSocket::ConnContext_s::SetParams(const ConnParams_s& params) const
{
	Assert(webSocket, "Can't set parameters on a NULL instance!");

	if (params.timeOut > 0)
		ProtoWebSocketControl(webSocket, 'time', params.timeOut, 0, NULL);

	if (params.keepAlive > 0)
		ProtoWebSocketControl(webSocket, 'keep', params.keepAlive, 0, NULL);

	if (params.useTls)
	{
		const int32_t helloExtn =
			PROTOSSL_HELLOEXTN_SERVERNAME |
			PROTOSSL_HELLOEXTN_SIGALGS |
			PROTOSSL_HELLOEXTN_ALPN |
			PROTOSSL_HELLOEXTN_ELLIPTIC_CURVES;

		ProtoWebSocketControl(webSocket, 'extn', helloExtn, 0, NULL);
	}

	if (params.protocol > 0)
		ProtoWebSocketControl(webSocket, 'vers', params.protocol, 0, NULL);

	ProtoWebSocketControl(webSocket, 'ncrt', params.laxSSL, 0, NULL);
	ProtoWebSocketUpdate(webSocket);
}

//-----------------------------------------------------------------------------
// Purpose: disconnect and mark socket as unavailable for removal
//-----------------------------------------------------------------------------
void CWebSocket::ConnContext_s::Disconnect()
{
	if (webSocket)
	{
		ProtoWebSocketDisconnect(webSocket);
		ProtoWebSocketUpdate(webSocket);
		ProtoWebSocketDestroy(webSocket);

		webSocket = nullptr;
	}

	state = CS_UNAVAIL;
}

//-----------------------------------------------------------------------------
// Purpose: reconnect without burning retry attempts
//-----------------------------------------------------------------------------
void CWebSocket::ConnContext_s::Reconnect()
{
	Disconnect();
	state = CS_CREATE;
}

//-----------------------------------------------------------------------------
// Purpose: reconnect while burning retry attempts
//-----------------------------------------------------------------------------
void CWebSocket::ConnContext_s::Destroy()
{
	Disconnect();
	state = CS_DESTROYED;
}

int32_t CWebSocket::ReceiveData(char* outBuf, int32_t bufSize)
{
	if (!IsInitialized() || !outBuf || bufSize <= 0)
		return 0;

	for (ConnContext_s& conn : m_addressList)
	{
		if (conn.state != CS_LISTENING || !conn.webSocket)
			continue;

		const int32_t received = ProtoWebSocketRecv(conn.webSocket, outBuf, bufSize);
		if (received > 0)
			return received;
	}

	return 0;
}

//-----------------------------------------------------------------------------
// Purpose: disconnect a specific address only
//-----------------------------------------------------------------------------
bool CWebSocket::Disconnect(const char* address)
{
	if (!IsInitialized() || !address)
		return false;

	for (ConnContext_s& conn : m_addressList)
	{
		if (conn.address == address)
		{
			conn.Disconnect();
			return true;
		}
	}

	return false;
}


//-----------------------------------------------------------------------------
// Purpose: check if a specific address is actively listening for data
//-----------------------------------------------------------------------------
bool CWebSocket::IsListening(const char* address) const
{
	if (!address)
		return false;
	for (const ConnContext_s& conn : m_addressList)
	{
		if (conn.address == address && conn.state == CS_LISTENING && conn.webSocket)
			return true;
	}
	return false;
}

//-----------------------------------------------------------------------------
// Purpose: check if a specific connection is established (handshaking or listening)
//-----------------------------------------------------------------------------
bool CWebSocket::IsConnected(const char* address) const
{
	if (!address)
		return false;
	for (const ConnContext_s& conn : m_addressList)
	{
		if (conn.address == address &&
			(conn.state == CS_CONNECTED || conn.state == CS_LISTENING) &&
			conn.webSocket)
			return true;
	}
	return false;
}

//-----------------------------------------------------------------------------
// Purpose: check if a specific connection is in any non-failed state
//-----------------------------------------------------------------------------
bool CWebSocket::IsActive(const char* address) const
{
	if (!address)
		return false;
	for (const ConnContext_s& conn : m_addressList)
	{
		if (conn.address == address &&
			conn.state != CS_DESTROYED &&
			conn.state != CS_UNAVAIL &&
			conn.webSocket)
			return true;
	}
	return false;
}


CWebSocket::ConnState_e CWebSocket::GetState(const char* address) const
{
	if (!address)
		return CS_UNAVAIL;
	for (const ConnContext_s& conn : m_addressList)
	{
		if ( conn.address == address )
			return conn.state;
	}

	return CS_UNAVAIL; // Not found
}

const char* CWebSocket::GetStateString(const ConnState_e state) const
{
	switch (state)
	{
		case CS_CREATE:    return "CS_CREATE";
		case CS_CONNECTED: return "CS_CONNECTED";
		case CS_LISTENING: return "CS_LISTENING";
		case CS_DESTROYED: return "CS_DESTROYED";
		case CS_RETRY:     return "CS_RETRY";
		case CS_UNAVAIL:   return "CS_UNAVAIL";
		default:           return "UNKNOWN_STATE";
	}
}

const char* CWebSocket::ConnContext_s::GetStateString(const ConnState_e contextState) const
{
	switch (contextState)
	{
		case CS_CREATE:    return "CS_CREATE";
		case CS_CONNECTED: return "CS_CONNECTED";
		case CS_LISTENING: return "CS_LISTENING";
		case CS_DESTROYED: return "CS_DESTROYED";
		case CS_RETRY:     return "CS_RETRY";
		case CS_UNAVAIL:   return "CS_UNAVAIL";
		default:           return "UNKNOWN_STATE";
	}
}

int32_t CWebSocket::SetCaCert(uint8_t* pBuf, int32_t nRead)
{
	if (!pBuf || nRead <= 0)
		return -1;

	return ProtoSSLSetCACert(pBuf, nRead);
}
