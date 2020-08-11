#pragma once

#include <WinSock2.h>
#include <MSWSock.h>
#include <Windows.h>
#include <stdexcept>
#include <sstream>

namespace Hydra
{
	int loadAcceptEx(SOCKET listening, LPFN_ACCEPTEX* ppfnAcceptEx);
	
	template<class E>
	[[noreturn]] void throwWithError(LPCSTR msg, int err)
	{
		std::ostringstream format;

		format << '(' << err << ')' << ' ' << msg;

		throw E{ format.str(), err };
	}

	class ServerException : public std::runtime_error
	{
	public:
		explicit ServerException(const char* msg, int err);
		explicit ServerException(const std::string& msg, int err);
		int error_code;
	};

	class ConnectionException : public std::runtime_error
	{
	public:
		explicit ConnectionException(const char* msg, int err);
		explicit ConnectionException(const std::string& msg, int err);
		int error_code;
	};
}