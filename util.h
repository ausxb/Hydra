#pragma once

#include <WinSock2.h>
#include <MSWSock.h>
#include <Windows.h>
#include <stdexcept>
#include <sstream>

namespace Hydra
{
	/*!
     * \struct StaticInfo
     *
     * This structure contains information about the system needed
     * before the instantiation of any object in Hydra. Since variables
     * at namespace scope have static storage duration, system information
     * can be queried during program initialization in the constructor.
     * Resources can also be released at program termination
     * in the destructor.
     *
     * \see Hydra::static_info
     */
    struct StaticInfo
    {
        SYSTEM_INFO sys;

        struct CpuInfo
        {
            CACHE_RELATIONSHIP l1;
            CACHE_RELATIONSHIP l2;
            CACHE_RELATIONSHIP l3;
        } cpu;

        unsigned page_shift; ///< Bit index of the page size

        /*!
         * \brief Queries all information during program initialization.
         *
         * Any information that is needed before the first class from Hydra
         * is instantiated must be retrieved in the constructor.
         */
        StaticInfo();

        /*!
         * \brief Releases any resources storing information..
         */
        ~StaticInfo();

        /*!
         * \static
         * \brief Makes calls to GetLogicalProcessorInformationEx to get
         *        CPU cache which are stored in StaticInfo::cpu
         *
         * \param[i] info Reference to a CpuInfo.
         */
        static void queryCacheInfo(CpuInfo &info);
    };

    static StaticInfo static_info;

    class ServerException : public std::runtime_error
	{
	public:
		explicit ServerException(const char* msg, int err);
		explicit ServerException(const std::string& msg, int err);
		int error_code;
	};

	class ConnectionException : public std::runtime_error
	{
	public:
		explicit ConnectionException(const char* msg, int err);
		explicit ConnectionException(const std::string& msg, int err);
		int error_code;
	};

	int loadAcceptEx(SOCKET listening, LPFN_ACCEPTEX* ppfnAcceptEx);

    constexpr unsigned align_up2(unsigned val, unsigned align)
    {
        return (val + (align - 1)) & ~(align - 1);
    }

	template<class E>
	[[noreturn]] void throwWithError(LPCSTR msg, int err)
	{
		std::ostringstream format;

		format << '(' << err << ')' << ' ' << msg;

		throw E{ format.str(), err };
	}
}
