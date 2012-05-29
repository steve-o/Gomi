/* A basic Velocity Analytics User-Plugin to exporting a new Tcl command and
 * periodically publishing out to ADH via RFA using RDM/MarketPrice.
 */

#include "gomi.hh"

#define __STDC_FORMAT_MACROS
#include <cstdint>
#include <inttypes.h>

/* Boost Posix Time */
#include "boost/date_time/gregorian/gregorian_types.hpp"

#include "chromium/file_util.hh"
#include "chromium/logging.hh"
#include "chromium/string_split.hh"
#include "get_bin.hh"
#include "snmp_agent.hh"
#include "error.hh"
#include "rfa_logging.hh"
#include "rfaostream.hh"
#include "version.hh"

/* RDM Usage Guide: Section 6.5: Enterprise Platform
 * For future compatibility, the DictionaryId should be set to 1 by providers.
 * The DictionaryId for the RDMFieldDictionary is 1.
 */
static const int kDictionaryId = 1;

/* RDM: Absolutely no idea. */
static const int kFieldListId = 3;

/* RDF direct limit on symbol list entries */
static const unsigned kSymbolListLimit = 150;

/* RDM FIDs. */
static const int kRdmTimeOfUpdateId		= 5;
static const int kRdmActiveDateId		= 17;

/* FlexRecord Quote identifier. */
static const uint32_t kQuoteId = 40002;

/* Feed log file FlexRecord name */
static const char* kGomiFlexRecordName = "Gomi";

/* Tcl exported API. */
static const char* kBasicFunctionName = "gomi_query";
static const char* kFeedLogFunctionName = "gomi_feedlog";
static const char* kRepublishFunctionName = "gomi_republish";
static const char* kLastBinFunctionName = "gomi_last_bin";

/* Default FlexRecord fields. */
static const char* kDefaultLastPriceField = "LastPrice";
static const char* kDefaultTickVolumeField = "TickVolume";

/* Special last 10-minute bin name */
static const char* kLast10MinuteBinName = "10MIN";

/* http://en.wikipedia.org/wiki/Unix_epoch */
static const boost::gregorian::date kUnixEpoch (1970, 1, 1);

LONG volatile gomi::gomi_t::instance_count_ = 0;

std::list<gomi::gomi_t*> gomi::gomi_t::global_list_;
boost::shared_mutex gomi::gomi_t::global_list_lock_;

using rfa::common::RFA_String;

/* Portware defined: round half up.
 */
static inline
double
round_half_up (double x)
{
	return floor (x + 0.5);
}

/* mantissa of 10E6
 */
static inline
int64_t
portware_mantissa (double x)
{
	return (int64_t) round_half_up (x * 1000000.0);
}

/* round a double value to 6 decimal places using round half up
 */
static inline
double
portware_round (double x)
{
	return (double) portware_mantissa (x) / 1000000.0;
}

static
void
on_timer (
	PTP_CALLBACK_INSTANCE Instance,
	PVOID Context,
	PTP_TIMER Timer
	)
{
	gomi::gomi_t* gomi = static_cast<gomi::gomi_t*>(Context);
	gomi->processTimer (nullptr);
}

/* parse from /bin/ decls formatted as <name>=<start>-<end>, e.g. "OPEN=09:00-09:33"
 */
static
bool
ParseBinDecl (
	const std::string& str,
	boost::local_time::time_zone_ptr tz,
	unsigned day_count,
	gomi::bin_t* bin
	)
{
	DCHECK(nullptr != bin);
	DLOG(INFO) << "bin decl: \"" << str << "\", tz: " << tz->std_zone_abbrev() << ", day_count: " << day_count;

/* name */
	std::string::size_type pos1 = str.find_first_of ("=");
	if (std::string::npos == pos1)
		return false;
	bin->bin_name = str.substr (0, pos1);
	VLOG(1) << "bin name: " << bin->bin_name;

/* open time */
	std::string::size_type pos2 = str.find_first_of ("-", pos1);
	if (std::string::npos == pos2)
		return false;
	++pos1;
	const std::string start (str.substr (pos1, pos2 - pos1));
	VLOG(1) << "bin start: " << start;
	bin->bin_start = boost::posix_time::duration_from_string (start);

/* close time */
	++pos2;
	const std::string end (str.substr (pos2));
	VLOG(1) << "bin start: " << end;
	bin->bin_end = boost::posix_time::duration_from_string (end);

/* time zone */
	bin->bin_tz = tz;

/* bin length */
	bin->bin_day_count = day_count;
	return true;
}

/* read entire symbolmap file into memory and spit into contiguous blocks of
 * non-whitespace characters.  Zoom zoom.
 */
static
bool
ReadSymbolMap (
	const std::string& symbolmap_file,
	std::vector<std::string>* symbolmap
	)
{
	std::string contents;
	if (!file_util::ReadFileToString (symbolmap_file, &contents))
		return false;
	chromium::SplitStringAlongWhitespace (contents, symbolmap);
	return true;
}

gomi::gomi_t::gomi_t() :
	is_shutdown_ (false),
	last_refresh_ (boost::posix_time::not_a_date_time),
	last_activity_ (boost::posix_time::microsec_clock::universal_time()),
	min_tcl_time_ (boost::posix_time::pos_infin),
	max_tcl_time_ (boost::posix_time::neg_infin),
	total_tcl_time_ (boost::posix_time::seconds(0)),
	min_refresh_time_ (boost::posix_time::pos_infin),
	max_refresh_time_ (boost::posix_time::neg_infin),
	total_refresh_time_ (boost::posix_time::seconds(0))
{
	ZeroMemory (cumulative_stats_, sizeof (cumulative_stats_));
	ZeroMemory (snap_stats_, sizeof (snap_stats_));

/* Unique instance number, never decremented. */
	instance_ = InterlockedExchangeAdd (&instance_count_, 1L);

	boost::unique_lock<boost::shared_mutex> (global_list_lock_);
	global_list_.push_back (this);
}

gomi::gomi_t::~gomi_t()
{
/* Remove from list before clearing. */
	boost::unique_lock<boost::shared_mutex> (global_list_lock_);
	global_list_.remove (this);

	clear();
}

/* is bin not a 10-minute time period, i.e. a realtime component.
 */
bool
gomi::gomi_t::is_special_bin (const gomi::bin_t& bin)
{
	assert (!bin.bin_name.empty());
	auto it = config_.realtime_fids.find (bin.bin_name);
	return (it != config_.realtime_fids.end());
}

/* Plugin entry point from the Velocity Analytics Engine.
 */

void
gomi::gomi_t::init (
	const vpf::UserPluginConfig& vpf_config
	)
{
	/* Thunk to VA user-plugin base class. */
	vpf::AbstractUserPlugin::init (vpf_config);

/* Save copies of provided identifiers. */
	plugin_id_.assign (vpf_config.getPluginId());
	plugin_type_.assign (vpf_config.getPluginType());
	LOG(INFO) << "{ pluginType: \"" << plugin_type_ << "\""
		", pluginId: \"" << plugin_id_ << "\""
		", instance: " << instance_ <<
		", version: \"" << version_major << '.' << version_minor << '.' << version_build << "\""
		", build: { date: \"" << build_date << "\""
			", time: \"" << build_time << "\""
			", system: \"" << build_system << "\""
			", machine: \"" << build_machine << "\""
			" }"
		" }";

	std::vector<std::string> symbolmap;

	if (!config_.parseDomElement (vpf_config.getXmlConfigData()))
		goto cleanup;

	LOG(INFO) << config_;

/* Boost time zone database. */
	try {
		tzdb_.load_from_file (config_.tzdb);
	} catch (boost::local_time::data_not_accessible& e) {
		LOG(ERROR) << "Time zone specifications cannot be loaded: " << e.what();
		goto cleanup;
	} catch (boost::local_time::bad_field_count& e) {
		LOG(ERROR) << "Time zone specifications malformed: " << e.what();
		goto cleanup;
	}

/* default time zone */
	TZ_ = tzdb_.time_zone_from_region (config_.tz);
	if (nullptr == TZ_) {
		LOG(ERROR) << "TZ not listed within configured time zone specifications.";
		goto cleanup;
	}

/* /bin/ declarations */
	unsigned day_count = std::stoi (config_.day_count);
	for (auto it = config_.bins.begin(); it != config_.bins.end(); ++it)
	{
		bin_t bin;
		if (!ParseBinDecl (*it, TZ_, day_count, &bin)) {
			LOG(ERROR) << "Cannot parse bin delcs.";
			goto cleanup;
		}
		bins_.insert (bin);
	}

/* "Symbol map" a.k.a. list of Reuters Instrument Codes (RICs). */
	if (!ReadSymbolMap (config_.symbolmap, &symbolmap)) {
		LOG(ERROR) << "Cannot read symbolmap file: " << config_.symbolmap;
		goto cleanup;
	}

/** RFA initialisation. **/
	try {
/* RFA context. */
		rfa_.reset (new rfa_t (config_));
		if (!(bool)rfa_ || !rfa_->init())
			goto cleanup;

/* RFA asynchronous event queue. */
		const RFA_String eventQueueName (config_.event_queue_name.c_str(), 0, false);
		event_queue_.reset (rfa::common::EventQueue::create (eventQueueName), std::mem_fun (&rfa::common::EventQueue::destroy));
		if (!(bool)event_queue_)
			goto cleanup;

/* RFA logging. */
		log_.reset (new logging::LogEventProvider (config_, event_queue_));
		if (!(bool)log_ || !log_->Register())
			goto cleanup;

/* RFA provider. */
		provider_.reset (new provider_t (config_, rfa_, event_queue_));
		if (!(bool)provider_ || !provider_->init())
			goto cleanup;

/* Create state for published instruments: For every instrument, e.g. MSFT.O
 *	Realtime RIC: MSFT.O<suffix>
 *	Archive RICs: MSFT.O.<bin><suffix>
 */
/* realtime set of multiple bins */
		for (auto it = symbolmap.begin();
			it != symbolmap.end();
			++it)
		{
			const auto& symbol = *it;

/* analytic publish stream */
			std::ostringstream ss;
			ss << symbol << config_.suffix;
			auto stream = std::make_shared<realtime_stream_t> (symbol);
			assert ((bool)stream);
			if (!provider_->createItemStream (ss.str().c_str(), stream))
				goto cleanup;
			stream_vector_.push_back (stream);

/* last 10-minute bin fidset is constant */
			stream->last_10min.first = config_.realtime_fids[kLast10MinuteBinName];
		}

/* archive bins */
		for (auto it = bins_.begin();
			it != bins_.end();
			++it)
		{
/* create a symbolmap vector per bin */
			std::pair<std::vector<std::shared_ptr<janku_t>>,
				  std::vector<std::shared_ptr<archive_stream_t>>> v;

			for (auto jt = stream_vector_.begin();
				jt != stream_vector_.end();
				++jt)
			{
				auto janku = std::make_shared<janku_t> ((*jt)->symbol_name.c_str(), kDefaultLastPriceField, kDefaultTickVolumeField);
				assert ((bool)janku);

				v.first.push_back (janku);

/* analytic publish stream */
				std::ostringstream ss;
				ss << janku->symbol_name << '.' << it->bin_name	<< config_.suffix;
				auto stream = std::make_shared<archive_stream_t> (janku);
				assert ((bool)stream);
				if (!provider_->createItemStream (ss.str().c_str(), stream))
					goto cleanup;
				v.second.push_back (stream);

/* add reference to this archive to realtime stream */
				if (is_special_bin (*it)) {
					const auto& realtime_fids = config_.realtime_fids[it->bin_name];
					(*jt)->special.emplace_back (std::make_pair (realtime_fids, stream));
				} else {
					(*jt)->last_10min.second.emplace (std::make_pair (*it, stream));
				}
			}

			query_vector_.emplace (std::make_pair (*it, v));
		}

/* list bins */
for (auto it = bins_.begin(); it != bins_.end(); ++it)
	LOG(INFO) << "bin: " << it->bin_name;
/* walk one archive stream */
{
	auto it = stream_vector_[0];
	for (auto jt = it->special.begin(); jt != it->special.end(); ++jt)
		LOG(INFO) << "special: " << jt->second->rfa_name;
}

/* Microsoft threadpool timer. */
		timer_.reset (CreateThreadpoolTimer (static_cast<PTP_TIMER_CALLBACK>(on_timer), this /* closure */, nullptr /* env */));
		if (!(bool)timer_)
			goto cleanup;

	} catch (rfa::common::InvalidUsageException& e) {
		LOG(ERROR) << "InvalidUsageException: { "
			"Severity: \"" << severity_string (e.getSeverity()) << "\""
			", Classification: \"" << classification_string (e.getClassification()) << "\""
			", StatusText: \"" << e.getStatus().getStatusText() << "\" }";
		goto cleanup;
	} catch (rfa::common::InvalidConfigurationException& e) {
		LOG(ERROR) << "InvalidConfigurationException: { "
			"Severity: \"" << severity_string (e.getSeverity()) << "\""
			", Classification: \"" << classification_string (e.getClassification()) << "\""
			", StatusText: \"" << e.getStatus().getStatusText() << "\""
			", ParameterName: \"" << e.getParameterName() << "\""
			", ParameterValue: \"" << e.getParameterValue() << "\" }";
		goto cleanup;
	}

/* No main loop inside this thread, must spawn new thread for message pump. */
	event_pump_.reset (new event_pump_t (event_queue_));
	if (!(bool)event_pump_)
		goto cleanup;

	thread_.reset (new boost::thread (*event_pump_.get()));
	if (!(bool)thread_)
		goto cleanup;

/* Spawn SNMP implant. */
	if (config_.is_snmp_enabled) {
		snmp_agent_.reset (new snmp_agent_t (*this));
		if (!(bool)snmp_agent_)
			goto cleanup;
	}

/* Register Tcl API. */
	registerCommand (getId(), kBasicFunctionName);
	LOG(INFO) << "Registered Tcl API \"" << kBasicFunctionName << "\"";
	registerCommand (getId(), kFeedLogFunctionName);
	LOG(INFO) << "Registered Tcl API \"" << kFeedLogFunctionName << "\"";
	registerCommand (getId(), kRepublishFunctionName);
	LOG(INFO) << "Registered Tcl API \"" << kRepublishFunctionName << "\"";
	registerCommand (getId(), kLastBinFunctionName);
	LOG(INFO) << "Registered Tcl API \"" << kLastBinFunctionName << "\"";

/* Timer for periodic publishing.
 */
	FILETIME due_time;
	if (!get_next_bin_close (TZ_, due_time)) {
		LOG(ERROR) << "Cannot calculate next bin close time of day.";
		goto cleanup;
	}
	const DWORD timer_period = std::stoul (config_.interval) * 1000;
#if 1
	SetThreadpoolTimer (timer_.get(), &due_time, timer_period, 0);
	LOG(INFO) << "Added periodic timer, interval " << timer_period << "ms"
		", due time " << boost::posix_time::to_simple_string (boost::posix_time::from_ftime<boost::posix_time::ptime>(due_time));
#else
/* requires Platform SDK 7.1 */
	typedef BOOL (WINAPI *SetWaitableTimerExProc)(
		__in  HANDLE hTimer,
		__in  const LARGE_INTEGER *lpDueTime,
		__in  LONG lPeriod,
		__in  PTIMERAPCROUTINE pfnCompletionRoutine,
		__in  LPVOID lpArgToCompletionRoutine,
		__in  PREASON_CONTEXT WakeContext,
		__in  ULONG TolerableDelay
	);
	SetWaitableTimerExProc pFnSetWaitableTimerEx = nullptr;
	ULONG tolerance = std::stoul (config_.tolerable_delay);
	REASON_CONTEXT reasonContext = {0};
	reasonContext.Version = 0;
	reasonContext.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
	reasonContext.Reason.SimpleReasonString = L"GomiTimer";
	HMODULE hKernel32Module = GetModuleHandle (_T("kernel32.dll"));
	BOOL timer_status = false;
	if (nullptr != hKernel32Module)
		pFnSetWaitableTimerEx = (SetWaitableTimerExProc) ::GetProcAddress (hKernel32Module, "SetWaitableTimerEx");
	if (nullptr != pFnSetWaitableTimerEx)
		timer_status = pFnSetWaitableTimerEx (timer_.get(), &due_time, timer_period, nullptr, nullptr, &reasonContext, tolerance);
	if (timer_status) {
		LOG(INFO) << "Added periodic timer, interval " << timer_period << "ms, tolerance " << tolerance << "ms";
	} else {
		SetThreadpoolTimer (timer_.get(), &due_time, timer_period, 0);
		LOG(INFO) << "Added periodic timer, interval " << timer_period << "ms";
	}
#endif	
	LOG(INFO) << "Init complete, awaiting queries.";

	return;
cleanup:
	LOG(INFO) << "Init failed, cleaning up.";
	clear();
	is_shutdown_ = true;
	throw vpf::UserPluginException ("Init failed.");
}

void
gomi::gomi_t::clear()
{
/* Stop generating new events. */
	if (timer_)
		SetThreadpoolTimer (timer_.get(), nullptr, 0, 0);
	timer_.reset();

/* Close SNMP agent. */
	snmp_agent_.reset();

/* Signal message pump thread to exit. */
	if ((bool)event_queue_)
		event_queue_->deactivate();
/* Drain and close event queue. */
	if ((bool)thread_)
		thread_->join();

/* Release everything with an RFA dependency. */
	thread_.reset();
	event_pump_.reset();
	query_vector_.clear();
	assert (provider_.use_count() <= 1);
	provider_.reset();
	assert (log_.use_count() <= 1);
	log_.reset();
	assert (event_queue_.use_count() <= 1);
	event_queue_.reset();
	assert (rfa_.use_count() <= 1);
	rfa_.reset();
}

/* Plugin exit point.
 */

void
gomi::gomi_t::destroy()
{
	LOG(INFO) << "Closing instance.";
/* Unregister Tcl API. */
	deregisterCommand (getId(), kLastBinFunctionName);
	LOG(INFO) << "Unregistered Tcl API \"" << kLastBinFunctionName << "\"";
	deregisterCommand (getId(), kRepublishFunctionName);
	LOG(INFO) << "Unregistered Tcl API \"" << kRepublishFunctionName << "\"";
	deregisterCommand (getId(), kFeedLogFunctionName);
	LOG(INFO) << "Unregistered Tcl API \"" << kFeedLogFunctionName << "\"";
	deregisterCommand (getId(), kBasicFunctionName);
	LOG(INFO) << "Unregistered Tcl API \"" << kBasicFunctionName << "\"";
	clear();
	LOG(INFO) << "Runtime summary: {"
		    " tclQueryReceived: " << cumulative_stats_[GOMI_PC_TCL_QUERY_RECEIVED] <<
		   ", timerQueryReceived: " << cumulative_stats_[GOMI_PC_TIMER_QUERY_RECEIVED] <<
		" }";
	LOG(INFO) << "Instance closed.";
	vpf::AbstractUserPlugin::destroy();
}

/* Tcl boilerplate.
 */

#define Tcl_GetLongFromObj \
	(tclStubsPtr->PTcl_GetLongFromObj)	/* 39 */
#define Tcl_GetStringFromObj \
	(tclStubsPtr->PTcl_GetStringFromObj)	/* 41 */
#define Tcl_ListObjAppendElement \
	(tclStubsPtr->PTcl_ListObjAppendElement)/* 44 */
#define Tcl_ListObjIndex \
	(tclStubsPtr->PTcl_ListObjIndex)	/* 46 */
#define Tcl_ListObjLength \
	(tclStubsPtr->PTcl_ListObjLength)	/* 47 */
#define Tcl_NewDoubleObj \
	(tclStubsPtr->PTcl_NewDoubleObj)	/* 51 */
#define Tcl_NewListObj \
	(tclStubsPtr->PTcl_NewListObj)		/* 53 */
#define Tcl_NewLongObj \
	(tclStubsPtr->PTcl_NewLongObj)		/* 54 */
#define Tcl_NewStringObj \
	(tclStubsPtr->PTcl_NewStringObj)	/* 56 */
#define Tcl_SetResult \
	(tclStubsPtr->PTcl_SetResult)		/* 232 */
#define Tcl_SetObjResult \
	(tclStubsPtr->PTcl_SetObjResult)	/* 235 */
#define Tcl_WrongNumArgs \
	(tclStubsPtr->PTcl_WrongNumArgs)	/* 264 */

int
gomi::gomi_t::execute (
	const vpf::CommandInfo& cmdInfo,
	vpf::TCLCommandData& cmdData
	)
{
	int retval = TCL_ERROR;
	TCLLibPtrs* tclStubsPtr = (TCLLibPtrs*)cmdData.mClientData;
	Tcl_Interp* interp = cmdData.mInterp;		/* Current interpreter. */
	const boost::posix_time::ptime t0 (boost::posix_time::microsec_clock::universal_time());
	last_activity_ = t0;

	cumulative_stats_[GOMI_PC_TCL_QUERY_RECEIVED]++;

	try {
		const char* command = cmdInfo.getCommandName();
		if (0 == strcmp (command, kBasicFunctionName))
			retval = tclGomiQuery (cmdInfo, cmdData);
		else if (0 == strcmp (command, kFeedLogFunctionName))
			retval = tclFeedLogQuery (cmdInfo, cmdData);
		else if (0 == strcmp (command, kRepublishFunctionName))
			retval = tclRepublishQuery (cmdInfo, cmdData);
		else if (0 == strcmp (command, kLastBinFunctionName))
			retval = tclRepublishLastBinQuery (cmdInfo, cmdData);
		else
			Tcl_SetResult (interp, "unknown function", TCL_STATIC);
	}
/* FlexRecord exceptions */
	catch (const vpf::PluginFrameworkException& e) {
		/* yay broken Tcl API */
		Tcl_SetResult (interp, (char*)e.what(), TCL_VOLATILE);
	}
	catch (...) {
		Tcl_SetResult (interp, "Unhandled exception", TCL_STATIC);
	}

/* Timing */
	const boost::posix_time::ptime t1 (boost::posix_time::microsec_clock::universal_time());
	const boost::posix_time::time_duration td = t1 - t0;
	DLOG(INFO) << "execute complete " << td.total_milliseconds() << "ms";
	if (td < min_tcl_time_) min_tcl_time_ = td;
	if (td > max_tcl_time_) max_tcl_time_ = td;
	total_tcl_time_ += td;

	return retval;
}

/* gomi_query <TZ> <symbol-list> <days> <startTime> <endTime>
 *
 * singular bin.
 *
 * example:
 *	gomi_query ”America/New_York” [TIBX.O, NKE.N] 10 "09:00" "09:30"
 */
int
gomi::gomi_t::tclGomiQuery (
	const vpf::CommandInfo& cmdInfo,
	vpf::TCLCommandData& cmdData
	)
{
	TCLLibPtrs* tclStubsPtr = (TCLLibPtrs*)cmdData.mClientData;
	Tcl_Interp* interp = cmdData.mInterp;		/* Current interpreter. */
	int objc = cmdData.mObjc;			/* Number of arguments. */
	Tcl_Obj** CONST objv = cmdData.mObjv;		/* Argument strings. */

	if (objc != 6) {
		Tcl_WrongNumArgs (interp, 1, objv, "TZ symbolList dayCount startTime endTime");
		return TCL_ERROR;
	}

/* /bin/ declaration */
	bin_t bin;

/* timezone for time calculations */
	int len = 0;
	const std::string region (Tcl_GetStringFromObj (objv[1], &len));
	if (0 == len) {
		Tcl_SetResult (interp, "TZ cannot be empty", TCL_STATIC);
		return TCL_ERROR;
	}

	bin.bin_tz = tzdb_.time_zone_from_region (region);
	if (nullptr == bin.bin_tz) {
		Tcl_SetResult (interp, "TZ not listed within configured time zone specifications", TCL_STATIC);
		return TCL_ERROR;
	}

	DVLOG(3) << "TZ=" << bin.bin_tz->std_zone_name();

/* count of days for bin analytic */
	long day_count;
	Tcl_GetLongFromObj (interp, objv[3], &day_count);
	if (0 == day_count) {
		Tcl_SetResult (interp, "dayCount must be greater than zero", TCL_STATIC);
		return TCL_ERROR;
	}

	bin.bin_day_count = day_count;
	DVLOG(3) << "dayCount=" << bin.bin_day_count;

/* startTime, not converted by "clock scan" as we require time-of-day only */
	const std::string start_time_str (Tcl_GetStringFromObj (objv[4], &len));
	bin.bin_start = boost::posix_time::duration_from_string (start_time_str);

/* endTime */
	const std::string end_time_str (Tcl_GetStringFromObj (objv[5], &len));
	bin.bin_end = boost::posix_time::duration_from_string (end_time_str);

/* Time must be ascending. */
	if (bin.bin_end <= bin.bin_start) {
		Tcl_SetResult (interp, "endTime must be after startTime", TCL_STATIC);
		return TCL_ERROR;
	}

	DVLOG(3) << "startTime=" << bin.bin_start << ", endTime=" << bin.bin_end;

/* symbolList must be a list object.
 * NB: VA 7.0 does not export Tcl_ListObjGetElements()
 */
	int listLen, result = Tcl_ListObjLength (interp, objv[2], &listLen);
	if (TCL_OK != result)
		return result;
	if (0 == listLen) {
		Tcl_SetResult (interp, "bad symbolList", TCL_STATIC);
		return TCL_ERROR;
	}

	DVLOG(3) << "symbolList with #" << listLen << " entries";

/* Convert TCl list parameter into STL container. */
	std::vector<std::shared_ptr<janku_t>> query;
	for (int i = 0; i < listLen; i++)
	{
		Tcl_Obj* objPtr = nullptr;
		Tcl_ListObjIndex (interp, objv[2], i, &objPtr);

		int len = 0;
		char* symbol_text = Tcl_GetStringFromObj (objPtr, &len);
		auto janku = std::make_shared<janku_t> (symbol_text, kDefaultLastPriceField, kDefaultTickVolumeField);
		assert ((bool)janku);
		if (len > 0) {
			query.push_back (janku);
			DVLOG(3) << "#" << (1 + i) << " " << symbol_text;
		} else {
			Tcl_SetResult (interp, "bad symbolList", TCL_STATIC);
			return TCL_ERROR;
		}
	}

	get_bin (bin, query);

	DVLOG(3) << "query complete, compiling result set.";

/* Convert STL container result set into a new Tcl list. */
	Tcl_Obj* resultListPtr = Tcl_NewListObj (0, NULL);
	std::for_each (query.begin(), query.end(),
		[&](std::shared_ptr<janku_t> it)
	{
		const double tenday_pc_rounded     = portware_round (it->tenday_percentage_change);
		const double fifteenday_pc_rounded = portware_round (it->fifteenday_percentage_change);
		const double twentyday_pc_rounded  = portware_round (it->twentyday_percentage_change);

		Tcl_Obj* elemObjPtr[] = {
			Tcl_NewStringObj (it->symbol_name.c_str(), -1),
			Tcl_NewDoubleObj (tenday_pc_rounded),
			Tcl_NewDoubleObj (fifteenday_pc_rounded),
			Tcl_NewDoubleObj (twentyday_pc_rounded),
/* no long long, alternative is to serialize to a string */
			Tcl_NewLongObj ((long)it->average_volume), Tcl_NewLongObj ((long)it->average_nonzero_volume),
			Tcl_NewLongObj ((long)it->total_moves),
			Tcl_NewLongObj ((long)it->maximum_moves),
			Tcl_NewLongObj ((long)it->minimum_moves), Tcl_NewLongObj ((long)it->smallest_moves)
		};
		Tcl_ListObjAppendElement (interp, resultListPtr, Tcl_NewListObj (_countof (elemObjPtr), elemObjPtr));
	});
	Tcl_SetObjResult (interp, resultListPtr);

	DVLOG(3) << "result set complete, returning.";

	return TCL_OK;
}

class flexrecord_t {
public:
	flexrecord_t (const __time32_t& timestamp, const char* symbol, const char* record)
	{
		VHTime vhtime;
		struct tm tm_time = { 0 };
		
		VHTimeProcessor::TTTimeToVH ((__time32_t*)&timestamp, &vhtime);
		_gmtime32_s (&tm_time, &timestamp);

		stream_ << std::setfill ('0')
/* 1: timeStamp : t_string : server receipt time, fixed format: YYYYMMDDhhmmss.ttt, e.g. 20120114060928.227 */
			<< std::setw (4) << 1900 + tm_time.tm_year
			<< std::setw (2) << 1 + tm_time.tm_mon
			<< std::setw (2) << tm_time.tm_mday
			<< std::setw (2) << tm_time.tm_hour
			<< std::setw (2) << tm_time.tm_min
			<< std::setw (2) << tm_time.tm_sec
			<< '.'
			<< std::setw (3) << 0
/* 2: eyeCatcher : t_string : @@a */
			<< ",@@a"
/* 3: recordType : t_string : FR */
			   ",FR"
/* 4: symbol : t_string : e.g. MSFT */
			   ","
			<< symbol
/* 5: defName : t_string : FlexRecord name, e.g. Quote */
			<< ',' << record
/* 6: sourceName : t_string : FlexRecord name of base derived record. */
			<< ","
/* 7: sequenceID : t_u64 : Sequence number. */
			   "," << sequence_++
/* 8: exchTimeStamp : t_VHTime : exchange timestamp */
			<< ",V" << vhtime
/* 9: subType : t_s32 : record subtype */
			<< ","
/* 10..497: user-defined data fields */
			   ",";
	}

	std::string str() { return stream_.str(); }
	std::ostream& stream() { return stream_; }
private:
	std::ostringstream stream_;
	static uint64_t sequence_;
};

uint64_t flexrecord_t::sequence_ = 0;

/* gomi_feedlog <feedlog-file> <TZ> <symbol-list> <days> <startTime> <endTime>
 */
int
gomi::gomi_t::tclFeedLogQuery (
	const vpf::CommandInfo& cmdInfo,
	vpf::TCLCommandData& cmdData
	)
{
	TCLLibPtrs* tclStubsPtr = (TCLLibPtrs*)cmdData.mClientData;
	Tcl_Interp* interp = cmdData.mInterp;		/* Current interpreter. */
	int objc = cmdData.mObjc;			/* Number of arguments. */
	Tcl_Obj** CONST objv = cmdData.mObjv;		/* Argument strings. */

	if (objc != 7) {
		Tcl_WrongNumArgs (interp, 1, objv, "feedLogFile TZ symbolList dayCount startTime endTime");
		return TCL_ERROR;
	}

/* feedLogFile */
	int len = 0;
	char* feedlog_file = Tcl_GetStringFromObj (objv[1], &len);
	if (0 == len) {
		Tcl_SetResult (interp, "bad feedlog file", TCL_STATIC);
		return TCL_ERROR;
	}

	ms::handle file (CreateFile (feedlog_file,
				     GENERIC_WRITE,
				     0,
				     NULL,
				     CREATE_ALWAYS,
				     0,
				     NULL));
	if (!file) {
		LOG(WARNING) << "Failed to create file " << feedlog_file << " error code=" << GetLastError();
		return TCL_ERROR;
	}

	DLOG(INFO) << "feedLogFile=" << feedlog_file;

/* /bin/ declaration */
	bin_t bin;

/* timezone for time calculations */
	len = 0;
	const std::string region (Tcl_GetStringFromObj (objv[2], &len));
	if (0 == len) {
		Tcl_SetResult (interp, "TZ cannot be empty", TCL_STATIC);
		return TCL_ERROR;
	}

	bin.bin_tz = tzdb_.time_zone_from_region (region);
	if (nullptr == bin.bin_tz) {
		Tcl_SetResult (interp, "TZ not listed within configured time zone specifications", TCL_STATIC);
		return TCL_ERROR;
	}

	DLOG(INFO) << "TZ=" << bin.bin_tz->std_zone_name();

/* count of days for bin analytic */
	long day_count = 0;
	Tcl_GetLongFromObj (interp, objv[4], &day_count);
	if (0 == day_count) {
		Tcl_SetResult (interp, "dayCount must be greater than zero", TCL_STATIC);
		return TCL_ERROR;
	}

	bin.bin_day_count = day_count;
	DLOG(INFO) << "dayCount=" << bin.bin_day_count;

/* startTime, not converted by "clock scan" as we require time-of-day only */
	const std::string start_time_str (Tcl_GetStringFromObj (objv[5], &len));
	bin.bin_start = boost::posix_time::duration_from_string (start_time_str);

/* endTime */
	const std::string end_time_str (Tcl_GetStringFromObj (objv[6], &len));
	bin.bin_end = boost::posix_time::duration_from_string (end_time_str);

/* Time must be ascending. */
	if (bin.bin_end <= bin.bin_start) {
		Tcl_SetResult (interp, "endTime must be after startTime", TCL_STATIC);
		return TCL_ERROR;
	}

	DLOG(INFO) << "startTime=" << bin.bin_start << ", endTime=" << bin.bin_end;

/* symbolList must be a list object.
 * NB: VA 7.0 does not export Tcl_ListObjGetElements()
 */
	int listLen, result = Tcl_ListObjLength (interp, objv[3], &listLen);
	if (TCL_OK != result)
		return result;
	if (0 == listLen) {
		Tcl_SetResult (interp, "bad symbolList", TCL_STATIC);
		return TCL_ERROR;
	}

	DLOG(INFO) << "symbolList with #" << listLen << " entries";

/* Convert TCl list parameter into STL container. */
	std::vector<std::shared_ptr<janku_t>> query;
	for (int i = 0; i < listLen; i++)
	{
		Tcl_Obj* objPtr = nullptr;
		Tcl_ListObjIndex (interp, objv[3], i, &objPtr);

		int len = 0;
		char* symbol_text = Tcl_GetStringFromObj (objPtr, &len);
		auto janku = std::make_shared<janku_t> (symbol_text, kDefaultLastPriceField, kDefaultTickVolumeField);
		assert ((bool)janku);
		if (len > 0) {
			query.push_back (janku);
			DLOG(INFO) << "#" << (1 + i) << " " << symbol_text;
		} else {
			Tcl_SetResult (interp, "bad symbolList", TCL_STATIC);
			return TCL_ERROR;
		}
	}

	get_bin (bin, query);
		
/* create flexrecord for each result */
	std::for_each (query.begin(), query.end(),
		[&](std::shared_ptr<janku_t> it)
	{
		std::ostringstream symbol_name;
		symbol_name << it->symbol_name << config_.suffix;

		const double tenday_pc_rounded     = portware_round (it->tenday_percentage_change);
		const double fifteenday_pc_rounded = portware_round (it->fifteenday_percentage_change);
		const double twentyday_pc_rounded  = portware_round (it->twentyday_percentage_change);

/* TODO: timestamp from end of last bin */
		__time32_t timestamp = 0;

		flexrecord_t fr (timestamp, symbol_name.str().c_str(), kGomiFlexRecordName);
		fr.stream() << tenday_pc_rounded << ','
			    << fifteenday_pc_rounded << ','
			    << twentyday_pc_rounded << ','
			    << it->average_volume << ',' << it->average_nonzero_volume << ','
			    << it->total_moves << ','
			    << it->maximum_moves << ','
			    << it->minimum_moves << ',' << it->smallest_moves;

		DWORD written;
		std::string line (fr.str());
		line.append ("\r\n");
		const BOOL result = WriteFile (file.get(), line.c_str(), (DWORD)line.length(), &written, nullptr);
		if (!result || written != line.length()) {
			LOG(WARNING) << "Writing file " << feedlog_file << " failed, error code=" << GetLastError();
		}
	});

	return TCL_OK;
}

/* gomi_republish
 */
int
gomi::gomi_t::tclRepublishQuery (
	const vpf::CommandInfo& cmdInfo,
	vpf::TCLCommandData& cmdData
	)
{
	TCLLibPtrs* tclStubsPtr = (TCLLibPtrs*)cmdData.mClientData;
	Tcl_Interp* interp = cmdData.mInterp;		/* Current interpreter. */
/* Refresh already running.  Note locking is handled outside query to enable
 * feedback to Tcl interface.
 */
	boost::unique_lock<boost::shared_mutex> lock (query_mutex_, boost::try_to_lock_t());
	if (!lock.owns_lock()) {
		Tcl_SetResult (interp, "query already running", TCL_STATIC);
		return TCL_ERROR;
	}

	try {
		dayRefresh();
	} catch (rfa::common::InvalidUsageException& e) {
		LOG(ERROR) << "InvalidUsageException: { "
			"Severity: \"" << severity_string (e.getSeverity()) << "\""
			", Classification: \"" << classification_string (e.getClassification()) << "\""
			", StatusText: \"" << e.getStatus().getStatusText() << "\" }";
	}
	return TCL_OK;
}

/* gomi_last_bin
 */
int
gomi::gomi_t::tclRepublishLastBinQuery (
	const vpf::CommandInfo& cmdInfo,
	vpf::TCLCommandData& cmdData
	)
{
	TCLLibPtrs* tclStubsPtr = (TCLLibPtrs*)cmdData.mClientData;
	Tcl_Interp* interp = cmdData.mInterp;		/* Current interpreter. */
/* Refresh already running.  Note locking is handled outside query to enable
 * feedback to Tcl interface.
 */
	boost::unique_lock<boost::shared_mutex> lock (query_mutex_, boost::try_to_lock_t());
	if (!lock.owns_lock()) {
		Tcl_SetResult (interp, "query already running", TCL_STATIC);
		return TCL_ERROR;
	}

	try {
		last_refresh_ = boost::posix_time::not_a_date_time;
		timeRefresh();
	} catch (rfa::common::InvalidUsageException& e) {
		LOG(ERROR) << "InvalidUsageException: { "
			"Severity: \"" << severity_string (e.getSeverity()) << "\""
			", Classification: \"" << classification_string (e.getClassification()) << "\""
			", StatusText: \"" << e.getStatus().getStatusText() << "\" }";
	}
	return TCL_OK;
}

/* callback from periodic timer.
 */
void
gomi::gomi_t::processTimer (
	void*	pClosure
	)
{
	cumulative_stats_[GOMI_PC_TIMER_QUERY_RECEIVED]++;

/* Prevent overlapped queries. */
	boost::unique_lock<boost::shared_mutex> lock (query_mutex_, boost::try_to_lock_t());
	if (!lock.owns_lock()) {
		LOG(WARNING) << "Periodic refresh aborted due to running query.";
		return;
	}

	try {
		timeRefresh();
	} catch (rfa::common::InvalidUsageException& e) {
		LOG(ERROR) << "InvalidUsageException: { "
			"Severity: \"" << severity_string (e.getSeverity()) << "\""
			", Classification: \"" << classification_string (e.getClassification()) << "\""
			", StatusText: \"" << e.getStatus().getStatusText() << "\" }";
	}
}

/* Calculate the next bin close timestamp for the requested timezone.
 */
bool
gomi::gomi_t::get_next_bin_close (
	boost::local_time::time_zone_ptr tz,
	FILETIME& ft
	)
{
	if (bins_.empty())
		return false;

	using namespace boost::posix_time;
	using namespace boost::local_time;

/* calculate timezone reference time-of-day */
	const auto now_utc = second_clock::universal_time();
	local_date_time now_tz (now_utc, tz);
	const auto td = now_tz.local_time().time_of_day();

/* search key */
	bin_t bin;
	bin.bin_end = td;

	auto it = bins_.upper_bound (bin);	/* > td (upper-bound) not >= td (lower-bound) */
	if (bins_.end() == it) {
		it = bins_.begin();	/* wrap around */
		now_tz += hours (24);
	}

/* convert time-of-day into Microsoft FILETIME */
	const local_date_time next_close (now_tz.local_time().date(), it->bin_end, tz, local_date_time::NOT_DATE_TIME_ON_ERROR);
	DCHECK (!next_close.is_not_a_date_time());

/* shift is difference between 1970-Jan-01 & 1601-Jan-01 in 100-nanosecond intervals.
 */
	const uint64_t shift = 116444736000000000ULL; // (27111902 << 32) + 3577643008

	union {
		FILETIME as_file_time;
		uint64_t as_integer; // 100-nanos since 1601-Jan-01
	} caster;
	caster.as_integer = (next_close.utc_time() - ptime (kUnixEpoch)).total_microseconds() * 10; // upconvert to 100-nanos
	caster.as_integer += shift; // now 100-nanos since 1601-Jan-01

	ft = caster.as_file_time;
	return true;
}

/* Calculate the time-of-day of the last bin close.
 */
bool
gomi::gomi_t::get_last_bin_close (
	boost::local_time::time_zone_ptr tz,
	boost::posix_time::time_duration* last_close
	)
{
	if (bins_.empty())
		return false;

	using namespace boost::posix_time;
	using namespace boost::local_time;

/* calculate timezone reference time-of-day */
	const auto now_utc = second_clock::universal_time();
	const local_date_time now_tz (now_utc, tz);
	const auto td = now_tz.local_time().time_of_day();

/* search key */
	bin_t bin;
	bin.bin_end = td;

	auto it = --(bins_.upper_bound (bin));	/* > td (upper-bound) not >= td (lower-bound) */
	*last_close = it->bin_end;
	return true;
}

/* http://msdn.microsoft.com/en-us/library/4ey61ayt.aspx */
#define CTIME_LENGTH	26

/* Refresh based upon time-of-day.  Archive & realtime bins.
 */
bool
gomi::gomi_t::timeRefresh()
{
	using namespace boost::posix_time;
	const ptime t0 (microsec_clock::universal_time());
	last_activity_ = t0;

	LOG(INFO) << "timeRefresh";

/* Calculate affected bins */
	bin_t bin;
	if (!get_last_bin_close (TZ_, &bin.bin_end)) {
		LOG(ERROR) << "Cannot calculate last bin close time of day.";
		return false;
	}

/* prevent replay except on daylight savings */
	if (!last_refresh_.is_not_a_date_time() && last_refresh_ == bin.bin_end) {
LOG(INFO) << "Last bin close time same as last refresh.";
		return false;
	}

/* constant iterator in C++11 */
	unsigned bin_refresh_count = 0;
	for (auto it = bins_.find (bin);
		it != bins_.end() && it->bin_end == bin.bin_end;
		++it)
	{
		binRefresh (*it);
		++bin_refresh_count;
	}

	if (0 == bin_refresh_count) {
LOG(INFO) << "No bins to re-calculate, last bin close: " << to_simple_string (bin.bin_end);
		return false;
	}

	summaryRefresh();

/* save this iteration time */
	last_refresh_ = bin.bin_end;
	
/* Timing */
	const ptime t1 (microsec_clock::universal_time());
	const time_duration td = t1 - t0;
	LOG(INFO) << "refresh complete " << td.total_milliseconds() << "ms";
	if (td < min_refresh_time_) min_refresh_time_ = td;
	if (td > max_refresh_time_) max_refresh_time_ = td;
	total_refresh_time_ += td;
	return true;
}

/* Refresh entire days analytic content.
 */
bool
gomi::gomi_t::dayRefresh()
{
	using namespace boost::posix_time;
	using namespace boost::local_time;
	const ptime t0 (microsec_clock::universal_time());
	last_activity_ = t0;

	LOG(INFO) << "dayRefresh";

/* Calculate affected bins */
	const auto now_utc = second_clock::universal_time();
	const local_date_time now_tz (now_utc, TZ_);
	const auto now_td = now_tz.local_time().time_of_day();

/* constant iterator in C++11 */
	for (auto it = query_vector_.begin(); it != query_vector_.end(); ++it) {
		for (auto jt = it->second.first.begin(); jt != it->second.first.end(); ++jt) {
			auto& janku = *jt;
			janku->clear();
		}
	}

	for (auto it = bins_.begin();
		it != bins_.end() && it->bin_end <= now_td;
		++it)
	{
		binRefresh (*it);

/* save this iteration time */
		last_refresh_ = it->bin_end;
	}

	summaryRefresh();

/* Timing */
	const ptime t1 (microsec_clock::universal_time());
	const time_duration td = t1 - t0;
	LOG(INFO) << "refresh complete " << td.total_milliseconds() << "ms";
	if (td < min_refresh_time_) min_refresh_time_ = td;
	if (td > max_refresh_time_) max_refresh_time_ = td;
	total_refresh_time_ += td;
	return true;
}

bool
gomi::gomi_t::binRefresh (
	const gomi::bin_t& ref_bin
	)
{
/* fixed /bin/ parameters */
	bin_t bin (ref_bin);
	bin.bin_tz = TZ_;
	bin.bin_day_count = std::stoi (config_.day_count);

	LOG(INFO) << "binRefresh ("
		"bin: { "
			"name: " << bin.bin_name << ", "
			"start: " << bin.bin_start << ", "
			"end: " << bin.bin_end << ", "
			"tz: " << bin.bin_tz->std_zone_abbrev() << ", "
			"day_count: " << bin.bin_day_count << " "
		"}"
		")";

/* refreshes last /x/ business days, i.e. executing on a holiday will only refresh the cache contents */
	auto& v = query_vector_[bin];
	get_bin (bin, v.first);

/**  (i) Refresh realtime RIC **/

/* 7.5.9.1 Create a response message (4.2.2) */
	rfa::message::RespMsg response (false);	/* reference */

/* 7.5.9.2 Set the message model type of the response. */
	response.setMsgModelType (rfa::rdm::MMT_MARKET_PRICE);
/* 7.5.9.3 Set response type. */
	response.setRespType (rfa::message::RespMsg::RefreshEnum);
	response.setIndicationMask (rfa::message::RespMsg::RefreshCompleteFlag);
/* 7.5.9.4 Set the response type enumation. */
	response.setRespTypeNum (rfa::rdm::REFRESH_UNSOLICITED);

/* 7.5.9.5 Create or re-use a request attribute object (4.2.4) */
	rfa::message::AttribInfo attribInfo (false);	/* reference */
	attribInfo.setNameType (rfa::rdm::INSTRUMENT_NAME_RIC);
	RFA_String service_name (config_.service_name.c_str(), 0, false);	/* reference */
	attribInfo.setServiceName (service_name);
	response.setAttribInfo (attribInfo);

/* 6.2.8 Quality of Service. */
	rfa::common::QualityOfService QoS;
/* Timeliness: age of data, either real-time, unspecified delayed timeliness,
 * unspecified timeliness, or any positive number representing the actual
 * delay in seconds.
 */
	QoS.setTimeliness (rfa::common::QualityOfService::realTime);
/* Rate: minimum period of change in data, either tick-by-tick, just-in-time
 * filtered rate, unspecified rate, or any positive number representing the
 * actual rate in milliseconds.
 */
	QoS.setRate (rfa::common::QualityOfService::tickByTick);
	response.setQualityOfService (QoS);

/* 4.3.1 RespMsg.Payload */
// not std::map :(  derived from rfa::common::Data
	fields_.setAssociatedMetaInfo (provider_->getRwfMajorVersion(), provider_->getRwfMinorVersion());
	fields_.setInfo (kDictionaryId, kFieldListId);

/* DataBuffer based fields must be pre-encoded and post-bound. */
	rfa::data::FieldListWriteIterator it;
	rfa::data::FieldEntry timeact_field (false), activ_date_field (false), price_field (false), integer_field (false);
	rfa::data::DataBuffer timeact_data (false), activ_date_data (false), price_data (false), integer_data (false);
	rfa::data::Real64 real64, integer64;
	rfa::data::Time rfaTime;
	rfa::data::Date rfaDate;
	struct tm _tm;

/* TIMEACT */
	assert (!v.first.empty());
	const auto& first = v.first[0];
	using namespace boost::posix_time;
	timeact_field.setFieldID (kRdmTimeOfUpdateId);
	__time32_t time32 = (first->close_time - ptime (kUnixEpoch)).total_seconds();
	_gmtime32_s (&_tm, &time32);
	rfaTime.setHour   (_tm.tm_hour);
	rfaTime.setMinute (_tm.tm_min);
	rfaTime.setSecond (_tm.tm_sec);
	rfaTime.setMillisecond (0);
	timeact_data.setTime (rfaTime);
	timeact_field.setData (timeact_data);

/* PCTCHG_10D, etc, as PRICE field type */
	real64.setMagnitudeType (rfa::data::ExponentNeg6);
	price_data.setReal64 (real64);
	price_field.setData (price_data);

/* VMA_20D, etc, as integer field type */
	integer64.setMagnitudeType (rfa::data::Exponent0);
	integer_data.setReal64 (integer64);
	integer_field.setData (integer_data);

/* ACTIV_DATE */
	activ_date_field.setFieldID (kRdmActiveDateId);
	rfaDate.setDay   (/* rfa(1-31) */ _tm.tm_mday        /* tm(1-31) */);
	rfaDate.setMonth (/* rfa(1-12) */ 1 + _tm.tm_mon     /* tm(0-11) */);
	rfaDate.setYear  (/* rfa(yyyy) */ 1900 + _tm.tm_year /* tm(yyyy-1900 */);
	activ_date_data.setDate (rfaDate);
	activ_date_field.setData (activ_date_data);

	rfa::common::RespStatus status;
/* Item interaction state: Open, Closed, ClosedRecover, Redirected, NonStreaming, or Unspecified. */
	status.setStreamState (rfa::common::RespStatus::OpenEnum);
/* Data quality state: Ok, Suspect, or Unspecified. */
	status.setDataState (rfa::common::RespStatus::OkEnum);
/* Error code, e.g. NotFound, InvalidArgument, ... */
	status.setStatusCode (rfa::common::RespStatus::NoneEnum);
	response.setRespStatus (status);

	std::for_each (v.second.begin(), v.second.end(),
		[&](std::shared_ptr<archive_stream_t>& stream)
	{
		VLOG(1) << "publish: " << stream->rfa_name;
		attribInfo.setName (stream->rfa_name);
		it.start (fields_);
/* TIMACT */
		it.bind (timeact_field);

/* PRICE field is a rfa::Real64 value specified as <mantissa> × 10?.
 * Rfa deprecates setting via <double> data types so we create a mantissa from
 * source value and consider that we publish to 6 decimal places.
 */
/* PCTCHG_10D */
		price_field.setFieldID (config_.archive_fids.Rdm10DayPercentChangeId);
		const int64_t pctchg_10d_mantissa = portware_mantissa (stream->janku->tenday_percentage_change);
		real64.setValue (pctchg_10d_mantissa);		
		it.bind (price_field);
/* PCTCHG_15D */
		price_field.setFieldID (config_.archive_fids.Rdm15DayPercentChangeId);
		const int64_t pctchg_15d_mantissa = portware_mantissa (stream->janku->fifteenday_percentage_change);
		real64.setValue (pctchg_15d_mantissa);		
		it.bind (price_field);
/* PCTCHG_20D */
		price_field.setFieldID (config_.archive_fids.Rdm20DayPercentChangeId);
		const int64_t pctchg_20d_mantissa = portware_mantissa (stream->janku->twentyday_percentage_change);
		real64.setValue (pctchg_20d_mantissa);		
		it.bind (price_field);
/* VMA_20D */
		integer_field.setFieldID (config_.archive_fids.RdmAverageVolumeId);
		integer64.setValue (stream->janku->average_volume);
		it.bind (integer_field);
/* VMA_20TD */
		integer_field.setFieldID (config_.archive_fids.RdmAverageNonZeroVolumeId);
		integer64.setValue (stream->janku->average_nonzero_volume);
		it.bind (integer_field);
/* TRDCNT_20D */
		integer_field.setFieldID (config_.archive_fids.RdmTotalMovesId);
		integer64.setValue (stream->janku->total_moves);
		it.bind (integer_field);
/* HICNT_20D */
		integer_field.setFieldID (config_.archive_fids.RdmMaximumMovesId);
		integer64.setValue (stream->janku->maximum_moves);
		it.bind (integer_field);
/* LOCNT_20D */
		integer_field.setFieldID (config_.archive_fids.RdmMinimumMovesId);
		integer64.setValue (stream->janku->minimum_moves);
		it.bind (integer_field);
/* SMCNT_20D */
		integer_field.setFieldID (config_.archive_fids.RdmSmallestMovesId);
		integer64.setValue (stream->janku->smallest_moves);
		it.bind (integer_field);
/* ACTIV_DATE */
		it.bind (activ_date_field);
		it.complete();
		response.setPayload (fields_);

#ifdef DEBUG
/* 4.2.8 Message Validation.  RFA provides an interface to verify that
 * constructed messages of these types conform to the Reuters Domain
 * Models as specified in RFA API 7 RDM Usage Guide.
 */
		RFA_String warningText;
		const uint8_t validation_status = response.validateMsg (&warningText);
		if (rfa::message::MsgValidationWarning == validation_status) {
			LOG(ERROR) << "respMsg::validateMsg: { warningText: \"" << warningText << "\" }";
		} else {
			assert (rfa::message::MsgValidationOk == validation_status);
		}
#endif
		provider_->send (*stream.get(), static_cast<rfa::common::Msg&> (response));
	});

	return true;
}

/* Refresh summary bin analytics, i.e. the realtime set of bins including
 * a special last 10-minute bin derived from the current time-of-day.
 */
bool
gomi::gomi_t::summaryRefresh ()
{
	using namespace boost::posix_time;
	using namespace boost::local_time;

	ptime close_time (neg_infin);
	bin_t last_10min_bin;
	bool has_last_10min_bin = false;

/* find close time of last bin suitable for timestamp */
/* walk entire archive data set for most recent non-null janku and capture timestamp */
	for (auto it = query_vector_.begin(); it != query_vector_.end(); ++it) {
		auto jt = it->second.first.begin();
		if ((*jt)->is_null)
			break;
		close_time = (*jt)->close_time;
		if (!is_special_bin (it->first)) {
			has_last_10min_bin = true;
			last_10min_bin = it->first;
		}
	}

/* no analytics found */
	if (close_time.is_neg_infinity())
		return false;

	LOG(INFO) << "summary timestamp: " << to_simple_string (close_time);

/* 7.5.9.1 Create a response message (4.2.2) */
	rfa::message::RespMsg response (false);	/* reference */

/* 7.5.9.2 Set the message model type of the response. */
	response.setMsgModelType (rfa::rdm::MMT_MARKET_PRICE);
/* 7.5.9.3 Set response type. */
	response.setRespType (rfa::message::RespMsg::RefreshEnum);
	response.setIndicationMask (rfa::message::RespMsg::RefreshCompleteFlag);
/* 7.5.9.4 Set the response type enumation. */
	response.setRespTypeNum (rfa::rdm::REFRESH_UNSOLICITED);

/* 7.5.9.5 Create or re-use a request attribute object (4.2.4) */
	rfa::message::AttribInfo attribInfo (false);	/* reference */
	attribInfo.setNameType (rfa::rdm::INSTRUMENT_NAME_RIC);
	RFA_String service_name (config_.service_name.c_str(), 0, false);	/* reference */
	attribInfo.setServiceName (service_name);
	response.setAttribInfo (attribInfo);

/* 6.2.8 Quality of Service. */
	rfa::common::QualityOfService QoS;
/* Timeliness: age of data, either real-time, unspecified delayed timeliness,
 * unspecified timeliness, or any positive number representing the actual
 * delay in seconds.
 */
	QoS.setTimeliness (rfa::common::QualityOfService::realTime);
/* Rate: minimum period of change in data, either tick-by-tick, just-in-time
 * filtered rate, unspecified rate, or any positive number representing the
 * actual rate in milliseconds.
 */
	QoS.setRate (rfa::common::QualityOfService::tickByTick);
	response.setQualityOfService (QoS);

/* 4.3.1 RespMsg.Payload */
// not std::map :(  derived from rfa::common::Data
	fields_.setAssociatedMetaInfo (provider_->getRwfMajorVersion(), provider_->getRwfMinorVersion());
	fields_.setInfo (kDictionaryId, kFieldListId);

/* DataBuffer based fields must be pre-encoded and post-bound. */
	rfa::data::FieldListWriteIterator it;
	rfa::data::FieldEntry timeact_field (false), activ_date_field (false), price_field (false), integer_field (false);
	rfa::data::DataBuffer timeact_data (false), activ_date_data (false), price_data (false), integer_data (false);
	rfa::data::Real64 real64, integer64;
	rfa::data::Time rfaTime;
	rfa::data::Date rfaDate;
	struct tm _tm;

/* TIMEACT */
	using namespace boost::posix_time;
	timeact_field.setFieldID (kRdmTimeOfUpdateId);
	__time32_t time32 = (close_time - ptime (kUnixEpoch)).total_seconds();
	_gmtime32_s (&_tm, &time32);
	rfaTime.setHour   (_tm.tm_hour);
	rfaTime.setMinute (_tm.tm_min);
	rfaTime.setSecond (_tm.tm_sec);
	rfaTime.setMillisecond (0);
	timeact_data.setTime (rfaTime);
	timeact_field.setData (timeact_data);

/* PCTCHG_10D, etc, as PRICE field type */
	real64.setMagnitudeType (rfa::data::ExponentNeg6);
	price_data.setReal64 (real64);
	price_field.setData (price_data);

/* VMA_20D, etc, as integer field type */
	integer64.setMagnitudeType (rfa::data::Exponent0);
	integer_data.setReal64 (integer64);
	integer_field.setData (integer_data);

/* ACTIV_DATE */
	activ_date_field.setFieldID (kRdmActiveDateId);
	rfaDate.setDay   (/* rfa(1-31) */ _tm.tm_mday        /* tm(1-31) */);
	rfaDate.setMonth (/* rfa(1-12) */ 1 + _tm.tm_mon     /* tm(0-11) */);
	rfaDate.setYear  (/* rfa(yyyy) */ 1900 + _tm.tm_year /* tm(yyyy-1900 */);
	activ_date_data.setDate (rfaDate);
	activ_date_field.setData (activ_date_data);

	rfa::common::RespStatus status;
/* Item interaction state: Open, Closed, ClosedRecover, Redirected, NonStreaming, or Unspecified. */
	status.setStreamState (rfa::common::RespStatus::OpenEnum);
/* Data quality state: Ok, Suspect, or Unspecified. */
	status.setDataState (rfa::common::RespStatus::OkEnum);
/* Error code, e.g. NotFound, InvalidArgument, ... */
	status.setStatusCode (rfa::common::RespStatus::NoneEnum);
	response.setRespStatus (status);

	std::for_each (stream_vector_.begin(), stream_vector_.end(),
		[&](std::shared_ptr<realtime_stream_t>& stream)
	{
		VLOG(1) << "publish: " << stream->rfa_name;
		attribInfo.setName (stream->rfa_name);
		it.start (fields_);
/* TIMACT */
		it.bind (timeact_field);

		auto add_fidset = [&](const fidset_t& fids, std::shared_ptr<janku_t> janku)
		{
/* PCTCHG_10D */
			price_field.setFieldID (fids.Rdm10DayPercentChangeId);
			const int64_t pctchg_10d_mantissa = portware_mantissa (janku->tenday_percentage_change);
			real64.setValue (pctchg_10d_mantissa);		
			it.bind (price_field);
/* PCTCHG_15D */
			price_field.setFieldID (fids.Rdm15DayPercentChangeId);
			const int64_t pctchg_15d_mantissa = portware_mantissa (janku->fifteenday_percentage_change);
			real64.setValue (pctchg_15d_mantissa);		
			it.bind (price_field);
/* PCTCHG_20D */
			price_field.setFieldID (fids.Rdm20DayPercentChangeId);
			const int64_t pctchg_20d_mantissa = portware_mantissa (janku->twentyday_percentage_change);
			real64.setValue (pctchg_20d_mantissa);		
			it.bind (price_field);
/* VMA_20D */
			integer_field.setFieldID (fids.RdmAverageVolumeId);
			integer64.setValue (janku->average_volume);
			it.bind (integer_field);
/* VMA_20TD */
			integer_field.setFieldID (fids.RdmAverageNonZeroVolumeId);
			integer64.setValue (janku->average_nonzero_volume);
			it.bind (integer_field);
/* TRDCNT_20D */
			integer_field.setFieldID (fids.RdmTotalMovesId);
			integer64.setValue (janku->total_moves);
			it.bind (integer_field);
/* HICNT_20D */
			integer_field.setFieldID (fids.RdmMaximumMovesId);
			integer64.setValue (janku->maximum_moves);
			it.bind (integer_field);
/* LOCNT_20D */
			integer_field.setFieldID (fids.RdmMinimumMovesId);
			integer64.setValue (janku->minimum_moves);
			it.bind (integer_field);
/* SMCNT_20D */
			integer_field.setFieldID (fids.RdmSmallestMovesId);
			integer64.setValue (janku->smallest_moves);
			it.bind (integer_field);
		};

/* every special named bin analytic */
		std::for_each (stream->special.begin(), stream->special.end(),
			[&](std::pair<fidset_t, std::shared_ptr<archive_stream_t>> archive)
		{
			add_fidset (archive.first, archive.second->janku);
		});

/* last 10-minute special bin */
		if (has_last_10min_bin)
			add_fidset (stream->last_10min.first, stream->last_10min.second[last_10min_bin]->janku);

/* ACTIV_DATE */
		it.bind (activ_date_field);
		it.complete();
		response.setPayload (fields_);

#ifdef DEBUG
/* 4.2.8 Message Validation.  RFA provides an interface to verify that
 * constructed messages of these types conform to the Reuters Domain
 * Models as specified in RFA API 7 RDM Usage Guide.
 */
		RFA_String warningText;
		const uint8_t validation_status = response.validateMsg (&warningText);
		if (rfa::message::MsgValidationWarning == validation_status) {
			LOG(ERROR) << "respMsg::validateMsg: { warningText: \"" << warningText << "\" }";
		} else {
			assert (rfa::message::MsgValidationOk == validation_status);
		}
#endif
		provider_->send (*stream.get(), static_cast<rfa::common::Msg&> (response));
	});

	return true;
}

/* eof */
