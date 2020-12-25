template<class A>
Hydra::TCPServer<A>::TCPServer(
	A& application,
	typename TCPServer<A>::Procedure readproc,
	typename TCPServer<A>::Procedure writeproc,
	typename TCPServer<A>::Procedure acceptproc,
	typename TCPServer<A>::Procedure closeproc,
	typename TCPServer<A>::Procedure errorproc,
	const Hydra::ServerConfig& config
) : refApp{ application }, pmfnReadProc{ readproc }, pmfnWriteProc{ writeproc },
pmfnAcceptProc{ acceptproc }, pmfnCloseProc{ closeproc },
pmfnErrorProc{ errorproc }, mConfig{ config },
mSocket{ INVALID_SOCKET }, mAcceptCount{ 0 },
hCompPort{ INVALID_HANDLE_VALUE }, hWorkers{ config.initWorkers },
bRunning{ false }, dwBarrierCount{ 0 },
hServerHeap{ INVALID_HANDLE_VALUE }
{
	if (readproc == nullptr || writeproc == nullptr ||
		acceptproc == nullptr || closeproc == nullptr || errorproc == nullptr)
		throw ServerException{ "TCPServer::TCPServer(): All callbacks must be valid", 0 };

	if (mConfig.initWorkers == 0)
		throw ServerException{ "TCPServer::TCPServer(): There must be at least a single thread", 0 };

	if (mConfig.backlog == 0)
		throw ServerException{ "TCPServer::TCPServer(): The backlog size must be greater than zero", 0 };

	if (mConfig.heapSize == 0)
		throw ServerException{ "TCPServer::TCPServer(): The packet heap must be of a reasonable size", 0 };

	hServerHeap = HeapCreate(0, mConfig.heapSize, 0);
	if (!hServerHeap)
	{
		int e = GetLastError();
		throwWithError<ServerException>("TCPServer::TCPServer(): unable to create packet heap", e);
	}
}

template<class A>
Hydra::TCPServer<A>::~TCPServer()
{
	stop();
	HeapDestroy(hServerHeap);
}

template<class A>
void Hydra::TCPServer<A>::start()
{
	if (bRunning.exchange(true) == false)
	{
		setupCompletionPort();
		acquireSocket();
		launchWorkers();
		postInitialAccepts();
	}
}

template<class A>
void Hydra::TCPServer<A>::stop()
{
	if (bRunning.exchange(false) == true)
	{
		releaseSocket();
		terminateWorkers();
		cleanupCompletionPort();
	}
}

template<class A>
Hydra::Connection::Packet* Hydra::TCPServer<A>::acquirePacket(Connection::Packet::Operation op, ULONG length, PVOID context) const
{
	// Windows' heaps are serialized
	CHAR* mem = reinterpret_cast<CHAR*>(HeapAlloc(hServerHeap, 0, sizeof(Connection::Packet) + length));
	
	//std::cout << "[ALLOC] Packet at " << (void*)mem << std::endl; 

	if (!mem) return nullptr; // Should threaded exceptions be used?

	return new(mem) Connection::Packet{ op, length, mem + sizeof(Connection::Packet), context };
}

template<class A>
void Hydra::TCPServer<A>::discardPacket(Connection::Packet* packet) const
{
	//std::cout << "[DEALLOC] Packet at " << packet << std::endl;
	packet->~Packet();
	HeapFree(hServerHeap, 0, packet);
}

template<class A>
void Hydra::TCPServer<A>::workerLoop()
{
	Connection* client = nullptr;
	Connection::Packet* io = nullptr;
	DWORD bytes = 0;
	DWORD timeout = INFINITE;

	while(true)
	{
		BOOL status = GetQueuedCompletionStatus(
			hCompPort,
			&bytes,
			reinterpret_cast<PULONG_PTR>(&client),
			reinterpret_cast<LPOVERLAPPED*>(&io),
			timeout
		);

		if (status && io == &shutdown_packet) // Terminate thread
		{
			break;
		}
		else if (!status) // Handle error
		{
			int e = WSAGetLastError();
			Connection::Packet::Operation op_err = Connection::Packet::EXCEPTIONAL;

			// To handle when GetQueuedCompletionStatus gives NULL for lpOverlapped
			if (io) op_err = io->op;

			switch (op_err)
			{
			case Connection::Packet::Operation::READ:
			case Connection::Packet::Operation::WRITE:
				--client->mOutstanding;
				(refApp.*pmfnErrorProc)(this, client, io, e);
				break;

			case Connection::Packet::Operation::ACCEPT:
			{
				SOCKET newsock = reinterpret_cast<SOCKET>(io->ptr);

				BOOL status = FALSE;
				int err = 0;
				if (bRunning.load() == true)
				{
					status = fnAcceptEx(mSocket,
						newsock,
						mAcceptExBuffer,
						0,
						sizeof(sockaddr_in) + 16,
						sizeof(sockaddr_in) + 16,
						&mAcceptExBytes,
						reinterpret_cast<LPOVERLAPPED>(io));

					if (!status) err = WSAGetLastError();
				}

				// Either AcceptEx failed synchronously or the conditions for re-posting are not met
				if(!status && err != WSA_IO_PENDING)
				{
					io->ptr = nullptr;
					// If accepting on a socket fails, there is no valid connection, indicated by nullptr
					(refApp.*pmfnErrorProc)(this, nullptr, io, e);

					mAcceptMutex.lock();
					mPostedAccepts.erase(newsock);
					mAcceptMutex.unlock();
					closesocket(newsock);
				}

				break;
			}

			case Connection::Packet::Operation::CLOSE:
				(refApp.*pmfnErrorProc)(this, client, io, e);

				mConnectionsMutex.lock();
				mConnections.erase(client);
				mConnectionsMutex.unlock();

				closesocket(client->mSocket); // Close socket unconditionally
				client->~Connection();
				HeapFree(hServerHeap, 0, client);

				break;

			default:
				(refApp.*pmfnErrorProc)(this, nullptr, nullptr, e);
				
				// Timeout occured
				if(!bRunning.load() && !io)
				{
					bool last = false;
					mNotifyMutex.lock();
					if(++dwBarrierCount == hWorkers.size())
						last = true;
					mNotifyMutex.unlock();
					
					if(last) mNotifyCV.notify_one();
					
					std::unique_lock<std::mutex> sync{ mBarrierMutex };
				}
				
				break;
			}
		}
		else // Handle network event
		{
			switch (io->op)
			{
			case Connection::Packet::Operation::READ:
				// Must decrement counter for unqueued packets regardless of callback exceptions
				--client->mOutstanding;

				if (bytes == 0)
				{	
					Logger::dbg("Sending disconnect");

					/* Acknowledge disconnect, don't try to initiate it */
					client->sendDisconnect();
					
					io->op = Connection::Packet::Operation::CLOSE;
					PostQueuedCompletionStatus(
						hCompPort,
						0,
						reinterpret_cast<ULONG_PTR>(client),
						reinterpret_cast<LPOVERLAPPED>(io)
					);
				}
				else
				{
					(refApp.*pmfnReadProc)(this, client, io, bytes);
				}
					
				break;

			case Connection::Packet::Operation::WRITE:
				--client->mOutstanding;

				(refApp.*pmfnWriteProc)(this, client, io, bytes);

				break;

			case Connection::Packet::Operation::ACCEPT:
			{
				SOCKET newsock = reinterpret_cast<SOCKET>(io->ptr);
				io->ptr = nullptr; // Applications should never have direct access to the socket

				mAcceptMutex.lock();
				size_t removed = mPostedAccepts.erase(newsock);
				size_t numPosted = mPostedAccepts.size();
				mAcceptMutex.unlock();

				int result = setsockopt(
					newsock,
					SOL_SOCKET,
					SO_UPDATE_ACCEPT_CONTEXT,
					(char*)&this->mSocket,
					sizeof(SOCKET)
				);
				
				if (result == SOCKET_ERROR)
				{
					result = WSAGetLastError();
					(refApp.*pmfnErrorProc)(this, nullptr, io, result);
					closesocket(newsock);
					break;
				}

				void* mem = HeapAlloc(hServerHeap, 0, sizeof(Connection));

				//if (!mem) return nullptr;

				DWORD64 count = mAcceptCount++;
				Connection* connected = new(mem) Connection{ newsock };

				if (!CreateIoCompletionPort(
					reinterpret_cast<HANDLE>(newsock),
					hCompPort,
					reinterpret_cast<ULONG_PTR>(connected),
					0))
				{
					result = GetLastError();
					(refApp.*pmfnErrorProc)(this, nullptr, io, result);
					closesocket(newsock);
					connected->~Connection();
					HeapFree(hServerHeap, 0, connected);
					break;
				}
				
				int namelen = sizeof(connected->mAddr);
				getpeername(connected->mSocket, &connected->mAddr, &namelen);

				mConnectionsMutex.lock();
				mConnections.insert(connected);
				mConnectionsMutex.unlock();

				if (numPosted < mConfig.acceptPostMin) postAcceptSocket();
				
				(refApp.*pmfnAcceptProc)(this, connected, io, 0);

				break;
			}

			case Connection::Packet::Operation::CLOSE:
			{
				if (client->mOutstanding.load() > 0)
				{
					Logger::dbg("Waiting to close on " + Logger::addrString(client->sockaddr()));
					PostQueuedCompletionStatus(
						hCompPort,
						0,
						reinterpret_cast<ULONG_PTR>(client),
						reinterpret_cast<LPOVERLAPPED>(io)
					);
				}
				else
				{
					// Execute callback while we still have a valid client
					(refApp.*pmfnCloseProc)(this, client, io, 0);

					Logger::dbg("Closing socket");

					/* Initiate cancellation of outstanding operations */
					closesocket(client->mSocket);
					
					Logger::dbg("Clearing client");
					
					mConnectionsMutex.lock();
					mConnections.erase(client);
					mConnectionsMutex.unlock();
					
					client->~Connection();
					HeapFree(hServerHeap, 0, client);
				}

				break;
			}

			default:
				/* Synchronization barrier */
				if(io == &barrier_packet || io == &close_packet)
				{
					bool last = false;
					mNotifyMutex.lock();
					if(++dwBarrierCount == hWorkers.size())
						last = true;
					mNotifyMutex.unlock();
					
					if(last) mNotifyCV.notify_one();
					
					/* Closing server socket, set timeout for client response */
					if(io == &close_packet)
						timeout = mConfig.shutdownTimeoutMs;
					
					std::unique_lock<std::mutex> sync{ mBarrierMutex };
				}
				
				break;
			}
		}
	}
}

template<class A>
void Hydra::TCPServer<A>::acquireSocket()
{
	struct addrinfo req, *result;
	int status = 0;

	ZeroMemory(&req, sizeof(struct addrinfo));
	req.ai_flags = AI_PASSIVE;
	req.ai_family = AF_INET;
	req.ai_socktype = SOCK_STREAM;
	req.ai_protocol = IPPROTO_TCP;

	status = getaddrinfo(mConfig.hostname, mConfig.port, &req, &result);
	if (status != 0)
	{
		//Check here for WSANOTINITIALIZED error and throw exception for that accordingly
		throwWithError<ServerException>("TCPServer::acquireSocket(): \
			getaddrinfo() failed", status);
	}

	mSocket = WSASocket(
		result->ai_family,
		result->ai_socktype,
		result->ai_protocol,
		NULL,
		0,
		WSA_FLAG_OVERLAPPED
	);

	if (mSocket == INVALID_SOCKET)
	{
		int e = WSAGetLastError();
		freeaddrinfo(result);
		throwWithError<ServerException>("TCPServer::acquireSocket(): \
			WSASocket() failed", e);
	}

	/* Do not allow any other application to use the server's address and port. */
	BOOL set_exclusive = TRUE;
	if (setsockopt(mSocket,
		SOL_SOCKET,
		SO_EXCLUSIVEADDRUSE,
		(char*) &set_exclusive,
		sizeof(BOOL)) == SOCKET_ERROR)
	{
		int e = WSAGetLastError();
		freeaddrinfo(result);
		closesocket(mSocket);
		throwWithError<ServerException>(
			"TCPServer::acquireSocket(): unable to set SO_EXCLUSIVEADDRUSE", e);
	}

	/* Use packet buffer on heap directly.
		Buffers must remain valid until completion. */
	DWORD set_size = 0;
	if (setsockopt(mSocket,
		SOL_SOCKET,
		SO_SNDBUF,
		(char*) &set_size,
		sizeof(DWORD)) == SOCKET_ERROR)
	{
		int e = WSAGetLastError();
		freeaddrinfo(result);
		closesocket(mSocket);
		throwWithError<ServerException>(
			"TCPServer::acquireSocket(): unable to set SO_SNDBUF to 0", e);
	}

	if (setsockopt(mSocket,
		SOL_SOCKET,
		SO_RCVBUF,
		(char*) &set_size,
		sizeof(DWORD)) == SOCKET_ERROR)
	{
		int e = WSAGetLastError();
		freeaddrinfo(result);
		closesocket(mSocket);
		throwWithError<ServerException>("TCPServer::acquireSocket(): \
			unable to set SO_RCVBUF to 0", e);
	}

	if (bind(mSocket, result->ai_addr, result->ai_addrlen) == SOCKET_ERROR)
	{
		int e = WSAGetLastError();
		freeaddrinfo(result);
		closesocket(mSocket);
		throwWithError<ServerException>("TCPServer::acquireSocket(): \
			bind() failed", e);
	}

	if (listen(mSocket, mConfig.backlog) == SOCKET_ERROR)
	{
		int e = WSAGetLastError();
		freeaddrinfo(result);
		closesocket(mSocket);
		throwWithError<ServerException>("TCPServer::acquireSocket(): \
			listen() failed", e);
	}

	freeaddrinfo(result);

	if (!CreateIoCompletionPort(reinterpret_cast<HANDLE>(mSocket), hCompPort, 0, 0))
	{
		int e = GetLastError();
		closesocket(mSocket);
		throwWithError<ServerException>("TCPServer::acquireSocket(): \
			failed to associate server socket with completion port", e);
	}

	if (loadAcceptEx(mSocket, &fnAcceptEx) == SOCKET_ERROR)
	{
		int e = WSAGetLastError();
		closesocket(mSocket);
		throwWithError<ServerException>("TCPServer::acquireSocket(): \
			failed to acquire function pointer to AcceptEx", e);
	}
}

template<class A>
void Hydra::TCPServer<A>::postInitialAccepts()
{
	UINT16 v = (mConfig.acceptPostMin + mConfig.acceptPostMax) / 2;
	
	Logger::info(std::string{"Posting "} + std::to_string(v) + " initial accepting sockets");
	
	for (UINT16 a = 0; a < v; ++a)
		postAcceptSocket();
}

template<class A>
void Hydra::TCPServer<A>::releaseSocket()
{	
	/* On return from synchronizeWorkers() this thread has
		exclusive control until mBarrierMutex is unlocked below. */
	synchronizeWorkers(&close_packet);

	closesocket(mSocket);

	Logger::dbg("Closing all pending accept sockets");

	for (SOCKET pending : mPostedAccepts)
		closesocket(pending);
	mPostedAccepts.clear();

	Logger::dbg("Closing all open connections");

	/* Phase 1: attempt graceful disconnect of clients */
	for (Connection* open : mConnections)
	{
		Connection::Packet* disconn = acquirePacket(Connection::Packet::READ, 0, nullptr);
		open->sendDisconnect(disconn);
	}
	
	Logger::dbg("Sent out disconnects");
	
	/* Resume workers for cleanup */
	mBarrierMutex.unlock();

	/*
		At this point, all remaining packets will be processed.
		Graceful closure will occur on clients that acknowledge
		the disconnect and remaining data will be read.
		Workers now have a timeout set in their event loop
		and will synchronize below on their first timeout.
	*/
	
	synchronizeWorkers(nullptr);

	/* Phase 2: forcibly terminate connections */
	if (!mConnections.empty())
	{
		Logger::dbg("Forcibly closing sockets");
	
		for (Connection* remaining : mConnections)
		{
			closesocket(remaining->mSocket);
			Logger::dbg(Logger::addrString(remaining->sockaddr()));
		}
		
		mBarrierMutex.unlock();
		
		/*
			Workers will process aborted IO packets until they
			timeout and synchrnoize again here, at which point
			the server can destroy connection objects.
		*/
		
		synchronizeWorkers(nullptr);
		
		for(Connection* remaining : mConnections)
		{
			remaining->~Connection();
			HeapFree(hServerHeap, 0, remaining);
		}	
	}
	
	Logger::dbg("All connections closed");
	
	mBarrierMutex.unlock();
}

template<class A>
void Hydra::TCPServer<A>::setupCompletionPort()
{
	hCompPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (!hCompPort)
	{
		int e = GetLastError();
		throwWithError<ServerException>(
			"TCPServer::setupCompletionPort(): unable to create a new completion port",
			e);
	}
}

template<class A>
void Hydra::TCPServer<A>::cleanupCompletionPort()
{
	CloseHandle(hCompPort);
}

template<class A>
void Hydra::TCPServer<A>::launchWorkers()
{
	for (std::thread& t : hWorkers)
	{
		t = std::thread{ &TCPServer::workerLoop, this };
	}
}

template<class A>
void Hydra::TCPServer<A>::terminateWorkers()
{
	Logger::dbg("Terminating threads");

	for (auto i = 0; i < hWorkers.size(); i++)
	{
		PostQueuedCompletionStatus(hCompPort,
			0, 0,
			reinterpret_cast<LPOVERLAPPED>(&shutdown_packet));
	}

	for (std::thread& t : hWorkers)
	{
		t.join();
	}
}

template<class A>
void Hydra::TCPServer<A>::synchronizeWorkers(Connection::Packet *broadcast)
{
	Logger::dbg("Initiating synchronization barrier");

	mBarrierMutex.lock();
	
	dwBarrierCount = 0;
	
	/* If broadcast is not NULL, we broadcast it to workers */
	for (auto i = 0; broadcast && i < hWorkers.size(); i++)
	{
		PostQueuedCompletionStatus(hCompPort,
			0, 0,
			reinterpret_cast<LPOVERLAPPED>(broadcast));
	}
	
	std::unique_lock<std::mutex> sync{ mNotifyMutex };
	
	/* Make sure all workers haven't yielded before sleeping. */
	if(dwBarrierCount != hWorkers.size())
		mNotifyCV.wait(sync);
	
	/* At this point, all workers are waiting on mBarrierMutex.
		mNotifyMutex will be released on return. */
}

template<class A>
void Hydra::TCPServer<A>::postAcceptSocket()
{
	SOCKET newsock = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);

	if (newsock == INVALID_SOCKET)
	{
		(refApp.*pmfnErrorProc)(this, nullptr, nullptr, WSAGetLastError());
		return;
	}

	mAcceptMutex.lock();
	mPostedAccepts.insert(newsock);
	mAcceptMutex.unlock();

	Connection::Packet* pending = acquirePacket(
		Connection::Packet::ACCEPT,
		mConfig.acceptPacketSize,
		reinterpret_cast<PVOID>(newsock)
	);

	BOOL ret = fnAcceptEx(
		mSocket,
		newsock,
		mAcceptExBuffer,
		0,
		sizeof(sockaddr_in) + 16,
		sizeof(sockaddr_in) + 16,
		&mAcceptExBytes,
		reinterpret_cast<LPOVERLAPPED>(pending)
	);

	if (!ret) 
	{
		ret = WSAGetLastError();

		if (ret != WSA_IO_PENDING)
		{
			pending->ptr = nullptr;
			(refApp.*pmfnErrorProc)(this, nullptr, pending, ret);

			mAcceptMutex.lock();
			mPostedAccepts.erase(newsock);
			mAcceptMutex.unlock();
			closesocket(newsock);
		}
	}
}

template<class A>
BYTE Hydra::TCPServer<A>::mAcceptExBuffer[] = { 0 };

template<class A>
DWORD Hydra::TCPServer<A>::mAcceptExBytes = 0;

template<class A>
Hydra::Connection::Packet Hydra::TCPServer<A>::shutdown_packet =
	Hydra::Connection::Packet
	{
		Connection::Packet::Operation::EXCEPTIONAL,
		0, nullptr, nullptr
	};
	
template<class A>
Hydra::Connection::Packet Hydra::TCPServer<A>::barrier_packet =
	Hydra::Connection::Packet
	{
		Connection::Packet::Operation::EXCEPTIONAL,
		0, nullptr, nullptr
	};
	
template<class A>
Hydra::Connection::Packet Hydra::TCPServer<A>::close_packet =
	Hydra::Connection::Packet
	{
		Connection::Packet::Operation::EXCEPTIONAL,
		0, nullptr, nullptr
	};