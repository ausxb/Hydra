#pragma once

#include <WinSock2.h>
#include <Windows.h>

namespace Hydra
{
    /*!
     * \struct Packet Packet.h
     *
     * This structure wraps structures necessary for overlapped IO and buffers for data transfer.
     * Its buffer represents a packet at the application level.
     */
    struct Packet
    {
        WSAOVERLAPPED overlapped; ///< Used for overlapped IO
        WSABUF wsa; ///< Used for Winsock function calls

        /*!
         * \enum Operation
         * The server uses this to determine which completion function to run. The enumeration
         * values are of the form ROLE_ACTION where ROLE determines whether the operation is
         * being performed as a server or client or is an internal message. When ROLE is server
         * or client, ACTION indicates whether the completed packet was queued as part of a receive,
         * send, or connection management operation. The "HALVED" action indicates the remote peer
         * has shut down its send side of the socket when used with TCP (i.e. FIN was sent).
         */
        enum Operation
        {
            server_read,        ///< Receive as server
            server_write,       ///< Send as server
            server_accept,      ///< Accept a new connection as server
            server_close,       ///< Close a connection as server
            server_halved,      ///< Client send side was shutdown
            client_read,        ///< Receive as client
            client_write,       ///< Send as client
            client_connect,     ///< Open a new connection as client
            client_disconnect,  ///< Shutdown send side as client
            client_close,       ///< Close a connection as client
            client_halved,      ///< Server send side was shutdown
            internal_barrier,
            internal_terminate,
            internal_exception
        };

        Operation op; ///< The operation this packet is involved in

        PVOID internal; ///< For internal use by the system

        /*!
         * \brief Initializes all fields on a packet, allows for convenient use of placement new.
         *
         * \param[in] op_enum A member of the Connection::Packet::Operation to initialize this packet with.
         * \param[in] length The size of the packet's buffer, which remains constant unlike wsabuf.len.
         * \param[in] buffer A pointer to the buffer that is associated used by wsabuf.
         * \param[in] ctx A pointer reserved for the application's use.
         */
        Packet(Operation op_enum, ULONG length, PCHAR buffer) :
            wsa{ length, buffer }, op{ op_enum }, internal{ nullptr }
        {
            ZeroMemory(&overlapped, sizeof(WSAOVERLAPPED));
        }
    };
}
