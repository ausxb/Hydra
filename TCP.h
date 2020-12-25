#pragma once

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#include <vector>
#include <set>
#include <unordered_set>
#include <thread> //Using C++ threads should initialize CRT thread-local features
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>

#include "util.h"
#include "Connection.h"
#include "Logger.h"

namespace Hydra
{
	struct ServerConfig
	{
		PCSTR hostname;
		PCSTR port;
		UINT initWorkers;
		UINT backlog;
		UINT heapSize;
		UINT connModulo;
		ULONG acceptPacketSize;
		UINT16 acceptPostMin;
		UINT16 acceptPostMax;
		ULONG shutdownTimeoutMs;
	};

	/*!
	 * \class TCPServer TCPServer.h
	 *
	 * Implements a TCP server using IO Completion Ports to enhance scalability.
	 * This is a template class with type parameters for a class representing the server side
	 * of a networked application as well as the length of the application's transmission unit.
	 * The application class implements callbacks to process incoming and outgoing data
	 * and to be notified of new and terminating connections.
	 *
	 * \attention The callbacks provided by the application class must be thread safe. IO completion ports
	 * are an asynchronous programming model and the server's worker threads run in parallel. These worker
	 * threads pass execution to the application callbacks for data processing, and so the application
	 * must manage asynchronous access to class members or other state.
	 *
	 * Instances of TCPServer or related objects must not be shared between processes as the underyling synchronization
	 * stuctures will not work across processes.
	 *
	 * \tparam A The application class for which the Server provides networking and which contains the send and receive callbacks.
	 */
	template<class A>
	class TCPServer
	{
	public:
		/*!
		 * \brief A user-supplied callback function run when when a IO completion packet is dequeued.
		 *
		 * For details on application callbacks see \ref callbacks "Server Application Callbacks".
		 *
		 * \param[in] server A pointer to the server.
		 * \param[in] client A pointer to the client's connection.
		 * \param[in] packet A pointer to the packet.
		 * \param[in] bytes The number of bytes transferred during the completed IO operation, or the error code in an error notification callback.
		 */
		using Procedure = void (A::*)(const TCPServer*, Connection*, Connection::Packet*, DWORD);

		TCPServer(
			A& application,
			Procedure readproc,
			Procedure writeproc,
			Procedure acceptproc,
			Procedure closeproc,
			Procedure errorproc,
			const ServerConfig& config
		);

		~TCPServer();

		/*!
		 * \brief Start the server.
		 *
		 * Executes the sequence of operations that establishes a running server, throwing exceptions
		 * in case startup cannot be completed with the requested parameters (e.g. port number).
		 * First a new completion port is created, then it launches worker threads, sets up a
		 * listening port, and posts the initial number of outstanding accept operations.
		 *
		 * Starting a server that is already running has no effect.
		 *
		 * \throws Hydra::ServerException server setup encountered an error.
		 */
		void start();

		/*!
		 * \brief Stop the server.
		 *
		 * Calling this from within an application callback will deadlock the server.
		 *
		 * Executes the sequence of operations that terminates a running server. The listening socket
		 * is first closed, rejecting new connections, then outstanding accepting sockets are closed.
		 * Disconnect is initiated on all open connections, and the server waits for remote peers
		 * to indicate connection shutdown and for all outstanding operations to complete,
		 * possibly with error in the case that the socket is already closed. Resources are freed
		 * except for the server heap, which persists through the lifetime of the server object.
		 *
		 * Stopping a server that is not running has no effect.
		 */
		void stop();

		/*!
		 * \brief Create a new packet.
		 *
		 * Allocates a new packet on the server heap, returning nullptr if a heap error occurs.
		 *
		 * \param[in] op A member of the Connection::Packet::Operation enumeration to initialize this packet with.
		 * \param[in] length The length of packet's transmission buffer.
		 *
		 * \return A pointer to a new packet or nullptr on allocation failure.
		 */
		Connection::Packet* acquirePacket(Connection::Packet::Operation op, ULONG length, PVOID context) const;

		/*!
		 * \brief Destroy a packet
		 *
		 * Deallocates the packet from the server heap. An application must manually discard packets.
		 *
		 * \param[in] packet A pointer to the packet.
		 */
		void discardPacket(Connection::Packet* packet) const;

		/*
			Copying the server makes little sense, the port would be unavailable anyway.
			Since the worker threads rely on the this pointer, moving is undesirable,
			though it is possible when the server is not running and the threads have been shut down.
		*/
		TCPServer(const TCPServer&) = delete;
		TCPServer(TCPServer&&) = delete;
		TCPServer& operator=(const TCPServer&) = delete;
		TCPServer& operator=(TCPServer&&) = delete;

	private:
		static Connection::Packet shutdown_packet;
		
		static Connection::Packet barrier_packet;
		
		static Connection::Packet close_packet;

		/*
			Note that mAcceptExBuffer and mAcceptExBytes will both incur race conditions
			as they are used by multiple outstanding accept operations. They are
			assumed to contain garbage but reserve necessary space.
		*/
		static BYTE mAcceptExBuffer[(sizeof(sockaddr_in) + 16) * 2]; ///< Scratch space for AcceptEx lpOutputBuffer
		static DWORD mAcceptExBytes; ///< Scratch space for lpdwBytesReceived for AcceptEx

		LPFN_ACCEPTEX fnAcceptEx;

		void workerLoop();

		void acquireSocket();
		void postInitialAccepts();
		void releaseSocket();

		void setupCompletionPort();
		void cleanupCompletionPort();

		void launchWorkers();
		void terminateWorkers();
		void synchronizeWorkers(Connection::Packet *broadcast);

		/*!
		 * \brief Make a new socket available to accept a connection on.
		 *
		 * Creates a new socket and calls AcceptEx on it through TCPServer::fnAcceptEx.
		 * A new Connection is created with that socket, and associated as its completion key.
		 * Return values are checked for synchronous failure, and the error callback is 
		 * called immediately (synchronously) in this case.
		 */
		void postAcceptSocket();

		A& refApp; ///< Reference to the server application
		Procedure pmfnReadProc; ///< Pointer to member function input callback
		Procedure pmfnWriteProc; ///< Pointer to member function output callback
		Procedure pmfnAcceptProc; ///< Pointer to member function connection accept callback
		Procedure pmfnCloseProc; ///< Pointer to member function connection close callback
		Procedure pmfnErrorProc; ///< Pointer to member function error notification callback

		const ServerConfig mConfig;

		SOCKET mSocket;
		std::mutex mConnectionsMutex; // Hopefully, this is a CRITICAL_SECTION under the hood
		std::unordered_set<Connection*> mConnections;

		std::mutex mAcceptMutex;
		std::unordered_set<SOCKET> mPostedAccepts;
		std::atomic_ullong mAcceptCount; ///< Integer "hash" used to assign new accepting sockets a mutex

		HANDLE hCompPort;
		std::vector<std::thread> hWorkers;
		std::atomic_bool bRunning;
		std::mutex mBarrierMutex;
		std::mutex mNotifyMutex;
		std::condition_variable mNotifyCV;
		DWORD dwBarrierCount;
		
		HANDLE hServerHeap;
		
		static_assert(ATOMIC_LLONG_LOCK_FREE == 2, "std::atomic_ullong is not lock free");
		static_assert(ATOMIC_BOOL_LOCK_FREE == 2, "std::atomic_bool is not lock free");
	};
}

#include "TCPServer.cpp"