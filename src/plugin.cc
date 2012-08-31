/* Boilerplate for exporting a data type to the Analytics Engine.
 */

/* VA leaks a dependency upon _MAX_PATH */
#include <cstdlib>

#include <windows.h>
#include <winsock2.h>
#include <mmsystem.h>

#pragma comment (lib, "winmm")

/* Boost string algorithms */
#include <boost/algorithm/string.hpp>

/* Google Protocol Buffers */
#include <google/protobuf/stubs/common.h>

/* Velocity Analytics Plugin Framework */
#include <vpf/vpf.h>

#include "chromium/chromium_switches.hh"
#include "chromium/command_line.hh"
#include "chromium/logging.hh"
#include "chromium/logging_win.hh"
#include "gomi.hh"

static const char* kPluginType = "GomiPlugin";

namespace { /* anonymous */

/* Vhayu log system wrapper */
static
bool
log_handler (
	int			severity,
	const char*		file,
	int			line,
	size_t			message_start,
	const std::string&	str
	)
{
	int priority;
	switch (severity) {
	default:
	case logging::LOG_INFO:		priority = eMsgInfo; break;
	case logging::LOG_WARNING:	priority = eMsgLow; break;
	case logging::LOG_ERROR:	priority = eMsgMedium; break;
	case logging::LOG_FATAL:	priority = eMsgFatal; break;
	}
/* Yay, broken APIs */
	std::string str1 (boost::algorithm::trim_right_copy (str));
	MsgLog (priority, 0, const_cast<char*> ("%s"), str1.c_str());
/* allow additional log targets */
	return false;
}

class env_t
{
public:
	env_t (const char* varname)
	{
/* startup from clean string */
		CommandLine::Init (0, nullptr);
/* the program name */
		std::string command_line (kPluginType);
/* parameters from environment */
		char* buffer;
		size_t numberOfElements;
		const errno_t err = _dupenv_s (&buffer, &numberOfElements, varname);
		if (0 == err && numberOfElements > 0) {
			command_line.append (" ");
			command_line.append (buffer);
			free (buffer);
		}
		std::string log_path = GetLogFileName();
/* update command line */
		CommandLine::ForCurrentProcess()->ParseFromString (command_line);
/* forward onto logging */
#ifndef USE_ETW_LOGGING
		logging::InitLogging(
			log_path.c_str(),
			DetermineLogMode (*CommandLine::ForCurrentProcess()),
			logging::DONT_LOCK_LOG_FILE,
			logging::APPEND_TO_OLD_LOG_FILE,
			logging::ENABLE_DCHECK_FOR_NON_OFFICIAL_RELEASE_BUILDS
			);
		logging::SetLogMessageHandler (log_handler);
#else
		logging::LogEventProvider::Initialize (kLogProvider);
#endif
	}

protected:
	std::string GetLogFileName() {
		const std::string log_filename ("/Gomi.log");
		return log_filename;
	}

	logging::LoggingDestination DetermineLogMode (const CommandLine& command_line) {
#ifdef NDEBUG
		const logging::LoggingDestination kDefaultLoggingMode = logging::LOG_NONE;
#else
		const logging::LoggingDestination kDefaultLoggingMode = logging::LOG_TO_BOTH_FILE_AND_SYSTEM_DEBUG_LOG;
#endif

		logging::LoggingDestination log_mode;
// Let --enable-logging=file force file logging, particularly useful for
// non-debug builds where otherwise you can't get logs on fault at all.
		if (command_line.GetSwitchValueASCII (switches::kEnableLogging) == "file")
			log_mode = logging::LOG_ONLY_TO_FILE;
		else
			log_mode = kDefaultLoggingMode;
		return log_mode;
	}
};

class winsock_t
{
	bool initialized;
public:
	winsock_t (unsigned majorVersion, unsigned minorVersion) :
		initialized (false)
	{
		WORD wVersionRequested = MAKEWORD (majorVersion, minorVersion);
		WSADATA wsaData;
		if (WSAStartup (wVersionRequested, &wsaData) != 0) {
			LOG(ERROR) << "WSAStartup returned " << WSAGetLastError();
			return;
		}
		if (LOBYTE (wsaData.wVersion) != majorVersion || HIBYTE (wsaData.wVersion) != minorVersion) {
			WSACleanup();
			LOG(ERROR) << "WSAStartup failed to provide requested version " << majorVersion << '.' << minorVersion;
			return;
		}
		initialized = true;
	}

	~winsock_t ()
	{
		if (initialized)
			WSACleanup();
	}
};

class timecaps_t
{
	UINT wTimerRes;
public:
	timecaps_t (unsigned resolution_ms) :
		wTimerRes (0)
	{
		TIMECAPS tc;
		if (MMSYSERR_NOERROR == timeGetDevCaps (&tc, sizeof (TIMECAPS))) {
			wTimerRes = min (max (tc.wPeriodMin, resolution_ms), tc.wPeriodMax);
			if (TIMERR_NOCANDO == timeBeginPeriod (wTimerRes)) {
				LOG(WARNING) << "Minimum timer resolution " << wTimerRes << "ms is out of range.";
				wTimerRes = 0;
			}
		} else {
			LOG(WARNING) << "Failed to query timer device resolution.";
		}
	}

	~timecaps_t()
	{
		if (wTimerRes > 0)
			timeEndPeriod (wTimerRes);
	}
};

class factory_t : public vpf::ObjectFactory
{
	env_t env_;
	winsock_t winsock_;
	timecaps_t timecaps_;

public:
	factory_t() :
		env_ ("TR_DEBUG"),
		winsock_ (2, 2),
		timecaps_ (1 /* ms */)
	{
// Verify that the version of the library that we linked against is
// compatible with the version of the headers we compiled against.
		GOOGLE_PROTOBUF_VERIFY_VERSION;

		registerType (kPluginType);
	}

/* no API to unregister type. */

	virtual void* newInstance (const char* type)
	{
		assert (0 == strcmp (kPluginType, type));
		return static_cast<vpf::AbstractUserPlugin*> (new gomi::gomi_t());
	}
};

static factory_t g_factory_instance;

} /* anonymous namespace */

/* eof */