#include <iostream>
#include <string>
#include "TCPServer.h"

static char reply[] = "HTTP/1.0 202 OK\r\nConnection: close\r\nContent-Type: text/html\r\nContent-Length: 112\r\n\r\n\
<!DOCTYPE html><head><title>Hello World</title></head><body><h1 style=\"color: #950028\">Hello, world!</h1></body>";

static char not_found[] = "HTTP/1.0 404 Not Found\r\nConnection: close\r\nContent-Type: text/html\r\nContent-Length: 101\r\n\r\n\
<!DOCTYPE html><head><title>404</title></head><body><h1>This example only has a root page</h1></body>";

class HTTPHello
{
public:
	void accept(const Hydra::TCPServer<HTTPHello>* server,
		Hydra::Connection* client,
		Hydra::Connection::Packet* packet,
		DWORD bytes)
	{	
		Hydra::Logger::info(std::string{"Connection opened by "} + Hydra::Logger::addrString(client->sockaddr()));
		
		int status = client->receive(packet); // Use the accept packet
		
		switch(status)
		{
		case 0: // Success
			Hydra::Logger::dbg("Read client request synchronously");
			break;
		case WSA_IO_PENDING: // Pending
			Hydra::Logger::dbg("Will read client request asynchronously");
			break;
			
		// ...
			
		default: // Error
			Hydra::Logger::err(std::string{"Receiving failed synchronously: "} + std::to_string(status));
			client->sendDisconnect(packet);
			break;
		}
	};
	
	void read(const Hydra::TCPServer<HTTPHello>* server,
		Hydra::Connection* client,
		Hydra::Connection::Packet* packet,
		DWORD bytes)
	{
		std::ostringstream fmt;
		fmt << "Received the following from " << Hydra::Logger::addrString(client->sockaddr())
			<< ":\n" << packet->wsabuf.buf;
		
		Hydra::Logger::info(fmt.str());
		
		// Ideally, the request would first be validated
		std::string req_method{packet->wsabuf.buf, 3};
		std::string req_path{packet->wsabuf.buf + 4, 2};
		
		if (req_method == "GET" && req_path == "/ ")
		{
			packet->wsabuf.len = sizeof(reply);
			packet->wsabuf.buf = reply;
		}
		else
		{
			packet->wsabuf.len = sizeof(not_found);
			packet->wsabuf.buf = not_found;
		}
		
		int status = client->send(packet);
		
		if(!status && status != WSA_IO_PENDING) 
		{
			Hydra::Logger::err(std::string{"Sending failed synchronously: "} + std::to_string(status));
			client->sendDisconnect(packet);
		}
	};
	
	void write(const Hydra::TCPServer<HTTPHello>* server,
		Hydra::Connection* client,
		Hydra::Connection::Packet* packet,
		DWORD bytes)
	{
		// Verify all data was sent
		if (bytes < packet->wsabuf.len)
		{
			Hydra::Logger::dbg("Partial data sent");
			packet->wsabuf.buf += bytes;
			packet->wsabuf.len -= bytes;
			
			int status = client->send(packet);
			
			if(!status && status != WSA_IO_PENDING) 
			{
				Hydra::Logger::err(std::string{"Sending failed synchronously: "} + std::to_string(status));
				client->sendDisconnect(packet);
			}
		}
		else
		{
			Hydra::Logger::dbg("Sent full response\n");
			// Read again
			int status = client->receive(packet);
		}
	};
	
	void close(const Hydra::TCPServer<HTTPHello>* server,
		Hydra::Connection* client,
		Hydra::Connection::Packet* packet,
		DWORD bytes)
	{
		// Goodbye!
		Hydra::Logger::info(std::string{"Connection closing on "} + Hydra::Logger::addrString(client->sockaddr()));
		server->discardPacket(packet);
	};
	
	void error(const Hydra::TCPServer<HTTPHello>* server,
		Hydra::Connection* client,
		Hydra::Connection::Packet* packet,
		DWORD err)
	{
		// Realistically, client and packet should first be checked for nullptr
		
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
			op = "NULL ";
		else
			switch(packet->op)
			{
				case Hydra::Connection::Packet::READ:
					op = "READ ";
					break;
				case Hydra::Connection::Packet::WRITE:
					op = "WRITE ";
					break;
				case Hydra::Connection::Packet::ACCEPT:
					op = "ACCEPT ";
					break;
				case Hydra::Connection::Packet::CLOSE:
					op = "CLOSE ";
					break;
				default:
					op = "EXCEPTIONAL ";
					break;
			}
		
		std::ostringstream fmt;
		fmt << op << "(" << err << ") " << msgbuf;
		
		Hydra::Logger::err(fmt.str());
		
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
			// something bad, e.g. failed to associate a new connection with the completion port
			if(client) client->sendDisconnect(packet);
			break;
		}
	};
};

int main()
{
	Hydra::Logger::init();
	
	Hydra::ServerConfig cfg;
	
	cfg.hostname = NULL;
	cfg.port = "80";
	cfg.initWorkers = 3;
	cfg.backlog = 10;
	cfg.heapSize = 1024 * 2048; // 2MB on server heap
	cfg.connModulo = 10;
	cfg.acceptPacketSize = 2048;
	cfg.acceptPostMin = 5;
	cfg.acceptPostMax = 10;
	cfg.shutdownTimeoutMs = 2000;
	
	HTTPHello app{};
	
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
		
		Hydra::TCPServer<HTTPHello> server{
			app,
			&HTTPHello::read,
			&HTTPHello::write,
			&HTTPHello::accept,
			&HTTPHello::close,
			&HTTPHello::error,
			cfg
		};

		server.start();
		
		std::cin >> s; // Enter some text to exit
		
		server.stop();
	}
	catch(const Hydra::ServerException& e)
	{
		std::cerr << e.what() << std::endl;
	}
	
	WSACleanup();
	
	Hydra::Logger::deinit();
	
	return 0;
}