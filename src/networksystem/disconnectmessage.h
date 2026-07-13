#pragma once

#include <cstddef>
#include <string>

static constexpr size_t DISCONNECT_MESSAGE_MAX_LENGTH = 192;

struct DisconnectMessage_t
{
	std::string m_svReasonCode;
	std::string m_svAction;
	std::string m_svIPAddr;
	std::string m_svProcessedAt;
};

bool DisconnectMessage_BuildAsciiReason(const DisconnectMessage_t& message,
	const char* pszReasonTemplate, std::string& outReason);
bool DisconnectMessage_BuildSafeUtf8Reason(const std::string& reason,
	std::string& outReason);
