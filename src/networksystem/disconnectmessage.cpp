//=============================================================================//
//
// Purpose: Builds legacy-client-safe, server-originated disconnect messages.
//
//=============================================================================//

#include "core/stdafx.h"
#include <string>
#include "tier1/strtools.h"
#include "networksystem/disconnectmessage.h"

static constexpr size_t DISCONNECT_MESSAGE_FIELD_MAX_LENGTH = 64;

bool DisconnectMessage_BuildSafeUtf8Reason(const std::string& reason, std::string& outReason)
{
	outReason.clear();
	if (reason.empty() || reason.length() > DISCONNECT_MESSAGE_MAX_LENGTH ||
		reason.front() == '#' || !V_IsValidUTF8(reason.c_str()))
	{
		return false;
	}

	for (const unsigned char ch : reason)
	{
		if (ch < 0x20 || ch == 0x7f)
			return false;
	}

	outReason = reason;
	return true;
}

static bool DisconnectMessage_IsReasonCodeChar(const unsigned char ch)
{
	return (ch >= 'A' && ch <= 'Z') ||
		(ch >= 'a' && ch <= 'z') ||
		(ch >= '0' && ch <= '9') ||
		ch == '_' || ch == '-';
}

static bool DisconnectMessage_NormalizeReasonCode(const std::string& value, std::string& outValue)
{
	outValue.clear();
	if (value.length() > DISCONNECT_MESSAGE_FIELD_MAX_LENGTH)
		return false;

	for (const unsigned char ch : value)
	{
		if (!DisconnectMessage_IsReasonCodeChar(ch))
			return false;
		outValue.push_back(static_cast<char>(ch));
	}

	return true;
}

static bool DisconnectMessage_NormalizeIP(const std::string& value, std::string& outValue)
{
	outValue.clear();
	if (value.length() > DISCONNECT_MESSAGE_FIELD_MAX_LENGTH)
		return false;

	for (const unsigned char ch : value)
	{
		const bool bValid = (ch >= 'A' && ch <= 'F') ||
			(ch >= 'a' && ch <= 'f') ||
			(ch >= '0' && ch <= '9') ||
			ch == '.' || ch == ':' || ch == '-';
		if (!bValid)
			return false;
		outValue.push_back(static_cast<char>(ch));
	}

	return true;
}

static bool DisconnectMessage_NormalizeProcessedAt(const std::string& value, std::string& outValue)
{
	outValue.clear();
	if (value.length() > DISCONNECT_MESSAGE_FIELD_MAX_LENGTH)
		return false;

	for (const unsigned char ch : value)
	{
		if (ch == ' ')
		{
			outValue.push_back('T');
			continue;
		}
		if (ch == ':')
		{
			outValue.push_back('-');
			continue;
		}

		const bool bValid = (ch >= '0' && ch <= '9') ||
			ch == '-' || ch == 'T' || ch == 'Z' || ch == '+' || ch == '.';
		if (!bValid)
			return false;
		outValue.push_back(static_cast<char>(ch));
	}

	return true;
}

static bool DisconnectMessage_ReplacePlaceholder(std::string& text,
	const char* const pszPlaceholder, const std::string& replacement)
{
	size_t position = text.find(pszPlaceholder);
	if (position == std::string::npos)
		return true;
	if (replacement.empty())
		return false;

	const size_t nPlaceholderLength = strlen(pszPlaceholder);
	do
	{
		text.replace(position, nPlaceholderLength, replacement);
		position = text.find(pszPlaceholder, position + replacement.length());
	} while (position != std::string::npos);

	return true;
}

static bool DisconnectMessage_IsSafeOutput(const std::string& value)
{
	if (value.empty() || value.length() > DISCONNECT_MESSAGE_MAX_LENGTH || value.front() == '#')
		return false;

	for (const unsigned char ch : value)
	{
		if (ch <= 0x20 || ch >= 0x7f || ch == '|' || ch == '%' || ch == '{' || ch == '}')
			return false;
	}

	return true;
}

bool DisconnectMessage_BuildAsciiReason(const DisconnectMessage_t& message,
	const char* const pszReasonTemplate, std::string& outReason)
{
	outReason.clear();
	if (!VALID_CHARSTAR(pszReasonTemplate))
		return false;

	std::string svReasonCode;
	std::string svIPAddr;
	std::string svProcessedAt;
	if (!DisconnectMessage_NormalizeReasonCode(message.m_svReasonCode, svReasonCode) ||
		!DisconnectMessage_NormalizeIP(message.m_svIPAddr, svIPAddr) ||
		!DisconnectMessage_NormalizeProcessedAt(message.m_svProcessedAt, svProcessedAt))
	{
		return false;
	}

	outReason = pszReasonTemplate;
	if (!DisconnectMessage_ReplacePlaceholder(outReason, "{reason_code}", svReasonCode) ||
		!DisconnectMessage_ReplacePlaceholder(outReason, "{ip}", svIPAddr) ||
		!DisconnectMessage_ReplacePlaceholder(outReason, "{processed_at}", svProcessedAt) ||
		!DisconnectMessage_IsSafeOutput(outReason))
	{
		outReason.clear();
		return false;
	}

	return true;
}
