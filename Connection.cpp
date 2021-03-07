#include "Connection.h"

#include "Packet.h"
#include "TCP.h"
#include "util.h"

Hydra::Connection::Connection() :
	mSocket{ INVALID_SOCKET }, mAddr{ 0 }, port{ nullptr }, outstanding{ 0 }
{

}

int Hydra::Connection::receive(Packet* packet)
{
	DWORD flags = 0;

	if (this->port)
		packet->op = Packet::Operation::server_read;
	else
		packet->op = Packet::Operation::client_read;

	inMutex.lock();
	DWORD status = static_cast<DWORD>(WSARecv(
		mSocket,
		&packet->wsa,
		1,
		NULL,
		&flags,
		reinterpret_cast<LPWSAOVERLAPPED>(packet),
		NULL
	));
	inMutex.unlock();

	if (status != 0)
	{
		status = WSAGetLastError();
		if (status != WSA_IO_PENDING)
		{
			throw SynchronousException{
				"Connection::receive(): failed synchronously", status
			};
		}
	}

	outstanding.fetch_add(1, std::memory_order_relaxed);

	return status;
}

int Hydra::Connection::send(Packet* packet)
{
	DWORD flags = 0; //No flags

	if (this->port)
		packet->op = Packet::Operation::server_write;
	else
		packet->op = Packet::Operation::client_write;

	outMutex.lock();
	DWORD status = static_cast<DWORD>(WSASend(
		mSocket,
		&packet->wsa,
		1,
		NULL,
		flags,
		reinterpret_cast<LPWSAOVERLAPPED>(packet),
		NULL
	));
	outMutex.unlock();

	if (status != 0)
	{
		status = WSAGetLastError();
		if (status != WSA_IO_PENDING)
		{
			throw SynchronousException{
				"Connection::send(): failed synchronously", status
			};
		}
	}

	outstanding.fetch_add(1, std::memory_order_relaxed);

	return status;
}

void Hydra::Connection::terminate(Packet *packet)
{
	if (port)
		packet->op = Packet::Operation::server_close;
	else
		packet->op = Packet::Operation::client_disconnect;

	PostQueuedCompletionStatus(
		this->port->hCompPort,
		0,
		reinterpret_cast<ULONG_PTR>(this),
		reinterpret_cast<LPOVERLAPPED>(packet)
	);

	outstanding.fetch_add(1, std::memory_order_relaxed);
}

/* This should be changed to use TransmitFile() with TF_DISCONNECT | TF_REUSE_SOCKET
   and to preserve which sockets were created with AcceptEx vs ConnectEx */
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
