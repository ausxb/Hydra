#pragma once

#include <string>
#include <iostream>

#include <Winsock2.h>
#include <Ws2tcpip.h>

namespace Hydra
{
	class Logger
	{
	public:
		static void init();
		static void deinit();
		static void dbg(const std::string& msg);
		static void info(const std::string& msg);
		static void warn(const std::string &msg);
		static void err(const std::string &msg);
		static std::string addrString(const LPSOCKADDR addr);
	private:
		static CRITICAL_SECTION stream_lock;
	};
}