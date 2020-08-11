#include "util.h"

int Hydra::loadAcceptEx(SOCKET listening, LPFN_ACCEPTEX *ppfnAcceptEx)
{
	int status = 0;
	GUID guid_AcceptEx = WSAID_ACCEPTEX;
	DWORD bytes = 0;

	status = WSAIoctl(
		listening,
		SIO_GET_EXTENSION_FUNCTION_POINTER,
		&guid_AcceptEx,
		sizeof(guid_AcceptEx),
		ppfnAcceptEx,
		sizeof(ppfnAcceptEx),
		&bytes,
		NULL,
		NULL
	);

	return status;
}

Hydra::ServerException::ServerException(const char* msg, int err)
	: std::runtime_error{ msg }, error_code{ err } { }

Hydra::ServerException::ServerException(const std::string& msg, int err)
	: std::runtime_error{ msg }, error_code{ err } { }

Hydra::ConnectionException::ConnectionException(const char* msg, int err)
	: std::runtime_error{ msg }, error_code{ err } { }

Hydra::ConnectionException::ConnectionException(const std::string& msg, int err)
	: std::runtime_error{ msg }, error_code{ err } { }