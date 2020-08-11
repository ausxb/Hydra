#pragma once

#include <WinSock2.h>
#include <Windows.h>
#include <mutex>
#include <atomic>
#include "util.h"

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
		 * \struct Packet Connection.h
		 *
		 * This structure wraps structures necessary for overlapped IO and buffers for data transfer.
		 * Its buffer represents a packet at the application level.
		 */
		struct Packet
		{
			WSAOVERLAPPED overlapped; ///< Used for overlapped IO
			WSABUF wsabuf; ///< Used for Winsock function calls
			const ULONG size; ///< Stores full length available to wsabuf, useful when resending partial data
			PVOID ptr; ///< Pointer reserved for application's use

			/*!
			 * \enum Op
			 * The server uses this to determine when to call Server::readproc() or Server::writeproc().
			 */
			enum Operation { READ, WRITE, ACCEPT, CLOSE, EXCEPTIONAL };

			Operation op; ///< The operation this packet is involved in

			/*!
			 * \brief Initializes all fields on a packet, allows for convenient use of placement new.
			 *
			 * \param[in] op_enum A member of the Connection::Packet::Operation to initialize this packet with.
			 * \param[in] length The size of the packet's buffer, which remains constant unlike wsabuf.len.
			 * \param[in] buffer A pointer to the buffer that is associated used by wsabuf.
			 * \param[in] ptr A pointer reserved for the application's use.
			 */
			Packet(Operation op_enum, ULONG length, PCHAR buffer, PVOID context) :
				wsabuf{ length, buffer }, size{ length },
				op{ op_enum }, ptr{ context }
			{
				ZeroMemory(&overlapped, sizeof(WSAOVERLAPPED));
			}
		};

		/*
		 * TCPServer's worker loop is allowed to directly access connection details.
		 * This is so that no public interface is needed for those details, which otherwise
		 * if used by an application, might wreak havoc (e.g. locking the mutex).
		 */
		template<class A> friend class TCPServer;

		/*!
		 * \brief Wrap an open socket.
		 *
		 * \param[in] accepted A SOCKET that has been accepted and is ready to receive/send.
		 */
		Connection(SOCKET accepted);

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
		 * \brief Acknowledge graceful disconnect of client.
		 *
		 * When a client terminates its connection, the server will successfuly read 0 bytes.
		 * The server waits until all data has been received and then can call sendDisconnect
		 * to gracefully close the connection.
		 *
		 * \return Return value of WSASendDisconnect().
		 */
		int sendDisconnect();

		/*!
		 * \brief Initiate graceful disconnect of client from server.
		 *
		 * When TCPServer terminates its connections on shutdown,
		 * it will call sendDisconnect on every open connection and wait for final data.
		 * An outstanding read operation is necessary so that the server can be notified when the
		 * client disconnects by receiving zero bytes. Since there may not be any outstanding
		 * read operations on a client, the server must provide a packet to intiate a new
		 * read operation to ensure that it can receive a close notification.
		 * This procedure is facilitated by this method.
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
			SOCKET mSocket; ///< Remote socket
			SOCKADDR mAddr; ///< Remote address

			std::mutex mOutMutex; ///< Synchronize multiple WSASend
			std::mutex mInMutex; ///< Synchronize multiple WSARecv

			std::atomic_uint mOutstanding; ///< IO operations outstanding
			static_assert(ATOMIC_INT_LOCK_FREE == 2, "std::atomic_uint is not lock free");
	};
}