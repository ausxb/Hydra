#pragma once

#include <string>
#include <fstream>

#include <Winsock2.h>
#include <Ws2tcpip.h>

namespace Hydra
{
	class FileLogger
	{
	public:
		FileLogger(const std::string &path);
		void dbg(const std::string &msg);
		void info(const std::string &msg);
		void warn(const std::string &msg);
		void err(const std::string &msg);
	private:
		std::ofstream logfile;
	};

	class ConsoleLogger
	{
	public:
		ConsoleLogger();
		~ConsoleLogger();
		void dbg(const std::string &msg);
		void info(const std::string &msg);
		void warn(const std::string &msg);
		void err(const std::string &msg);
	private:
		CRITICAL_SECTION stream_lock;
	};

	std::string addrString(const LPSOCKADDR addr);

	static ConsoleLogger globalLog;
}
