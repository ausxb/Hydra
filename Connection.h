#pragma once

#include <WinSock2.h>
#include <Windows.h>

#include <list>
#include <mutex>
#include <atomic>

#include "TCP.h"

namespace Hydra
{
	/*!
	 * \class Connection Connection.h
	 *
	 * This class wraps connection details in a TCPServer.
	 * The address of a Connection object is passed as the CompletionKey to CreateIoCompletionPort()
	 * so it can be retrieved from from GetQueuedCompletionStatus() in TCPServer::workerLoop().
	 */
	class Connection
	{
	public:
		/*!
		 * \brief Construct a connection with an uninitialized socket.
		 *
		 * Since Connection is an interface exposed to applications, all internal
		 * modifications (i.e. to the socket) are done via direct access in TCP
		 * which is declared as a friend class. This ensures applications can
		 * perform actions on the underlying connections through this abstraction.
		 *
		 * Default construction allows a Connections to be created and cached
		 * before use. A connection object can the be recycled simply by swapping
		 * out the warpped socket handle.
		 */
		Connection();

		Connection(const Connection&) = delete;
		Connection& operator=(const Connection&) = delete;
		Connection(Connection&& mv) = delete;
		Connection& operator=(Connection&& mv) = delete;

		/*!
		 * \brief Receive a packet.
		 *
		 * Sets the packet's operation to Connection::Packet::READ.
		 * The pointer to the packet must be one returned from a call to Connection::acquirePacket().
		 * The server application is responsible for discarding the packet to avoid memory leaks
		 * and heap corruption. See TCPServer::Procedure for details.
		 *
		 * \param[in] packet A pointer to a packet acquired through Connection::acquirePacket().
		 *
		 * \return Zero on synchronous completion and the value of WSAGetLastError() otherwise.
		 */
		int receive(Packet* packet);

		/*!
		 * \brief Send a packet.
		 *
		 * Sets the packet's operation to Connection::Packet::WRITE.
		 * The pointer to the packet must be one returned from a call to Connection::acquirePacket().
		 * The server application is responsible for discarding the packet to avoid memory leaks
		 * and heap corruption. See TCPServer::Procedure for details.
		 *
		 * \param[in] packet A pointer to a packet acquired through Connection::acquirePacket().
		 *
		 * \return Zero on synchronous completion and the value of WSAGetLastError() otherwise.
		 */
		int send(Packet* packet);

		/*!
		 * \brief Close the socket and cleanup resources.
		 *
		 * This function is intended to be called after receiving a Packet::Operation::*_HALVED
		 * notification, which indicates the remote peer is done sending data, and after completing any
		 * final transmissions. This is effectively a call to closesocket() since the function issues
		 * a Packet::Operation::*_CLOSE completion notification on the connection. Consequently,
		 * it will cause pending asynchronous operations to return with an aborted status. To make
		 * sure that remaining operations have finished, the caller can query the connection's outstanding
		 * counter. This behavior also allows the function to also be for forcibly terminating a connection.
		 *
		 * \attention A Connection object may be destroyed by the owning TCP instance at any point after
		 * calling terminate(). Do not store or use the connection object after the TCP::server_close()
		 * completion function is called. The connection may still appear in error callbacks after
		 * TCP::server_close() has run depending on whether pending operations are being aborted. Use
		 * any such error notifications to perform cleanup/bookkeeping but do not initiate new operations.
		 *
		 * \param[in] packet A packet used to queue a connection closure request to the notification loop.
		 */
		void terminate(Packet *packet);

		/*!
		 * \brief Initiate graceful disconnect of a connection.
		 *
		 * An outstanding read operation is necessary so that the application can be notified when the
		 * remote side disconnects by receiving zero bytes. If the application wishes to receive
		 * any final transmissions before closing the socket, it make additional Connection::receive()
		 * calls in the TCP::client_disconnect() callback.
		 *
		 * \param[in] packet A packet on which to initiate the closing read operation.
		 *
		 * \return Zero on success and either a value from WSAGetLastError() or SOCKET_ERROR otherwise.
		 * If the read operation fails synchronously, the result of Connection::receive()
		 * will be returned as usual. If receiving succeeds, the return value of WSASendDisconnect(),
		 * which may be zero or SOCKET_ERROR, is returned. This allows the caller to differentiate
		 * between failure on receiving and failure on disconnecting. If the return value is SOCKET_ERROR,
		 * the caller should immediately call WSAGetLastError() before any further network operations
		 * overwrite the error status.
		 */
		int sendDisconnect(Packet* packet);

		/*!
		 * \brief Get the SOCKADDR this client was initialized with
		 *
		 * The structure holds the remote address and other information about the socket.
		 *
		 * \return A pointer to the SOCKADDR structure.
		 */
		const LPSOCKADDR sockaddr();

		private:
			/*
			 * TCP's worker loop is allowed to directly access connection details.
			 * This is so that no public interface is needed for those details, which otherwise
			 * if used by an application, might wreak havoc (e.g. locking the mutex).
			 */
			friend class TCP;

			SOCKET mSocket; ///< Remote socket
			SOCKADDR mAddr; ///< Remote address

			TCP::OpenPort *port; ///< Server port through which this connection is serviced

			std::list<Connection*>::iterator linkage;

			std::mutex outMutex; ///< Synchronize multiple WSASend
			std::mutex inMutex; ///< Synchronize multiple WSARecv

			std::atomic_int outstanding; ///< IO operations outstanding
			static_assert(ATOMIC_INT_LOCK_FREE == 2, "std::atomic_int is not lock free");
	};
}
