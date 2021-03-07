#include "Logger.h"

#include <iostream>
#include <sstream>

Hydra::FileLogger::FileLogger(const std::string &path) :
	logfile{ path }
{

}

void Hydra::FileLogger::dbg(const std::string &msg)
{
	std::cout << "[DEBUG] " << msg << std::endl;
}

void Hydra::FileLogger::info(const std::string &msg)
{
	std::cout << "[INFO] " << msg << std::endl;
}

void Hydra::FileLogger::warn(const std::string &msg)
{
	std::cout << "[WARNING] " << msg << std::endl;
}

void Hydra::FileLogger::err(const std::string &msg)
{
	std::cout << "[ERROR] " << msg << std::endl;
}

Hydra::ConsoleLogger::ConsoleLogger()
{
	InitializeCriticalSection(&stream_lock);
}

Hydra::ConsoleLogger::~ConsoleLogger()
{
	DeleteCriticalSection(&stream_lock);
}

void Hydra::ConsoleLogger::dbg(const std::string &msg)
{
	EnterCriticalSection(&stream_lock);
	std::cout << "[DEBUG] " << msg << std::endl;
	LeaveCriticalSection(&stream_lock);
}

void Hydra::ConsoleLogger::info(const std::string &msg)
{
	EnterCriticalSection(&stream_lock);
	std::cout << "[INFO] " << msg << std::endl;
	LeaveCriticalSection(&stream_lock);
}

void Hydra::ConsoleLogger::warn(const std::string &msg)
{
	EnterCriticalSection(&stream_lock);
	std::cout << "[WARNING] " << msg << std::endl;
	LeaveCriticalSection(&stream_lock);
}

void Hydra::ConsoleLogger::err(const std::string &msg)
{
	EnterCriticalSection(&stream_lock);
	std::cout << "[ERROR] " << msg << std::endl;
	LeaveCriticalSection(&stream_lock);
}

std::string Hydra::addrString(const LPSOCKADDR addr)
{
	const LPSOCKADDR_IN _addr =
		reinterpret_cast<LPSOCKADDR_IN>(addr);
	std::ostringstream fmt;
	CHAR str[17];

	InetNtop(AF_INET, &_addr->sin_addr, str, 17);

	fmt << str << ':' << ntohs(_addr->sin_port);

	return fmt.str();
}

Hydra::ConsoleLogger globalLog { };
