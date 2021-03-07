#pragma once

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Mswsock.h>
#include <Windows.h>

#include <list>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>

#include "allocators/EmbeddedSlabCache.h"

namespace Hydra
{
	class Connection;
	class Packet;

	/*!
	 * \class TCP TCP.h
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
	 */
	class TCP
	{
	public:

		/*!
		 * \class OpenPort TCP.h
		 *
		 * Represents a TCP port that the application is listening on in the capacity of a server.
		 * It is essentially a handle returned by a successful call to TCP::listen().
		 */
		class OpenPort
		{
		public:
			/*!
			 * \brief A constructor that is only accessible to instances of TCP.
			 */
			OpenPort(SOCKET listening, HANDLE iocp);

		private:
			/*
			 * TCP is allowed to directly access port details. This is so that no public interface
			 * is needed for those details, which are best hidden from the application. Connection
			 * is also "trusted" since it does not expose access to OpenPort's internals through
			 * its public interface.
			 */
			friend class TCP;
			friend class Connection;

			SOCKET mSocket;
			HANDLE hCompPort;
			std::atomic_bool closing;
			std::atomic_int acceptCount;

			std::mutex controlMutex; // Hopefully, this is a CRITICAL_SECTION under the hood
			std::list<Connection*> connections;
			std::condition_variable connectionsClosedCond;
			std::condition_variable acceptsCancelledCond;

			std::list<OpenPort>::iterator linkage;

			/*std::mutex mAcceptMutex;
			std::unordered_set<Connection*> mPostedAccepts;*/
		};

		TCP(unsigned initWorkers, unsigned long shutdownTimeoutMs);

		~TCP();

		/*!
		 * \brief Termiante all connections and shutdown the application.
		 *
		 * Calling this from within an application callback will deadlock the system.
		 *
		 * This function is designed to allow concurrent shutdown of all ports and connection,
		 * instead of issuing a blocking call to TCP::close() for each open ports and waiting.
		 * Stopping a server that is not running has no effect. Unlike TCP::close(), which must
		 * be called more than once on a reference, shutdown() is idempotent.
		 */
		void shutdown();

		/*!
		 * \brief Open and begin listening on a new server port.
		 *
		 * It is expected that server ports are opened by the application at startup,
		 * so this function is not thread safe.
		 *
		 * \throws SynchronousException If the requested port is not available or there
		 * is an internal error with setting up a new socket, the source of failure is
		 * recorded in the exception message along with a relevant WSA error code.
		 *
		 * \param[in] bind Address (interface) to listen on. Parameter 1 of getaddrinfo.
		 * \param[in] port A string containing a port number, or a service name recognized
		 *                 by Windows. Parameter 2 of getaddrinfo().
		 * \param[in] backlog The backlog length for pending connections.
		 *                    Parameter 2 of listen().
		 *
		 * \return A reference to a newly opened port if successful.
		 */
		OpenPort& listen(PCSTR bind, PCSTR port, UINT backlog);

		/*!
		 * \brief Close an open server port.
		 *
		 * \attention Do not invoke close() more than once on an opened port.
		 * The reference is invalidated upon being passed as an argument.
		 *
		 * \param[in] port A reference returned by TCP::listen().
		 */
		void close(OpenPort &port);

		/*!
		 * \brief Open a new client connection.
		 *
		 *
		 * \return

		Connection& connect(PCSTR addr, PCSTR port);

		/*!
		 * \brief Open a new client connection.
		 *
		 *
		 * \return

		void disconnect(Connection &connection);
		*/

		/*!
		 * \brief Make a new socket available to accept a connection on.
		 *
		 * Creates a new socket and calls AcceptEx on it through TCP::fnAcceptEx.
		 * A new Connection is created with that socket, and associated as its completion key.
		 * Return values are checked for synchronous failure, and the error callback is
		 * called immediately (synchronously) in this case.
		 *
		 * \param[in] port
		 * \param[in] packet A pointer to a Hydra::Packet, which must contain enough
		 * 		space to hold both a local and remote sockaddr (16 bytes).
		 */
		void postAcceptSocket(OpenPort &port, Packet *packet);

		/*!
		 * \brief The minimum buffer length requried for packets used with this server.
		 *
		 * This minimum is required by AcceptEx() to store the source and destination addresses.
		 */
		constexpr static DWORD minimum_packet_buffer = 2 * (sizeof(SOCKADDR_STORAGE) + 16);

		/*
			Copying the server makes little sense, the port would be unavailable anyway.
			Since the worker threads rely on the this pointer, moving is undesirable,
			though it is possible when the server is not running and the threads have been shut down.
		*/
		TCP(const TCP&) = delete;
		TCP(TCP&&) = delete;
		TCP& operator=(const TCP&) = delete;
		TCP& operator=(TCP&&) = delete;

	protected:
		/*!
		 * \brief Operation processing function for Packet::Operation::server_read.
		 *
		 * Operation processing functions are run when a Packet is dequeued from the completion port.
		 * All parameters are the same, except the interpretation of the last parameter may always be
		 * zero for operations that do not transfer application data.
		 * For details on application callbacks see \ref callbacks "Server Application Callbacks".
		 *
		 * \param[in] client A pointer to the client's connection.
		 * \param[in] packet A pointer to the packet.
		 * \param[in] bytes The number of bytes transferred during the completed IO operation, or the error code in an error notification callback.
		 */
		virtual void server_read(OpenPort &port, Connection *client, Packet *data, DWORD io_bytes) noexcept;

		virtual void server_write(OpenPort &port, Connection *client, Packet *data, DWORD io_bytes) noexcept;
		virtual void server_accept(OpenPort &port, Connection *client, Packet *data, DWORD io_bytes) noexcept;
		virtual void server_close(OpenPort &port, Connection *client, Packet *data, DWORD io_bytes) noexcept;
		virtual void server_halved(OpenPort &port, Connection *client, Packet *data, DWORD io_bytes) noexcept;

		virtual void client_read(OpenPort &port, Connection *server, Packet *data, DWORD io_bytes) noexcept;
		virtual void client_write(OpenPort &port, Connection *server, Packet *data, DWORD io_bytes) noexcept;
		virtual void client_connect(OpenPort &port, Connection *server, Packet *data, DWORD io_bytes) noexcept;
		virtual void client_disconnect(OpenPort &port, Connection *server, Packet *data, DWORD io_bytes) noexcept;
		virtual void client_close(OpenPort &port, Connection *server, Packet *data, DWORD io_bytes) noexcept;
		virtual void client_halved(OpenPort &port, Connection *server, Packet *data, DWORD io_bytes) noexcept;

		virtual void async_error(OpenPort &port, Connection *remote, Packet *data, DWORD error) noexcept;

	private:
		static Packet terminate_packet;

		static Packet barrier_packet;

		void workerLoop(unsigned id);

		SOCKET acquireSocket(bool server, PCSTR addr, PCSTR port, UINT backlog);

		void setupCompletionPort();
		void cleanupCompletionPort();

		void launchWorkers();
		void terminateWorkers();
		void synchronizeWorkers(Packet *broadcast);
		void resumeWorkers();

		LPFN_ACCEPTEX fnAcceptEx;

		EmbeddedSlabCache connection_cache;

		HANDLE hCompPort;
		std::vector<std::thread> hWorkers;
		std::mutex mBarrierMutex;
		std::mutex mNotifyMutex;
		std::condition_variable mNotifyCV;
		DWORD dwBarrierCount;

		SOCKET mSocket;
		std::mutex mConnectionsMutex; // Hopefully, this is a CRITICAL_SECTION under the hood
		std::list<Connection*> mConnections;

		std::list<OpenPort> mOpenPorts;

		static_assert(ATOMIC_INT_LOCK_FREE == 2, "std::atomic_int is not lock free");
	};
}
