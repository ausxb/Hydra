#include "TCP.h"

#include "Connection.h"
#include "Packet.h"
#include "Logger.h"
#include "util.h"

Hydra::TCP::TCP(unsigned initWorkers, unsigned long shutdownTimeoutMs) :
	fnAcceptEx{ nullptr },
	connection_cache{ sizeof(Connection), alignof(Connection), 1 },
	hCompPort{ INVALID_HANDLE_VALUE },
	hWorkers{ initWorkers },
	dwBarrierCount{ 0 }
{
	_ASSERT_EXPR(initWorkers > 0, L"TCP::TCP(): There must be at least a single thread");

	setupCompletionPort();
	launchWorkers();
}

Hydra::TCP::~TCP()
{
	shutdown();
	terminateWorkers();
	cleanupCompletionPort();
}

void Hydra::TCP::shutdown()
{
	globalLog.dbg("Shutdown phase 1");

	/* Phase 1: forcibly close all sockets */
	for (OpenPort &port : mOpenPorts)
	{
		port.closing = true;

		closesocket(port.mSocket);

		std::unique_lock<std::mutex> lock{ port.controlMutex };

		std::list<Connection*>::iterator iter = port.connections.begin();
		std::list<Connection*>::iterator fwd;

		while (iter != port.connections.end())
		{
			fwd = std::next(iter);
			closesocket((*iter)->mSocket);
			/* See comment at this point in the code in TCP::close() below */
			if ((*iter)->outstanding.load(std::memory_order_relaxed) == 0)
			{
				Connection *term = *iter;
				term->~Connection();
				connection_cache.dealloc(term);
				port.connections.erase(iter);
			}
			iter = fwd;
		}
	}

	globalLog.dbg("Shutdown phase 2");

	/* Phase 2: wait for active connections to release their resources */
	for (OpenPort &port : mOpenPorts)
	{
		std::unique_lock<std::mutex> lock{ port.controlMutex };
		while (!port.connections.empty())
		{
			port.connectionsClosedCond.wait(lock);
		}
	}

	globalLog.dbg("Shutdown phase 3");

	/* Phase 3: wait for outstanding accepts to be cancelled */
	for (OpenPort &port : mOpenPorts)
	{
		std::unique_lock<std::mutex> lock{ port.controlMutex };
		while (port.acceptCount.load(std::memory_order_relaxed) > 0)
		{
			port.acceptsCancelledCond.wait(lock);
		}
	}

	globalLog.dbg("Shutdown phase 4");

	/* Phase 4: release resources */
	mOpenPorts.clear();
}

Hydra::TCP::OpenPort& Hydra::TCP::listen(PCSTR bind, PCSTR port, UINT backlog)
{
	_ASSERT_EXPR(backlog > 0, "TCP::listen(): The backlog size must be greater than zero");

	SOCKET s = acquireSocket(true, bind, port, backlog);

	// OpenPort *port = new OpenPort{ s };
	mOpenPorts.emplace_front(s, hCompPort);
	mOpenPorts.front().linkage = mOpenPorts.begin();
	return mOpenPorts.front();
}

void Hydra::TCP::close(OpenPort &port)
{
	port.closing = true;

	closesocket(port.mSocket);

	std::unique_lock<std::mutex> lock{ port.controlMutex };

	std::list<Connection*>::iterator iter = port.connections.begin();
	std::list<Connection*>::iterator fwd;

	/* Phase 1: forcibly close all sockets */
	while (iter != port.connections.end())
	{
		fwd = std::next(iter);
		closesocket((*iter)->mSocket);
		/* This check is used to avoid deleting a connection twice. Once closing is set,
           worker threads will delete clients when the last completion arrives and before
		   decrementing their outstanding operation counter, so the client isn't deleted
		   below. Zero will be observed only if the application has not submitted new work,
		   meaning the connection is idle (even if the remote end is sending data) since
		   no completion packets will be queued. Relaxed semantics are sufficient for this. */
		if ((*iter)->outstanding.load(std::memory_order_relaxed) == 0)
		{
			Connection *term = *iter;
			term->~Connection();
			connection_cache.dealloc(term);
			port.connections.erase(iter);
		}
		iter = fwd;
	}

	/* Phase 2: wait for active connections to release their resources */
	while (!port.connections.empty())
	{
		port.connectionsClosedCond.wait(lock);
	}

	/* Phase 3: wait for outstanding accepts to be cancelled */
	while (port.acceptCount.load(std::memory_order_relaxed) > 0)
	{
		port.acceptsCancelledCond.wait(lock);
	}

	/* Phase 4: now that all dependencies are cleaned up, destroy the port */
	mOpenPorts.erase(port.linkage);
}

/*
Hydra::Connection& Hydra::TCP::connect(PCSTR addr, PCSTR port)
{

}

void Hydra::TCP::disconnect(Connection &connection)
{

}
*/

void Hydra::TCP::workerLoop(unsigned id)
{
	Connection* remote = nullptr;
	Packet* io = nullptr;
	DWORD bytes = 0;
	DWORD timeout = INFINITE;

	globalLog.dbg("Thread " + std::to_string(id) + " startup");

	while(true)
	{
		BOOL status = GetQueuedCompletionStatus(
			hCompPort,
			&bytes,
			reinterpret_cast<PULONG_PTR>(&remote),
			reinterpret_cast<LPOVERLAPPED*>(&io),
			timeout
		);

		bool shutdown_aware_decrement = false;

		if (status && io == &terminate_packet) /* Terminate thread */
		{
			break;
		}
		else if (!status) /* Handle async error */
		{
			int wsa_error = WSAGetLastError();
			Packet::Operation op_err = Packet::Operation::internal_exception;

			/* If lpOverlapped is NULL, dereferencing it is a bad idea */
			if (io) op_err = io->op;

			switch (op_err)
			{
			case Packet::Operation::server_read:
			case Packet::Operation::server_write:
			{
				async_error(*remote->port, remote, io, wsa_error);

				// remote->outstanding.fetch_sub(1, std::memory_order_relaxed);
				shutdown_aware_decrement = true;

				break;
			}

			case Packet::Operation::server_accept:
			{
				remote = static_cast<Connection*>(io->internal);

				closesocket(remote->mSocket);

				/* If accepting on a socket fails, there is no valid connection,
				   indicated by nullptr */
				async_error(*remote->port, nullptr, io, wsa_error);

				/* Atomic decrement, and if last, notify the port in case it is waiting */
				if (remote->port->acceptCount
						.fetch_sub(1, std::memory_order_relaxed) == 1)
				{
					remote->port->acceptsCancelledCond.notify_one();
				}

				remote->~Connection();
				connection_cache.dealloc(remote);

				break;
			}

			case Packet::Operation::server_close:
			case Packet::Operation::server_halved:
			{
				/* The close operation and send side shutdown notification are not
				   asynchronous network operations. They are queued to the IOCP by the
				   application, so they are always delivered with a successful status. */
				_ASSERT_EXPR(FALSE, L"TCP::workerLoop(): Asynchronous errors are not supposed "
			        L"to be possible on SERVER_CLOSE or SERVER_HALVED packets");
				// async_error(remote, io, wsa_error);

				break;
			}

			case Packet::Operation::client_read:
			case Packet::Operation::client_write:
			case Packet::Operation::client_connect:
			case Packet::Operation::client_disconnect:
			case Packet::Operation::client_close:
			case Packet::Operation::client_halved:
				_ASSERT_EXPR(FALSE, L"TCP::workerLoop(): CLIENT_* operations are not yet implemented");
				break;

			default:
				/* wsa_error should indicate a dequeue timeout */
				async_error(*remote->port, nullptr, nullptr, wsa_error);

				break;
			}
		}
		else /* Handle network event */
		{
			switch (io->op)
			{
			case Packet::Operation::server_read:
			{
				/* If the client send side was shutdown, don't update outstanding,
				   and requeue packet for SERVER_HALVED notification */
				if (bytes == 0)
				{
					globalLog.dbg("Received 0 bytes on client socket");

					io->op = Packet::Operation::server_halved;
					PostQueuedCompletionStatus(
						hCompPort,
						0,
						reinterpret_cast<ULONG_PTR>(remote),
						reinterpret_cast<LPOVERLAPPED>(io)
					);
				}
				else
				{
					server_read(*remote->port, remote, io, bytes);

					// remote->outstanding.fetch_sub(1, std::memory_order_relaxed);
					shutdown_aware_decrement = true;
				}

				break;
			}

			case Packet::Operation::server_write:
			{
				server_write(*remote->port, remote, io, bytes);

				// remote->outstanding.fetch_sub(1, std::memory_order_relaxed);
				shutdown_aware_decrement = true;

				break;
			}

			case Packet::Operation::server_accept:
			{
				DWORD opt = TRUE;
				int namelen = sizeof(SOCKADDR);
				bool discard = false;

				remote = static_cast<Connection*>(io->internal);
				/* In case there is a backlog of completed accepts at the time a port
				   begins shutdown, we want to skip setsockopt and go directly to
				   deleting the connection. Once inside the lock, we must check again
				   whether the port is closing since shutdown may have been initiated
				   between the first check and acquiring the lock. */
				if (remote->port->closing)
				{
					discard = true;
				}
				else
				{
					int result = setsockopt(
						remote->mSocket,
						SOL_SOCKET,
						SO_UPDATE_ACCEPT_CONTEXT,
						reinterpret_cast<char*>(&remote->mSocket),
						sizeof(SOCKET)
					);

					if (result == SOCKET_ERROR)
					{
						result = WSAGetLastError();
						async_error(*remote->port, nullptr, io, result);

						discard = true;
					}
					else
					{
						remote->port->controlMutex.lock();
						if (remote->port->closing)
						{
							discard = true;
						}
						else
						{
							remote->linkage = remote->port->connections.insert(
								remote->port->connections.end(),
								remote
							);
						}
						remote->port->controlMutex.unlock();
					}
				}

				if (discard)
				{
					closesocket(remote->mSocket);
					remote->~Connection();
					connection_cache.dealloc(remote);
				}
				else
				{
					getpeername(remote->mSocket, &remote->mAddr, &namelen);
					server_accept(*remote->port, remote, io, 0);
				}

				/* Atomic decrement, and if last, notify the port in case it is waiting */
				if (remote->port->acceptCount
						.fetch_sub(1, std::memory_order_relaxed) == 1)
				{
					remote->port->acceptsCancelledCond.notify_one();
				}

				break;
			}

			case Packet::Operation::server_close:
			{
				bool notify = false;
				bool cleanup = false;

				/* Execute callback while we still have a valid client */
				server_close(*remote->port, remote, io, 0);

				globalLog.dbg("Closing socket and clearing connection");

				remote->port->controlMutex.lock();
				/* No outstanding operations, remove from list now */
				if (remote->outstanding.load(std::memory_order_relaxed) == 1)
				{
					cleanup = true;
					remote->port->connections.erase(remote->linkage);
					if (remote->port->connections.empty())
						notify = true;
				}

				/* Initiate cancellation of outstanding operations. If closing is already set
				   then closesocket has already been called so we don't want to call it twice. */
				if (!remote->port->closing)
				{
					WSASendDisconnect(remote->mSocket, NULL);
					closesocket(remote->mSocket);
				}
				remote->port->controlMutex.unlock();

				if (cleanup)
				{
					if (notify)
						remote->port->connectionsClosedCond.notify_one();

					remote->~Connection();
					connection_cache.dealloc(remote);
				}

				break;
			}

			case Packet::Operation::server_halved:
			{
				server_halved(*remote->port, remote, io, 0);

				remote->outstanding.fetch_sub(1, std::memory_order_relaxed);

				break;
			}
			
			case Packet::Operation::client_read:
			case Packet::Operation::client_write:
			case Packet::Operation::client_connect:
			case Packet::Operation::client_disconnect:
			case Packet::Operation::client_close:
			case Packet::Operation::client_halved:
				_ASSERT_EXPR(FALSE, L"TCP::workerLoop(): CLIENT_* operations are not yet implemented");
				break;

			case Packet::Operation::internal_barrier:
			{
				/* Synchronization barrier */
				bool last = false;
				mNotifyMutex.lock();
				if(++dwBarrierCount == hWorkers.size())
					last = true;
				mNotifyMutex.unlock();

				if(last) mNotifyCV.notify_one();

				/* Wait for controller to resume workers */
				std::unique_lock<std::mutex> sync{ mBarrierMutex };

				break;
			}

			default:
				_ASSERT_EXPR(FALSE, L"TCP::workerLoop(): This machine is on fire");
				break;
			}
		}

		if (shutdown_aware_decrement)
		{
			/* It is necessary to check the number of outstanding operations before
			   decrementing. This ensures that if a port is being closed concurrently,
			   the connection will not be deleted twice concurrently because the number
			   of outstanding operations remains non-zero while the connection is being
			   destroyed in the if branch. Short circuiting the '&&' makes this a
			   relaxed-only operation in most cases. */
			if (remote->outstanding.load(std::memory_order_relaxed) == 1
				&& remote->port->closing == true)
			{
				bool notify = false;

				remote->port->controlMutex.lock();
				remote->port->connections.erase(remote->linkage);
				if (remote->port->connections.empty())
					notify = true;
				remote->port->controlMutex.unlock();

				if (notify)
					remote->port->connectionsClosedCond.notify_one();

				remote->~Connection();
				connection_cache.dealloc(remote);
			}
			else
			{
				remote->outstanding.fetch_sub(1, std::memory_order_relaxed);
			}
		}
	}
}

SOCKET Hydra::TCP::acquireSocket(bool server, PCSTR addr, PCSTR port, UINT backlog)
{
	SOCKET socket;
	LPFN_ACCEPTEX pAcceptEx;
	struct addrinfo req;
	struct addrinfo *result = nullptr;
	DWORD status = 0;
	std::string msg;

	ZeroMemory(&req, sizeof(struct addrinfo));
	req.ai_family = AF_INET;
	req.ai_socktype = SOCK_STREAM;
	req.ai_protocol = IPPROTO_TCP;

	if (server)
	{
		req.ai_flags = AI_PASSIVE;
	}

	status = getaddrinfo(addr, port, &req, &result);
	if (status != 0)
	{
		throw SynchronousException{ "TCP::acquireSocket(): getaddrinfo() failed", status };
	}

	socket = WSASocket(
		result->ai_family,
		result->ai_socktype,
		result->ai_protocol,
		NULL,
		0,
		WSA_FLAG_OVERLAPPED
	);

	if (socket == INVALID_SOCKET)
	{
		msg = "TCP::acquireSocket(): WSASocket() failed";
		goto socket_acquire_fail;
	}

	/* Do not allow any other application to use the server's address and port. */
	BOOL set_exclusive = TRUE;
	if (setsockopt(socket,
		SOL_SOCKET,
		SO_EXCLUSIVEADDRUSE,
		(char*) &set_exclusive,
		sizeof(BOOL)) == SOCKET_ERROR)
	{
		msg = "TCP::acquireSocket(): unable to set SO_EXCLUSIVEADDRUSE";
		goto socket_acquire_fail;
	}

	/* Use packet buffer on heap directly.
		Buffers must remain valid until completion. */
	DWORD set_size = 0;
	if (setsockopt(socket,
		SOL_SOCKET,
		SO_SNDBUF,
		(char*) &set_size,
		sizeof(DWORD)) == SOCKET_ERROR)
	{
		msg = "TCP::acquireSocket(): unable to set SO_SNDBUF to 0";
		goto socket_acquire_fail;
	}

	if (setsockopt(socket,
		SOL_SOCKET,
		SO_RCVBUF,
		(char*) &set_size,
		sizeof(DWORD)) == SOCKET_ERROR)
	{
		msg = "TCP::acquireSocket(): unable to set SO_RCVBUF to 0";
		goto socket_acquire_fail;
	}

	if (server && bind(socket, result->ai_addr, result->ai_addrlen) == SOCKET_ERROR)
	{
		msg = "TCP::acquireSocket(): bind() failed";
		goto socket_acquire_fail;
	}

	/*
		Big todo right here. When acting as a client, good design dictates we attempt
		multiple interfaces returned by getaddrinfo() until we get a successful connection
		or run out of addrinfo's. So we can't simply return a socket, and need a way
		to persist the addinfo data so ConnectEx can fail asynchronously and be attempted
		on the next interface in the worker loop.
	*/

	freeaddrinfo(result);
	result = nullptr;

	if (server && ::listen(socket, backlog) == SOCKET_ERROR)
	{
		msg = "TCP::acquireSocket(): listen() failed";
		goto socket_acquire_fail;
	}

	if (!CreateIoCompletionPort(reinterpret_cast<HANDLE>(socket), hCompPort, 0, 0))
	{
		msg = "TCP::acquireSocket(): failed to associate socket with completion port";
		goto socket_acquire_fail;
	}

	if (server)
	{
		if (loadAcceptEx(socket, &pAcceptEx) == SOCKET_ERROR)
		{
			msg = "TCP::acquireSocket(): failed to acquire function pointer to AcceptEx";
			goto socket_acquire_fail;
		}
		else if(fnAcceptEx != nullptr && pAcceptEx != fnAcceptEx)
		{
			msg = "TCP::acquireSocket(): new pointer to AcceptEx differs from existing pointer";
			goto socket_acquire_fail;
		}
		else if(fnAcceptEx == NULL)
		{
			fnAcceptEx = pAcceptEx;
		}
	}

	return socket;

socket_acquire_fail:
	status = WSAGetLastError();

	if (result != nullptr) freeaddrinfo(result);
	if (socket != INVALID_SOCKET)
	{
		closesocket(socket);
		socket = INVALID_SOCKET;
	}
	throw SynchronousException{ msg, status };
}

void Hydra::TCP::setupCompletionPort()
{
	hCompPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (!hCompPort)
	{
		throw SynchronousException{
			"TCP::setupCompletionPort(): unable to create a new completion port",
			GetLastError()
		};
	}
}

void Hydra::TCP::cleanupCompletionPort()
{
	CloseHandle(hCompPort);
}

void Hydra::TCP::launchWorkers()
{
	unsigned id = 0;
	for (std::thread& t : hWorkers)
	{
		t = std::thread{ &TCP::workerLoop, this, id++ };
	}
}

void Hydra::TCP::terminateWorkers()
{
	globalLog.dbg("Terminating threads");

	for (auto i = 0; i < hWorkers.size(); i++)
	{
		PostQueuedCompletionStatus(hCompPort,0, 0,
			reinterpret_cast<LPOVERLAPPED>(&terminate_packet));
	}

	for (std::thread& t : hWorkers)
	{
		t.join();
	}
}

/* Since multiple threads will dequeue the same packet/address, broadcast must be
   an internal message that is safe for multiple threads to run */
void Hydra::TCP::synchronizeWorkers(Packet *broadcast)
{
	globalLog.dbg("Initiating synchronization barrier");

	mBarrierMutex.lock();

	dwBarrierCount = 0;

	/* If broadcast is not NULL, we broadcast it to workers */
	for (auto i = 0; broadcast && i < hWorkers.size(); i++)
	{
		PostQueuedCompletionStatus(hCompPort, 0, 0,
			reinterpret_cast<LPOVERLAPPED>(broadcast));
	}

	std::unique_lock<std::mutex> sync{ mNotifyMutex };

	/* Make sure all workers haven't yielded before sleeping. */
	if (dwBarrierCount != hWorkers.size())
		mNotifyCV.wait(sync);

	/* At this point, all workers are waiting on mBarrierMutex.
		mNotifyMutex will be released on return. */
}

void Hydra::TCP::resumeWorkers()
{
	mBarrierMutex.unlock();
}

void Hydra::TCP::postAcceptSocket(OpenPort &port, Packet *packet)
{
	SOCKET newsock = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
	DWORD acceptExBytes;
	Connection *pending;

	if (newsock == INVALID_SOCKET)
	{
		throw SynchronousException{
			"TCP::postAcceptSocket(): Error creating new socket",
	 		static_cast<DWORD>(WSAGetLastError())
		};
		return;
	}

	void *mem = connection_cache.alloc();
	if (!mem)
	{
		throw SynchronousException{
			"TCP::postAcceptSocket(): Error allocating new connection",
			0
		};
	}
	else
	{
		pending = new(mem) Connection{};
		pending->mSocket = newsock;
		pending->port = &port;

		if (!CreateIoCompletionPort(
			reinterpret_cast<HANDLE>(newsock),
			hCompPort,
			reinterpret_cast<ULONG_PTR>(pending),
			0))
		{
			DWORD result = GetLastError();
			closesocket(newsock);
			pending->~Connection();
			connection_cache.dealloc(pending);

			throw SynchronousException{
				"TCP::postAcceptSocket(): Unable to associate new connection with the "
				"completion port", result
			};
		}
	}

	packet->op = Packet::Operation::server_accept;
	packet->internal = pending;

	BOOL ret = fnAcceptEx(
		port.mSocket,
		newsock,
		packet->wsa.buf,
		0,
		sizeof(SOCKADDR_STORAGE) + 16,
		sizeof(SOCKADDR_STORAGE) + 16,
		&acceptExBytes,
		reinterpret_cast<LPOVERLAPPED>(packet)
	);

	if (!ret)
	{
		DWORD status = WSAGetLastError();

		if (status != WSA_IO_PENDING)
		{
			closesocket(newsock);
			pending->~Connection();
			connection_cache.dealloc(pending);

			throw SynchronousException{ "TCP::postAcceptSocket(): AcceptEx failed", status };
		}
	}

	port.acceptCount.fetch_add(1, std::memory_order_relaxed);
}

/*
	The following are all empty default implmenetations so that applications do not
	need to explicitly override each operation processor. This is particularly convenient
	when implementing only the server or client side within an application.
*/
void Hydra::TCP::server_read(OpenPort &port, Connection *client,
							 Packet *data, DWORD io_bytes) noexcept { }

void Hydra::TCP::server_write(OpenPort &port, Connection *client,
							  Packet *data, DWORD io_bytes) noexcept { }

void Hydra::TCP::server_accept(OpenPort &port, Connection *client,
							  Packet *data, DWORD io_bytes) noexcept { }

void Hydra::TCP::server_close(OpenPort &port, Connection *client,
							  Packet *data, DWORD io_bytes) noexcept { }

void Hydra::TCP::server_halved(OpenPort &port, Connection *client,
							   Packet *data, DWORD io_bytes) noexcept { }

void Hydra::TCP::client_read(OpenPort &port, Connection *server,
							 Packet *data, DWORD io_bytes) noexcept { }

void Hydra::TCP::client_write(OpenPort &port, Connection *server,
							  Packet *data, DWORD io_bytes) noexcept { }

void Hydra::TCP::client_connect(OpenPort &port, Connection *server,
								Packet *data, DWORD io_bytes) noexcept { }

void Hydra::TCP::client_disconnect(OpenPort &port, Connection *server,
								   Packet *data, DWORD io_bytes) noexcept { }

void Hydra::TCP::client_close(OpenPort &port, Connection *server,
							  Packet *data, DWORD io_bytes) noexcept { }

void Hydra::TCP::client_halved(OpenPort &port, Connection *server,
							   Packet *data, DWORD io_bytes) noexcept { }

void Hydra::TCP::async_error(OpenPort &port, Connection *remote,
							 Packet *data, DWORD error) noexcept { }

Hydra::Packet Hydra::TCP::terminate_packet =
	Hydra::Packet
	{
		Packet::Operation::internal_terminate,
		0, nullptr
	};

Hydra::Packet Hydra::TCP::barrier_packet =
	Hydra::Packet
	{
		Packet::Operation::internal_barrier,
		0, nullptr
	};

Hydra::TCP::OpenPort::OpenPort(SOCKET listening, HANDLE iocp) :
	mSocket{ listening }, hCompPort{ iocp }, closing{ false }, acceptCount{ 0 }
{

}
