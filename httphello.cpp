#include <iostream>
#include <string>

#include "TCP.h"
#include "Connection.h"
#include "Packet.h"
#include "Logger.h"
#include "util.h"

#define BUFFER_SIZE 2048

static char reply[] = "HTTP/1.0 202 OK\r\nConnection: keep-alive\r\nKeep-Alive: timeout=5, max=30\r\n\
Content-Type: text/html\r\nServer: Hydra Backend\r\nContent-Length: 112\r\n\r\n\
<!DOCTYPE html><head><title>Hello World</title></head><body>\
<h1 style=\"color: #950028\">Hello, world!</h1></body>";

static char not_found[] = "HTTP/1.0 404 Not Found\r\nConnection: keep-alive\r\n\
Keep-Alive: timeout=5, max=30\r\nContent-Type: text/html\r\nServer: Hydra Backend\r\n\
Content-Length: 101\r\n\r\n<!DOCTYPE html><head><title>404</title></head><body>\
<h1>This example only has a root page</h1></body>";

class HTTPHello : public Hydra::TCP
{
public:
	static_assert(sizeof(Hydra::Packet) == 64, "Packet struct header is not aligned");

	explicit HTTPHello(unsigned initWorkers, unsigned long shutdownTimeoutMs)
		: Hydra::TCP{initWorkers, shutdownTimeoutMs}
	{

	}

	Hydra::Packet* packetFromHeap()
	{
		char *buf = new char[sizeof(Hydra::Packet) + BUFFER_SIZE];
		return new(buf) Hydra::Packet{
			Hydra::Packet::Operation::server_accept,
			BUFFER_SIZE,
			buf + sizeof(Hydra::Packet)
		};
	}

private:
	void printSynchError(Hydra::Connection *client,
		const std::string &op,
		const Hydra::SynchronousException &e)
	{
		std::ostringstream fmt;
		fmt << Hydra::addrString(client->sockaddr()) << ' ' << op << " >>> ("
			<< e.wsa_error << ") " << e.what();
		Hydra::globalLog.dbg(fmt.str());
	}

protected:
	void server_read(Hydra::TCP::OpenPort &port,
		Hydra::Connection *client,
		Hydra::Packet *packet,
		DWORD bytes) noexcept override
	{
		std::ostringstream fmt;
		fmt << "READ: Received the following from " << Hydra::addrString(client->sockaddr())
			<< '\n' << packet->wsa.buf;

		Hydra::globalLog.info(fmt.str());

		// Ideally, the request would first be validated
		std::string req_method{packet->wsa.buf, 3};
		std::string req_path{packet->wsa.buf + 4, 2};

		if (req_method == "GET" && req_path == "/ ")
		{
			packet->wsa.len = sizeof(reply);
			packet->wsa.buf = reply;
		}
		else
		{
			packet->wsa.len = sizeof(not_found);
			packet->wsa.buf = not_found;
		}

		int status = 1;
		try
		{
			status = client->send(packet);
		}
		catch(const Hydra::SynchronousException &e)
		{
			printSynchError(client, "WRITE", e);
			client->terminate(packet);
		}

		if (status == WSA_IO_PENDING)
			Hydra::globalLog.dbg("READ: Will send response asynchronously");
		else if (status == 0)
			Hydra::globalLog.dbg("READ: Sent response synchronously");
	}

	void server_write(Hydra::TCP::OpenPort &port,
		Hydra::Connection *client,
		Hydra::Packet *packet,
		DWORD bytes) noexcept override
	{
		int status = 1;
		bool read;

		try
		{
			std::ostringstream fmt;

			// Verify all data was sent
			if (bytes < packet->wsa.len)
			{
				fmt << "WRITE: Partial data sent " << Hydra::addrString(client->sockaddr());
				Hydra::globalLog.dbg(fmt.str());
				packet->wsa.buf += bytes;
				packet->wsa.len -= bytes;
				read = false;

				status = client->send(packet);
			}
			else
			{
				fmt << "WRITE: Sent full response to " << Hydra::addrString(client->sockaddr());
				Hydra::globalLog.dbg(fmt.str());
				read = true;

				// Set pointer back to embedded buffer and read again
				packet->wsa.len = BUFFER_SIZE;
				packet->wsa.buf = (char*)packet + sizeof(Hydra::Packet);
				ZeroMemory(packet->wsa.buf, BUFFER_SIZE);
				status = client->receive(packet);
			}
		}
		catch(const Hydra::SynchronousException &e)
		{
			std::string op;
			if (read) op = "READ";
			else op = "WRITE";

			printSynchError(client, op, e);
			client->terminate(packet);
		}

		if (read)
			if (status == WSA_IO_PENDING)
				Hydra::globalLog.dbg("WRITE: Will read request asynchronously");
			else if (status == 0)
				Hydra::globalLog.dbg("WRITE: Read request synchronously");
		else
			if (status == WSA_IO_PENDING)
				Hydra::globalLog.dbg("WRITE: Will send response asynchronously");
			else if (status == 0)
				Hydra::globalLog.dbg("WRITE: Sent response synchronously");
	}

	void server_accept(Hydra::TCP::OpenPort &port,
		Hydra::Connection *client,
		Hydra::Packet *packet,
		DWORD bytes) noexcept override
	{
		std::ostringstream fmt;
		fmt << "ACCEPT: Connection opened by " << Hydra::addrString(client->sockaddr());
		Hydra::globalLog.dbg(fmt.str());

		int status = 1;
		bool posting = false;
		std::string op;
		try
		{
			op = "READ";
			ZeroMemory(packet->wsa.buf, BUFFER_SIZE);
			status = client->receive(packet); // Use the accept packet

			if (status == WSA_IO_PENDING)
				Hydra::globalLog.dbg("ACCEPT: Will read client request asynchronously");
			else if (status == 0)
				Hydra::globalLog.dbg("ACCEPT: Read client request synchronously");

			op = "ACCEPT";
			postAcceptSocket(port, packetFromHeap());
			posting = true;
		}
		catch(const Hydra::SynchronousException &e)
		{
			printSynchError(client, op, e);
			client->terminate(packet);
		}
	}

	void server_close(Hydra::TCP::OpenPort &port,
		Hydra::Connection *client,
		Hydra::Packet *packet,
		DWORD bytes) noexcept override
	{
		// Goodbye!
		std::ostringstream fmt;
		fmt << "CLOSE: Connection closed on " << Hydra::addrString(client->sockaddr());
		Hydra::globalLog.info(fmt.str());
		delete packet;
	};

	void server_halved(Hydra::TCP::OpenPort &port,
		Hydra::Connection *client,
		Hydra::Packet *packet,
		DWORD bytes) noexcept override
	{
		std::ostringstream fmt;
		fmt << "DISCONN: Client send side shutdown from " << Hydra::addrString(client->sockaddr());
		Hydra::globalLog.dbg(fmt.str());
		client->terminate(packet);
	}

	void async_error(Hydra::TCP::OpenPort &port,
		Hydra::Connection *remote,
		Hydra::Packet *packet,
		DWORD err) noexcept override
	{
		// Realistically, remote and packet should first be checked for nullptr
		DebugBreak();

		char msgbuf[512];

		FormatMessage(
			FORMAT_MESSAGE_FROM_SYSTEM,
			NULL,
			err,
			LANG_USER_DEFAULT,
			msgbuf,
			512,
			NULL
		);

		msgbuf[strlen(msgbuf) - 2] = '\0'; // Not the safest method of removing the newline

		char *op = NULL;
		if(packet == NULL)
			op = "NULL";
		else
			switch(packet->op)
			{
				case Hydra::Packet::Operation::server_read:
					op = "READ";
					break;
				case Hydra::Packet::Operation::server_write:
					op = "WRITE";
					break;
				case Hydra::Packet::Operation::server_accept:
					op = "ACCEPT";
					break;
				case Hydra::Packet::Operation::server_close:
					op = "CLOSE";
					break;
				default:
					op = "OTHER/UNIMPLEMENTED";
					break;
			}

		std::ostringstream fmt;
		fmt << op << ": (" << err << ") " << msgbuf;

		Hydra::globalLog.err(fmt.str());

		switch(err)
		{
		case WSAEDISCON:
		case WSAENOTCONN:
		case WSAEISCONN:
		case WSAETIMEDOUT:
		//etc.
			// might handle connection related matters and the like here
			break;

		// ...

		default:
			// something bad happened
			if(remote) remote->terminate(packet);
			break;
		}
	}
};

int main()
{
	int status = 0;
	WSADATA wsaData;
	status = WSAStartup(MAKEWORD( 2, 2 ), &wsaData);

	if (status != 0)
	{
		std::cerr << "HTTP Hello Server: WSAStartup() failed" << std::endl;
		return 1;
	}

	std::cout << "Winsock implementation: " << wsaData.szDescription << std::endl;
	std::cout << "Winsock configuration: " << wsaData.szSystemStatus << std::endl;
	std::cout << "Enter some text to exit\n" << std::endl;

	try
	{
		std::string s;

		// 3 threads (4th core is this main thread), 500ms shutdown timeout (unused)
		HTTPHello server{ 3, 500 };

		// No interface specified, port 80, backlog 24
		Hydra::TCP::OpenPort &http = server.listen(nullptr, "80", 24);

		// Open port 8080 as well, also with backlog 24
		Hydra::TCP::OpenPort &http8080 = server.listen(nullptr, "8080", 24);

		// Post initial accept packet on both ports
		// This should be refactored later so that postAcceptSocket is called
		// directly on http. May need to split up TCP::OpenPort into different
		// objects that provide an internal and external port control.
		server.postAcceptSocket(http, server.packetFromHeap());
		server.postAcceptSocket(http8080, server.packetFromHeap());

		std::cin >> s; // Enter some text to exit
	}
	catch(const Hydra::SynchronousException& e)
	{
		std::cerr << e.what() << std::endl;
	}

	WSACleanup();

	return 0;
}
