#include "util.h"

//========================================================================
// StaticInfo
//========================================================================
Hydra::StaticInfo::StaticInfo()
    : page_shift{ 0 }
{
    GetSystemInfo(&sys);

    unsigned page_size = sys.dwPageSize;
    while ((page_size >>= 1) != 0)
        page_shift++;

    queryCacheInfo(cpu);
}

Hydra::StaticInfo::~StaticInfo()
{

}

void Hydra::StaticInfo::queryCacheInfo(CpuInfo &info)
{
    SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *procinfo;
    DWORD len = 0;
    DWORD entries = 0;
    BOOL status = GetLogicalProcessorInformationEx(
        RelationCache,
        nullptr,
        &len
    );

    if (!status && GetLastError() == ERROR_INSUFFICIENT_BUFFER)
    {
        BYTE *buffer = new BYTE[len];
        procinfo = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>
                                    (buffer);

        status = GetLogicalProcessorInformationEx(
            RelationCache,
            procinfo,
            &len
        );

        entries = len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX);

        if (status)
        {
            for (unsigned i = 0; i < entries; i++)
            {
                if (procinfo[i].Relationship == RelationCache)
                {
                    switch (procinfo[i].Cache.Level)
                    {
                    case 1:
                        CopyMemory(&info.l1, &procinfo[i].Cache,
                                   sizeof(CACHE_RELATIONSHIP));
                        break;
                    case 2:
                        CopyMemory(&info.l2, &procinfo[i].Cache,
                                   sizeof(CACHE_RELATIONSHIP));
                        break;
                    case 3:
                        CopyMemory(&info.l3, &procinfo[i].Cache,
                                   sizeof(CACHE_RELATIONSHIP));
                        break;
                    default:
                        break;
                    }
                }
            }
        }

        delete[] buffer;
    }
}

//========================================================================
// Global system information needed by Hydra
//========================================================================
Hydra::StaticInfo static_info { };

//========================================================================
// Exception types
//========================================================================
Hydra::ServerException::ServerException(const char* msg, int err)
	: std::runtime_error{ msg }, error_code{ err } { }

Hydra::ServerException::ServerException(const std::string& msg, int err)
	: std::runtime_error{ msg }, error_code{ err } { }

Hydra::ConnectionException::ConnectionException(const char* msg, int err)
	: std::runtime_error{ msg }, error_code{ err } { }

Hydra::ConnectionException::ConnectionException(const std::string& msg, int err)
	: std::runtime_error{ msg }, error_code{ err } { }

//========================================================================
// Retrieve the AcceptEx function
//========================================================================
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
