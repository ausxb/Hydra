#include "Connection.h"

Hydra::Connection::Connection(SOCKET accepted) :
	mSocket{ accepted }, mAddr{ 0 }, mOutstanding{ 0 }
{
	
}

int Hydra::Connection::receive(Packet* packet)
{
	DWORD flags = 0;
	packet->op = Packet::READ;

	mInMutex.lock();
	int status = WSARecv(mSocket, &packet->wsabuf, 1, NULL, &flags, reinterpret_cast<LPWSAOVERLAPPED>(packet), NULL);
	mInMutex.unlock();

	if (status == SOCKET_ERROR)
	{
		status = WSAGetLastError();
		if (status != WSA_IO_PENDING) return status;
		
		//throwWithError<ConnectionException>("Connection::receive(): failed synchronously", status);
	}

	++mOutstanding;

	return status;
}

int Hydra::Connection::send(Packet* packet)
{
	DWORD flags = 0; //No flags
	packet->op = Packet::WRITE;

	mOutMutex.lock();
	int status = WSASend(mSocket, &packet->wsabuf, 1, NULL, flags, reinterpret_cast<LPWSAOVERLAPPED>(packet), NULL);
	mOutMutex.unlock();

	if (status == SOCKET_ERROR)
	{
		status = WSAGetLastError();
		if (status != WSA_IO_PENDING) return status;
		
		//throwWithError<ConnectionException>("Connection::send(): failed synchronously", status);
	}

	++mOutstanding;

	return status;
}

int Hydra::Connection::sendDisconnect()
{
	return WSASendDisconnect(mSocket, NULL);
}

int Hydra::Connection::sendDisconnect(Packet* packet)
{
	int status = receive(packet);
	
	if (status == 0 || status == WSA_IO_PENDING)
		return WSASendDisconnect(mSocket, NULL);
	else
		return status;
}

const LPSOCKADDR Hydra::Connection::sockaddr()
{
	return &mAddr;
}