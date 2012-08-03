/* Tcl command exports
 */

#include "gomi.hh"

#define __STDC_FORMAT_MACROS
#include <cstdint>
#include <inttypes.h>

/* Boost Posix Time */
#include "boost/date_time/gregorian/gregorian_types.hpp"

#include "chromium/file_util.hh"
#include "chromium/logging.hh"
#include "microsoft/unique_handle.hh"
#include "error.hh"
#include "rfaostream.hh"
#include "gomi_bin.hh"
#include "portware.hh"

/* Feed log file FlexRecord name */
static const char* kGomiFlexRecordName = "Gomi";

/* Default FlexRecord fields. */
static const char* kDefaultLastPriceField = "LastPrice";
static const char* kDefaultTickVolumeField = "TickVolume";

/* Tcl exported API. */
static const char* kBasicFunctionName = "gomi_query";
static const char* kFeedLogFunctionName = "gomi_feedlog";

static const char* kTclApi[] = {
	kBasicFunctionName,
	kFeedLogFunctionName
};

/* Register Tcl API.
 */
bool
gomi::gomi_t::register_tcl_api (const char* id)
{
	for (size_t i = 0; i < _countof (kTclApi); ++i) {
		registerCommand (id, kTclApi[i]);
		LOG(INFO) << "Registered Tcl API \"" << kTclApi[i] << "\"";
	}
	return true;
}

/* Unregister Tcl API.
 */
bool
gomi::gomi_t::unregister_tcl_api (const char* id)
{
	for (size_t i = 0; i < _countof (kTclApi); ++i) {
		deregisterCommand (id, kTclApi[i]);
		LOG(INFO) << "Unregistered Tcl API \"" << kTclApi[i] << "\"";
	}
	return true;
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
	bin_decl_t bin_decl;

/* timezone for time calculations */
	int len = 0;
	const std::string region (Tcl_GetStringFromObj (objv[1], &len));
	if (0 == len) {
		Tcl_SetResult (interp, "TZ cannot be empty", TCL_STATIC);
		return TCL_ERROR;
	}

	bin_decl.bin_tz = tzdb_.time_zone_from_region (region);
	if (nullptr == bin_decl.bin_tz) {
		Tcl_SetResult (interp, "TZ not listed within configured time zone specifications", TCL_STATIC);
		return TCL_ERROR;
	}

	DVLOG(3) << "TZ=" << bin_decl.bin_tz->std_zone_name();

/* count of days for bin analytic */
	long day_count;
	Tcl_GetLongFromObj (interp, objv[3], &day_count);
	if (0 == day_count) {
		Tcl_SetResult (interp, "dayCount must be greater than zero", TCL_STATIC);
		return TCL_ERROR;
	}

	bin_decl.bin_day_count = day_count;
	DVLOG(3) << "dayCount=" << bin_decl.bin_day_count;

/* startTime, not converted by "clock scan" as we require time-of-day only */
	const std::string start_time_str (Tcl_GetStringFromObj (objv[4], &len));
	bin_decl.bin_start = boost::posix_time::duration_from_string (start_time_str);

/* endTime */
	const std::string end_time_str (Tcl_GetStringFromObj (objv[5], &len));
	bin_decl.bin_end = boost::posix_time::duration_from_string (end_time_str);

/* Time must be ascending. */
	if (bin_decl.bin_end <= bin_decl.bin_start) {
		Tcl_SetResult (interp, "endTime must be after startTime", TCL_STATIC);
		return TCL_ERROR;
	}

	DVLOG(3) << "startTime=" << bin_decl.bin_start << ", endTime=" << bin_decl.bin_end;

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
	std::vector<std::shared_ptr<bin_t>> query;
	for (int i = 0; i < listLen; i++)
	{
		Tcl_Obj* objPtr = nullptr;
		Tcl_ListObjIndex (interp, objv[2], i, &objPtr);

		int len = 0;
		char* symbol_text = Tcl_GetStringFromObj (objPtr, &len);
		auto bin = std::make_shared<bin_t> (bin_decl, symbol_text, kDefaultLastPriceField, kDefaultTickVolumeField);
		assert ((bool)bin);
		if (len > 0) {
			query.push_back (bin);
			DVLOG(3) << "#" << (1 + i) << " " << symbol_text;
		} else {
			Tcl_SetResult (interp, "bad symbolList", TCL_STATIC);
			return TCL_ERROR;
		}
	}

	DVLOG(3) << "processing query.";
	using namespace boost::local_time;
	const auto now_in_tz = local_sec_clock::local_time (bin_decl.bin_tz);
	const auto today_in_tz = now_in_tz.local_time().date();
	auto work_area = work_area_.get();
	auto view_element = view_element_.get();	
	std::for_each (query.begin(), query.end(), [today_in_tz, work_area, view_element](std::shared_ptr<bin_t>& it) {
		it->Calculate (today_in_tz, work_area, view_element);
	});
	DVLOG(3) << "query complete, compiling result set.";

/* Convert STL container result set into a new Tcl list. */
	Tcl_Obj* resultListPtr = Tcl_NewListObj (0, NULL);
	std::for_each (query.begin(), query.end(), [&](std::shared_ptr<bin_t>& it) {
		const double tenday_pc_rounded     = portware::round (it->GetTenDayPercentageChange());
		const double fifteenday_pc_rounded = portware::round (it->GetFifteenDayPercentageChange());
		const double twentyday_pc_rounded  = portware::round (it->GetTwentyDayPercentageChange());

		Tcl_Obj* elemObjPtr[] = {
			Tcl_NewStringObj (it->GetSymbolName(), -1),
			Tcl_NewDoubleObj (tenday_pc_rounded),
			Tcl_NewDoubleObj (fifteenday_pc_rounded),
			Tcl_NewDoubleObj (twentyday_pc_rounded),
/* no long long, alternative is to serialize to a string */
			Tcl_NewLongObj ((long)it->GetAverageVolume()), Tcl_NewLongObj ((long)it->GetAverageNonZeroVolume()),
			Tcl_NewLongObj ((long)it->GetTotalMoves()),
			Tcl_NewLongObj ((long)it->GetMaximumMoves()),
			Tcl_NewLongObj ((long)it->GetMinimumMoves()), Tcl_NewLongObj ((long)it->GetSmallestMoves())
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
 *
 * variant of gomi_query that outputs to a feedlog formatted file.
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
	bin_decl_t bin_decl;

/* timezone for time calculations */
	len = 0;
	const std::string region (Tcl_GetStringFromObj (objv[2], &len));
	if (0 == len) {
		Tcl_SetResult (interp, "TZ cannot be empty", TCL_STATIC);
		return TCL_ERROR;
	}

	bin_decl.bin_tz = tzdb_.time_zone_from_region (region);
	if (nullptr == bin_decl.bin_tz) {
		Tcl_SetResult (interp, "TZ not listed within configured time zone specifications", TCL_STATIC);
		return TCL_ERROR;
	}

	DLOG(INFO) << "TZ=" << bin_decl.bin_tz->std_zone_name();

/* count of days for bin analytic */
	long day_count = 0;
	Tcl_GetLongFromObj (interp, objv[4], &day_count);
	if (0 == day_count) {
		Tcl_SetResult (interp, "dayCount must be greater than zero", TCL_STATIC);
		return TCL_ERROR;
	}

	bin_decl.bin_day_count = day_count;
	DLOG(INFO) << "dayCount=" << bin_decl.bin_day_count;

/* startTime, not converted by "clock scan" as we require time-of-day only */
	const std::string start_time_str (Tcl_GetStringFromObj (objv[5], &len));
	bin_decl.bin_start = boost::posix_time::duration_from_string (start_time_str);

/* endTime */
	const std::string end_time_str (Tcl_GetStringFromObj (objv[6], &len));
	bin_decl.bin_end = boost::posix_time::duration_from_string (end_time_str);

/* Time must be ascending. */
	if (bin_decl.bin_end <= bin_decl.bin_start) {
		Tcl_SetResult (interp, "endTime must be after startTime", TCL_STATIC);
		return TCL_ERROR;
	}

	DLOG(INFO) << "startTime=" << bin_decl.bin_start << ", endTime=" << bin_decl.bin_end;

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
	std::vector<std::shared_ptr<bin_t>> query;
	for (int i = 0; i < listLen; i++)
	{
		Tcl_Obj* objPtr = nullptr;
		Tcl_ListObjIndex (interp, objv[3], i, &objPtr);

		int len = 0;
		char* symbol_text = Tcl_GetStringFromObj (objPtr, &len);
		auto bin = std::make_shared<bin_t> (bin_decl, symbol_text, kDefaultLastPriceField, kDefaultTickVolumeField);
		assert ((bool)bin);
		if (len > 0) {
			query.push_back (bin);
			DLOG(INFO) << "#" << (1 + i) << " " << symbol_text;
		} else {
			Tcl_SetResult (interp, "bad symbolList", TCL_STATIC);
			return TCL_ERROR;
		}
	}

	DVLOG(3) << "processing query.";
	using namespace boost::local_time;
	const auto now_in_tz = local_sec_clock::local_time (bin_decl.bin_tz);
	const auto today_in_tz = now_in_tz.local_time().date();
	auto work_area = work_area_.get();
	auto view_element = view_element_.get();	
	std::for_each (query.begin(), query.end(), [today_in_tz, work_area, view_element](std::shared_ptr<bin_t>& it) {
		it->Calculate (today_in_tz, work_area, view_element);
	});
	DVLOG(3) << "query complete, compiling result set.";
		
/* create flexrecord for each result */
	std::for_each (query.begin(), query.end(), [&](std::shared_ptr<bin_t>& it)
	{
		std::ostringstream symbol_name;
		symbol_name << it->GetSymbolName() << config_.suffix;

		const double tenday_pc_rounded     = portware::round (it->GetTenDayPercentageChange());
		const double fifteenday_pc_rounded = portware::round (it->GetFifteenDayPercentageChange());
		const double twentyday_pc_rounded  = portware::round (it->GetTwentyDayPercentageChange());

/* TODO: timestamp from end of last bin */
		__time32_t timestamp = 0;

		flexrecord_t fr (timestamp, symbol_name.str().c_str(), kGomiFlexRecordName);
		fr.stream() << tenday_pc_rounded << ','
			    << fifteenday_pc_rounded << ','
			    << twentyday_pc_rounded << ','
			    << it->GetAverageVolume() << ',' << it->GetAverageNonZeroVolume() << ','
			    << it->GetTotalMoves() << ','
			    << it->GetMaximumMoves() << ','
			    << it->GetMinimumMoves() << ',' << it->GetSmallestMoves();

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

/* eof */