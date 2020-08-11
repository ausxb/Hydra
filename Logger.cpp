#include "Logger.h"

void Hydra::Logger::init()
{
	InitializeCriticalSection(&stream_lock);
}

void Hydra::Logger::deinit()
{
	DeleteCriticalSection(&stream_lock);
}

void Hydra::Logger::dbg(const std::string &msg)
{
	EnterCriticalSection(&stream_lock);
	std::cout << "[DEBUG] " << msg << std::endl;
	LeaveCriticalSection(&stream_lock);
}

void Hydra::Logger::info(const std::string &msg)
{
	EnterCriticalSection(&stream_lock);
	std::cout << "[INFO] " << msg << std::endl;
	LeaveCriticalSection(&stream_lock);
}

void Hydra::Logger::warn(const std::string &msg)
{
	EnterCriticalSection(&stream_lock);
	std::cout << "[WARNING] " << msg << std::endl;
	LeaveCriticalSection(&stream_lock);
}

void Hydra::Logger::err(const std::string &msg)
{
	EnterCriticalSection(&stream_lock);
	std::cout << "[ERROR] " << msg << std::endl;
	LeaveCriticalSection(&stream_lock);
}

std::string Hydra::Logger::addrString(const LPSOCKADDR addr)
{
	struct sockaddr_in tmp;
	CopyMemory(&tmp, addr, sizeof(struct sockaddr_in));
	CHAR str[17];
	str[0] = '\0';
	InetNtop(AF_INET, &tmp.sin_addr, str, 17);

	return std::string{ str };
}

CRITICAL_SECTION Hydra::Logger::stream_lock{};