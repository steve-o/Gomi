/* A basic Velocity Analytics User-Plugin to exporting a new Tcl command and
 * periodically publishing out to ADH via RFA using RDM/MarketPrice.
 */

#include "gomi.hh"

#define __STDC_FORMAT_MACROS
#include <cstdint>
#include <inttypes.h>

/* Boost Posix Time */
#include <boost/date_time/gregorian/gregorian_types.hpp>

#include "chromium/file_util.hh"
#include "chromium/logging.hh"
#include "chromium/string_split.hh"
#include "gomi_bin.hh"
#include "snmp_agent.hh"
#include "error.hh"
#include "rfa_logging.hh"
#include "rfaostream.hh"
#include "version.hh"
#include "portware.hh"

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

/* Convert Posix time to Unix Epoch time.
 */
template< typename TimeT >
inline
TimeT
to_unix_epoch (
	const boost::posix_time::ptime t
	)
{
	return (t - boost::posix_time::ptime (kUnixEpoch)).total_seconds();
}

/* parse from /bin/ decls formatted as <name>=<start>-<end>, e.g. "OPEN=09:00-09:33"
 */
static
bool
ParseBinDecl (
	const std::string& str,
	boost::local_time::time_zone_ptr tz,
	unsigned day_count,
	gomi::bin_decl_t* bin
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

gomi::gomi_t::gomi_t()
	:
	is_shutdown_ (false),
	manager_ (nullptr),
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

	Clear();
}

/* is bin not a 10-minute time period, i.e. a realtime component.
 */
bool
gomi::gomi_t::IsSpecialBin (const gomi::bin_decl_t& bin)
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
	LOG(INFO) << "{ "
		  "\"pluginType\": \"" << plugin_type_ << "\""
		", \"pluginId\": \"" << plugin_id_ << "\""
		", \"instance\": " << instance_ <<
		", \"version\": \"" << version_major << '.' << version_minor << '.' << version_build << "\""
		", \"build\": { "
			  "\"date\": \"" << build_date << "\""
			", \"time\": \"" << build_time << "\""
			", \"system\": \"" << build_system << "\""
			", \"machine\": \"" << build_machine << "\""
			" }"
		" }";

	if (!config_.parseDomElement (vpf_config.getXmlConfigData())) {
		is_shutdown_ = true;
		throw vpf::UserPluginException ("Invalid configuration, aborting.");
	}
	if (!Init()) {
		Clear();
		is_shutdown_ = true;
		throw vpf::UserPluginException ("Initialization failed, aborting.");
	}
}

bool
gomi::gomi_t::Init()
{
	std::vector<std::string> symbolmap;

	LOG(INFO) << config_;

	try {
/* FlexRecord cursor */
		manager_ = FlexRecDefinitionManager::GetInstance (nullptr);
		work_area_.reset (manager_->AcquireWorkArea(), [this](FlexRecWorkAreaElement* work_area){ manager_->ReleaseWorkArea (work_area); });
		view_element_.reset (manager_->AcquireView(), [this](FlexRecViewElement* view_element){ manager_->ReleaseView (view_element); });

		if (!manager_->GetView ("Trade", view_element_->view)) {
			LOG(ERROR) << "FlexRecDefinitionManager::GetView failed";
			return false;
		}
	} catch (std::exception& e) {
		LOG(ERROR) << "FlexRecord::Exception: { "
			"\"What\": \"" << e.what() << "\""
			" }";
		return false;
	}

/* Boost time zone database. */
	try {
		tzdb_.load_from_file (config_.tzdb);
/* default time zone */
		TZ_ = tzdb_.time_zone_from_region (config_.tz);
		if (nullptr == TZ_) {
			LOG(ERROR) << "TZ not listed within configured time zone specifications.";
			return false;
		}
	} catch (boost::local_time::data_not_accessible& e) {
		LOG(ERROR) << "Time zone specifications cannot be loaded: " << e.what();
		return false;
	} catch (boost::local_time::bad_field_count& e) {
		LOG(ERROR) << "Time zone specifications malformed: " << e.what();
		return false;
	}

	try {
/* /bin/ declarations */
		const auto day_count = std::stoi (config_.day_count);
		for (auto it = config_.bins.begin(); it != config_.bins.end(); ++it)
		{
			bin_decl_t bin;
			if (!ParseBinDecl (*it, TZ_, day_count, &bin)) {
				LOG(ERROR) << "Cannot parse bin delcs.";
				return false;
			}
			bins_.insert (bin);
		}
	} catch (std::exception& e) {
		LOG(ERROR) << "BinDecl::Exception: { "
			"\"What\": \"" << e.what() << "\" }";
		return false;
	}

	try {
/* "Symbol map" a.k.a. list of Reuters Instrument Codes (RICs). */
		if (!ReadSymbolMap (config_.symbolmap, &symbolmap)) {
			LOG(ERROR) << "Cannot read symbolmap file: " << config_.symbolmap;
			return false;
		}
	} catch (std::exception& e) {
		LOG(ERROR) << "SymbolMap::Exception: { "
			"\"What\": \"" << e.what() << "\" }";
		return false;
	}

/** RFA initialisation. **/
	try {
/* RFA context. */
		rfa_.reset (new rfa_t (config_));
		if (!(bool)rfa_ || !rfa_->Init())
			return false;

/* RFA asynchronous event queue. */
		const RFA_String eventQueueName (config_.event_queue_name.c_str(), 0, false);
/* translation: event_queue_.destroy() */
		event_queue_.reset (rfa::common::EventQueue::create (eventQueueName), std::mem_fn (&rfa::common::EventQueue::destroy));
		if (!(bool)event_queue_)
			return false;

/* RFA logging. */
		log_.reset (new logging::LogEventProvider (config_, event_queue_));
		if (!(bool)log_ || !log_->Register())
			return false;

/* RFA provider. */
		provider_.reset (new provider_t (config_, rfa_, event_queue_));
		if (!(bool)provider_ || !provider_->Init())
			return false;

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
			if (!provider_->CreateItemStream (ss.str().c_str(), stream))
				return false;
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
			std::pair<std::vector<std::shared_ptr<bin_t>>,
				  std::vector<std::shared_ptr<archive_stream_t>>> v;

			for (auto jt = stream_vector_.begin();
				jt != stream_vector_.end();
				++jt)
			{
				auto bin = std::make_shared<bin_t> (*it, (*jt)->symbol_name.c_str(), kDefaultLastPriceField, kDefaultTickVolumeField);
				assert ((bool)bin);

				v.first.push_back (bin);

/* analytic publish stream */
				std::ostringstream ss;
				ss << bin->GetSymbolName() << '.' << it->bin_name	<< config_.suffix;
				auto stream = std::make_shared<archive_stream_t> (bin);
				assert ((bool)stream);
				if (!provider_->CreateItemStream (ss.str().c_str(), stream))
					return false;
				v.second.push_back (stream);

/* add reference to this archive to realtime stream */
				if (IsSpecialBin (*it)) {
					const auto& realtime_fids = config_.realtime_fids[it->bin_name];
					(*jt)->special.emplace_back (std::make_pair (realtime_fids, stream));
				} else {
					(*jt)->last_10min.second.emplace (std::make_pair (*it, stream));
				}
			}

			query_vector_.emplace (std::make_pair (*it, v));
		}

/* Pre-allocate memory buffer for payload iterator */
		const long maximum_data_size = std::atol (config_.maximum_data_size.c_str());
		CHECK (maximum_data_size > 0);
		single_write_it_.initialize (fields_, maximum_data_size);
		CHECK (single_write_it_.isInitialized());

/* daily bar archives */
/* pre-allocate each day storage */
	} catch (rfa::common::InvalidUsageException& e) {
		LOG(ERROR) << "InvalidUsageException: { "
			  "\"Severity\": \"" << severity_string (e.getSeverity()) << "\""
			", \"Classification\": \"" << classification_string (e.getClassification()) << "\""
			", \"StatusText\": \"" << e.getStatus().getStatusText() << "\""
			" }";
		return false;
	} catch (rfa::common::InvalidConfigurationException& e) {
		LOG(ERROR) << "InvalidConfigurationException: { "
			  "\"Severity\": \"" << severity_string (e.getSeverity()) << "\""
			", \"Classification\": \"" << classification_string (e.getClassification()) << "\""
			", \"StatusText\": \"" << e.getStatus().getStatusText() << "\""
			", \"ParameterName\": \"" << e.getParameterName() << "\""
			", \"ParameterValue\": \"" << e.getParameterValue() << "\""
			" }";
		return false;
	} catch (std::exception& e) {
		LOG(ERROR) << "Rfa::Exception: { "
			"\"What\": \"" << e.what() << "\" }";
		return false;
	}

	try {
/* No main loop inside this thread, must spawn new thread for message pump. */
		event_pump_.reset (new event_pump_t (event_queue_));
		if (!(bool)event_pump_)
			return false;

		event_thread_.reset (new boost::thread ([this](){ event_pump_->Run(); }));
		if (!(bool)event_thread_)
			return false;
	} catch (std::exception& e) {
		LOG(ERROR) << "EventPump::Exception: { "
			"\"What\": \"" << e.what() << "\" }";
		return false;
	}

	try {
/* Spawn SNMP implant. */
		if (config_.is_snmp_enabled) {
			snmp_agent_.reset (new snmp_agent_t (*this));
			if (!(bool)snmp_agent_)
				return false;
		}
	} catch (std::exception& e) {
		LOG(ERROR) << "SnmpAgent::Exception: { "
			"\"What\": \"" << e.what() << "\" }";
		return false;
	}

	try {
/* Register Tcl commands with TREP-VA search engine and await callbacks. */
		if (!RegisterTclApi (getId()))
		return false;
	} catch (std::exception& e) {
		LOG(ERROR) << "TclApi::Exception: { "
			"\"What\": \"" << e.what() << "\" }";
		return false;
	}

	try {
/* Timer for periodic publishing.
 */
		boost::posix_time::ptime due_time;
		if (!GetDueTime (TZ_, &due_time)) {
			LOG(ERROR) << "Cannot calculate timer due time.";
			return false;
		}
/* convert Boost Posix Time into a Chrono time point */
		const auto time = to_unix_epoch<std::time_t> (due_time);
		const auto tp = boost::chrono::system_clock::from_time_t (time);

		const boost::chrono::seconds td (std::stoul (config_.interval));
		timer_.reset (new time_pump_t<boost::chrono::system_clock> (tp, td, this));
		if (!(bool)timer_) {
			LOG(ERROR) << "Cannot create time pump.";
			return false;
		}
		timer_thread_.reset (new boost::thread ([this](){ timer_->Run(); }));
		if (!(bool)timer_thread_) {
			LOG(ERROR) << "Cannot spawn timer thread.";
			return false;
		}

		LOG(INFO) << "Added periodic timer, interval " << td.count() << " seconds"
			<< ", due time " << boost::posix_time::to_simple_string (due_time);
	} catch (std::exception& e) {
		LOG(ERROR) << "TimerPump::Exception: { "
			"\"What\": \"" << e.what() << "\" }";
		return false;
	}

	LOG(INFO) << "Init complete, awaiting queries.";
	return true;
}

void
gomi::gomi_t::Clear()
{
/* Stop generating new events. */
	if (timer_thread_) {
		timer_thread_->interrupt();
		timer_thread_->join();
	}	
	timer_thread_.reset();
	timer_.reset();

/* Close SNMP agent. */
	snmp_agent_.reset();

/* Signal message pump thread to exit. */
	if ((bool)event_queue_)
		event_queue_->deactivate();
/* Drain and close event queue. */
	if ((bool)event_thread_)
		event_thread_->join();

/* Release everything with an RFA dependency. */
	event_thread_.reset();
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
	UnregisterTclApi (getId());
	Clear();
	LOG(INFO) << "Runtime summary: {"
		    " \"tclQueryReceived\": " << cumulative_stats_[GOMI_PC_TCL_QUERY_RECEIVED] <<
		   ", \"timerQueryReceived\": " << cumulative_stats_[GOMI_PC_TIMER_QUERY_RECEIVED] <<
		" }";
	LOG(INFO) << "Instance closed.";
	vpf::AbstractUserPlugin::destroy();
}

/* callback from periodic timer.
 */
bool
gomi::gomi_t::OnTimer (
	const boost::chrono::time_point<boost::chrono::system_clock>& t
	)
{
/* calculate timer accuracy, typically 15-1ms with default timer resolution.
 */
	if (DLOG_IS_ON(INFO)) {
		using namespace boost::chrono;
		auto now = system_clock::now();
		auto ms = duration_cast<milliseconds> (now - t);
		if (0 == ms.count()) {
			LOG(INFO) << "delta " << duration_cast<microseconds> (now - t).count() << "us";
		} else {
			LOG(INFO) << "delta " << ms.count() << "ms";
		}
	}

	cumulative_stats_[GOMI_PC_TIMER_QUERY_RECEIVED]++;

/* Prevent overlapped queries. */
	boost::unique_lock<boost::shared_mutex> lock (query_mutex_, boost::try_to_lock_t());
	if (!lock.owns_lock()) {
		LOG(WARNING) << "Periodic refresh aborted due to running query.";
		return true;
	}

	try {
		TimeRefresh();
	} catch (rfa::common::InvalidUsageException& e) {
		LOG(ERROR) << "InvalidUsageException: { "
			  "\"Severity\": \"" << severity_string (e.getSeverity()) << "\""
			", \"Classification\": \"" << classification_string (e.getClassification()) << "\""
			", \"StatusText\": \"" << e.getStatus().getStatusText() << "\""
			" }";
	} catch (std::exception& e) {
		LOG(ERROR) << "TimeRefresh::Exception: { "
			"\"What\": \"" << e.what() << "\" }";
		return false;
	}
	return true;
}

/* Calculate next ideal due time.
 * /or the next rounded minute/
 */
bool
gomi::gomi_t::GetDueTime (
	const boost::local_time::time_zone_ptr& tz,
	boost::posix_time::ptime* t
	)
{
	if (bins_.empty())
		return false;

	using namespace boost::posix_time;
	const auto now_utc     = second_clock::universal_time();
	const auto next_minute = now_utc + minutes (1);

	*t = ptime (next_minute.date(),
		    time_duration (next_minute.time_of_day().hours(), next_minute.time_of_day().minutes(), 0, 0));
	return true;
}

/* Calculate the next bin close timestamp for the requested timezone.
 */
bool
gomi::gomi_t::GetNextBinClose (
	const boost::local_time::time_zone_ptr& tz,
	boost::posix_time::ptime* t
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
	bin_decl_t bin_decl;
	bin_decl.bin_end = td;

	auto it = bins_.upper_bound (bin_decl);	/* > td (upper-bound) not >= td (lower-bound) */
	if (bins_.end() == it) {
		it = bins_.begin();	/* wrap around */
		now_tz += hours (24);
	}

/* convert time-of-day into Microsoft FILETIME */
	const local_date_time next_close (now_tz.local_time().date(), it->bin_end, tz, local_date_time::NOT_DATE_TIME_ON_ERROR);
	DCHECK (!next_close.is_not_a_date_time());

	*t = next_close.utc_time();
	return true;
}

/* Calculate the time-of-day of the last bin close.
 */
bool
gomi::gomi_t::GetLastBinClose (
	const boost::local_time::time_zone_ptr& tz,
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
	bin_decl_t bin_decl;
	bin_decl.bin_end = td;

	auto it = --(bins_.upper_bound (bin_decl));	/* > td (upper-bound) not >= td (lower-bound) */
	*last_close = it->bin_end;
	return true;
}

/* http://msdn.microsoft.com/en-us/library/4ey61ayt.aspx */
#define CTIME_LENGTH	26

/* Refresh based upon time-of-day.  Archive & realtime bins.
 */
bool
gomi::gomi_t::TimeRefresh()
{
	using namespace boost::posix_time;
	const ptime t0 (microsec_clock::universal_time());
	last_activity_ = t0;

	LOG(INFO) << "TimeRefresh";

/* Calculate affected bins */
	bin_decl_t bin_decl;
	bin_decl.bin_tz = TZ_;
	if (!GetLastBinClose (TZ_, &bin_decl.bin_end)) {
		LOG(ERROR) << "Cannot calculate last bin close time of day.";
		return false;
	}

/* prevent replay except on daylight savings */
	if (!last_refresh_.is_not_a_date_time() && last_refresh_ == bin_decl.bin_end) {		
/* next bin */
		auto it = bins_.find (bin_decl);
		do {
			if (++it == bins_.end())
				it = bins_.begin();
		} while (it->bin_end == bin_decl.bin_end);
		LOG(INFO) << "Aborting query, last bin close time same as last refresh, next bin: " << *it;
		return false;
	}

/* constant iterator in C++11 */
	unsigned bin_refresh_count = 0;
	for (auto it = bins_.find (bin_decl);
		it != bins_.end() && it->bin_end == bin_decl.bin_end;
		++it)
	{
		BinRefresh (*it);
		++bin_refresh_count;
	}

	if (0 == bin_refresh_count) {
		auto it = bins_.find (bin_decl), jt = it;
		do {
			if (++it == bins_.end())
				it = bins_.begin();
		} while (it->bin_end == bin_decl.bin_end);
		if (jt == bins_.end())
			LOG(INFO) << "No bins to re-calculate, first bin: " << *it;
		else
			LOG(INFO) << "No bins to re-calculate, previous bin: " << *jt << ", next bin: " << *it;
		return false;
	}

/* save this iteration time */
	last_refresh_ = bin_decl.bin_end;

	SummaryRefresh (last_refresh_);

/* Timing */
	const ptime t1 (microsec_clock::universal_time());
	const time_duration td = t1 - t0;
	LOG(INFO) << "Refresh complete " << td.total_milliseconds() << "ms";
	if (td < min_refresh_time_) min_refresh_time_ = td;
	if (td > max_refresh_time_) max_refresh_time_ = td;
	total_refresh_time_ += td;
	return true;
}

/* Refresh todays analytic content.
 */
bool
gomi::gomi_t::DayRefresh()
{
	using namespace boost::posix_time;
	using namespace boost::local_time;
	const ptime t0 (microsec_clock::universal_time());
	last_activity_ = t0;

	LOG(INFO) << "DayRefresh";

/* Calculate affected bins */
	const auto now_utc = second_clock::universal_time();
	const local_date_time now_tz (now_utc, TZ_);
	const auto now_td = now_tz.local_time().time_of_day();

/* constant iterator in C++11 */
	for (auto it = query_vector_.begin(); it != query_vector_.end(); ++it)
		for (auto jt = it->second.first.begin(); jt != it->second.first.end(); ++jt)
			(*jt)->Clear();

	for (auto it = bins_.begin(); it != bins_.end() && it->bin_end <= now_td; ++it) {
		BinRefresh (*it);

/* save this iteration time to prevent replay */
		last_refresh_ = it->bin_end;
	}

	SummaryRefresh (last_refresh_);

/* Timing */
	const ptime t1 (microsec_clock::universal_time());
	const time_duration td = t1 - t0;
	LOG(INFO) << "Refresh complete " << td.total_milliseconds() << "ms";
	if (td < min_refresh_time_) min_refresh_time_ = td;
	if (td > max_refresh_time_) max_refresh_time_ = td;
	total_refresh_time_ += td;
	return true;
}

/* Recalculate 24-hour period analytic content.
 */
bool
gomi::gomi_t::Recalculate()
{
	using namespace boost::posix_time;
	using namespace boost::local_time;
	const ptime t0 (microsec_clock::universal_time());
	last_activity_ = t0;

	LOG(INFO) << "Recalculate";

/* Calculate affected bins */
	const auto now_utc = second_clock::universal_time();
	const local_date_time now_tz (now_utc, TZ_);
	const auto now_td = now_tz.local_time().time_of_day();

/* constant iterator in C++11 */
	for (auto it = query_vector_.begin(); it != query_vector_.end(); ++it)
		for (auto jt = it->second.first.begin(); jt != it->second.first.end(); ++jt)
			(*jt)->Clear();

	std::for_each (bins_.begin(), bins_.end(), [this](const bin_decl_t& bin_decl) {
		BinRefresh (bin_decl);
	});

/* clear iteration time */
	last_refresh_ = boost::posix_time::not_a_date_time;

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
gomi::gomi_t::BinRefresh (
	const gomi::bin_decl_t& ref_bin
	)
{
/* fixed /bin/ parameters */
	bin_decl_t bin_decl (ref_bin);
	bin_decl.bin_tz = TZ_;
	bin_decl.bin_day_count = std::stoi (config_.day_count);

	LOG(INFO) << "BinRefresh (bin: " << bin_decl << ")";

/* refreshes last /x/ business days, i.e. executing on a holiday will only refresh the cache contents */
	auto& v = query_vector_[bin_decl];

	DVLOG(3) << "processing query.";
	using namespace boost::local_time;
	const auto now_in_tz = local_sec_clock::local_time (bin_decl.bin_tz);
	const auto today_in_tz = now_in_tz.local_time().date();
	auto work_area = work_area_.get();
	auto view_element = view_element_.get();	
	std::for_each (v.first.begin(), v.first.end(), [today_in_tz, work_area, view_element](std::shared_ptr<bin_t>& it) {
		it->Calculate (today_in_tz, work_area, view_element);
	});
	DVLOG(3) << "query complete, publishing analytic results.";

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
	fields_.setAssociatedMetaInfo (provider_->GetRwfMajorVersion(), provider_->GetRwfMinorVersion());
	fields_.setInfo (kDictionaryId, kFieldListId);

/* TIMEACT & ACTIV_DATE */
	struct tm _tm;
	CHECK (!v.first.empty());
	const auto& first = v.first[0];
	__time32_t time32 = to_unix_epoch<__time32_t> (first->GetCloseTime());
	_gmtime32_s (&_tm, &time32);

	rfa::common::RespStatus status;
/* Item interaction state: Open, Closed, ClosedRecover, Redirected, NonStreaming, or Unspecified. */
	status.setStreamState (rfa::common::RespStatus::OpenEnum);
/* Data quality state: Ok, Suspect, or Unspecified. */
	status.setDataState (rfa::common::RespStatus::OkEnum);
/* Error code, e.g. NotFound, InvalidArgument, ... */
	status.setStatusCode (rfa::common::RespStatus::NoneEnum);
	response.setRespStatus (status);

	std::for_each (v.second.begin(), v.second.end(), [&](std::shared_ptr<archive_stream_t>& stream)
	{
		VLOG(1) << "Publishing to stream " << stream->rfa_name;
		attribInfo.setName (stream->rfa_name);

/* Clear required for SingleWriteIterator state machine. */
		auto& it = single_write_it_;
		DCHECK (it.isInitialized());
		it.clear();
		it.start (fields_);

/* For each field set the Id via a FieldEntry bound to the iterator followed by setting the data.
 * The iterator API provides setters for common types excluding 32-bit floats, with fallback to 
 * a generic DataBuffer API for other types or support of pre-calculated values.
 */
		rfa::data::FieldEntry field (false);
/* TIMACT */
		field.setFieldID (kRdmTimeOfUpdateId);
		it.bind (field);
		it.setTime (_tm.tm_hour, _tm.tm_min, _tm.tm_sec, 0 /* ms */);

/* PRICE field is a rfa::Real64 value specified as <mantissa> × 10?.
 * Rfa deprecates setting via <double> data types so we create a mantissa from
 * source value and consider that we publish to 6 decimal places.
 */
/* PCTCHG_10D */
		field.setFieldID (config_.archive_fids.Rdm10DayPercentChangeId);
		it.bind (field);
		it.setReal (portware::mantissa (stream->bin->GetTenDayPercentageChange()), rfa::data::ExponentNeg6);
/* PCTCHG_15D */
		field.setFieldID (config_.archive_fids.Rdm15DayPercentChangeId);
		it.bind (field);
		it.setReal (portware::mantissa (stream->bin->GetFifteenDayPercentageChange()), rfa::data::ExponentNeg6);
/* PCTCHG_20D */
		field.setFieldID (config_.archive_fids.Rdm20DayPercentChangeId);
		it.bind (field);
		it.setReal (portware::mantissa (stream->bin->GetTwentyDayPercentageChange()), rfa::data::ExponentNeg6);
/* PCTCHG_10T */
		field.setFieldID (config_.archive_fids.Rdm10TradingDayPercentChangeId);
		it.bind (field);
		it.setReal (portware::mantissa (stream->bin->GetTenTradingDayPercentageChange()), rfa::data::ExponentNeg6);
/* PCTCHG_15T */
		field.setFieldID (config_.archive_fids.Rdm15TradingDayPercentChangeId);
		it.bind (field);
		it.setReal (portware::mantissa (stream->bin->GetFifteenTradingDayPercentageChange()), rfa::data::ExponentNeg6);
/* PCTCHG_20T */
		field.setFieldID (config_.archive_fids.Rdm20TradingDayPercentChangeId);
		it.bind (field);
		it.setReal (portware::mantissa (stream->bin->GetTwentyTradingDayPercentageChange()), rfa::data::ExponentNeg6);
/* VMA_20D */
		field.setFieldID (config_.archive_fids.RdmAverageVolumeId);
		it.bind (field);
		it.setReal (stream->bin->GetAverageVolume(), rfa::data::Exponent0);
/* VMA_20TD */
		field.setFieldID (config_.archive_fids.RdmAverageNonZeroVolumeId);
		it.bind (field);
		it.setReal (stream->bin->GetAverageNonZeroVolume(), rfa::data::Exponent0);
/* TRDCNT_20D */
		field.setFieldID (config_.archive_fids.RdmTotalMovesId);
		it.bind (field);
		it.setReal (stream->bin->GetTotalMoves(), rfa::data::Exponent0);
/* HICNT_20D */
		field.setFieldID (config_.archive_fids.RdmMaximumMovesId);
		it.bind (field);
		it.setReal (stream->bin->GetMaximumMoves(), rfa::data::Exponent0);
/* LOCNT_20D */
		field.setFieldID (config_.archive_fids.RdmMinimumMovesId);
		it.bind (field);
		it.setReal (stream->bin->GetMinimumMoves(), rfa::data::Exponent0);
/* SMCNT_20D */
		field.setFieldID (config_.archive_fids.RdmSmallestMovesId);
		it.bind (field);
		it.setReal (stream->bin->GetSmallestMoves(), rfa::data::Exponent0);
/* ACTIV_DATE */
		field.setFieldID (kRdmActiveDateId);
		it.bind (field);
		const uint16_t year  = /* rfa(yyyy) */ 1900 + _tm.tm_year /* tm(yyyy-1900 */;
		const uint8_t  month = /* rfa(1-12) */    1 + _tm.tm_mon  /* tm(0-11) */;
		const uint8_t  day   = /* rfa(1-31) */        _tm.tm_mday /* tm(1-31) */;
		it.setDate (year, month, day);

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
			LOG(ERROR) << "respMsg::validateMsg: { \"warningText\": \"" << warningText << "\" }";
		} else {
			assert (rfa::message::MsgValidationOk == validation_status);
		}
#endif
		provider_->Send (stream.get(), &response);
	});
	return true;
}

/* Refresh summary bin analytics, i.e. the realtime set of bins including
 * a special last 10-minute bin derived from the current time-of-day.
 */
bool
gomi::gomi_t::SummaryRefresh (const boost::posix_time::time_duration& time_of_day)
{
	using namespace boost::posix_time;
	using namespace boost::local_time;

	ptime close_time;
	bin_decl_t last_10min_bin;
	bool has_last_10min_bin = false;

	{
		auto it = query_vector_.begin();
		for (; it != query_vector_.end(); ++it)
		{
/* check every earlier bin for 10-minute publish */
			if (time_of_day >= it->first.bin_end && !IsSpecialBin (it->first)) {
				auto jt = it->second.first.begin();
				if ((bool)*(*jt)) {
					has_last_10min_bin = true;
					last_10min_bin = it->first;
				} else {
					LOG(ERROR) << "10-min bin " << to_simple_string (it->first.bin_end) << " has not been calculated.";
				}
			}

/* find last bin from provided time-of-day */
			if (time_of_day == it->first.bin_end) {
/* test for valid analytic data */
				auto jt = it->second.first.begin();
				if ((bool)*(*jt)) {
/* save full time & date */
					close_time = (*jt)->GetCloseTime();				
				} else {
					LOG(ERROR) << "bin " << to_simple_string (it->first.bin_end) << " has not been calculated.";
				}
				break;
			} else if (time_of_day < it->first.bin_end) {
				LOG(ERROR) << "Summary time-of-day " << to_simple_string (time_of_day) << " is before bin close time " << to_simple_string (it->first.bin_end);
				break;
			}
		}

/* no prior analytics found */
		if (close_time.is_not_a_date_time())
			return false;

/* find empty 10-min bin to use */
		if (!has_last_10min_bin) {
			for (auto jt = it; jt != query_vector_.end(); ++jt) {
				if (!IsSpecialBin (jt->first)) {
					has_last_10min_bin = true;
					last_10min_bin = jt->first;
					break;
				}
			}

			if (!has_last_10min_bin) {
				LOG(ERROR) << "Cannot find any 10-min bin.";
			}
		}

/* clear out post-bins */
		for (; it != query_vector_.end(); ++it) {
			if (time_of_day == it->first.bin_end)
				continue;

			for (auto jt = it->second.first.begin(); jt != it->second.first.end(); ++jt)
				if ((bool)*(*jt))
					(*jt)->Clear();
		}
	}

	LOG(INFO) << "Summary timestamp: " << to_simple_string (close_time);

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
	fields_.setAssociatedMetaInfo (provider_->GetRwfMajorVersion(), provider_->GetRwfMinorVersion());
	fields_.setInfo (kDictionaryId, kFieldListId);

/* TIMEACT */
	struct tm _tm;
	__time32_t time32 = to_unix_epoch<__time32_t> (close_time);
	_gmtime32_s (&_tm, &time32);

	rfa::common::RespStatus status;
/* Item interaction state: Open, Closed, ClosedRecover, Redirected, NonStreaming, or Unspecified. */
	status.setStreamState (rfa::common::RespStatus::OpenEnum);
/* Data quality state: Ok, Suspect, or Unspecified. */
	status.setDataState (rfa::common::RespStatus::OkEnum);
/* Error code, e.g. NotFound, InvalidArgument, ... */
	status.setStatusCode (rfa::common::RespStatus::NoneEnum);
	response.setRespStatus (status);

	std::for_each (stream_vector_.begin(), stream_vector_.end(), [&](std::shared_ptr<realtime_stream_t>& stream)
	{
		VLOG(1) << "publish: " << stream->rfa_name;
		attribInfo.setName (stream->rfa_name);

/* Clear required for SingleWriteIterator state machine. */
		auto& it = single_write_it_;
		DCHECK (it.isInitialized());
		it.clear();
		it.start (fields_);

		rfa::data::FieldEntry field (false);
/* TIMACT */
		field.setFieldID (kRdmTimeOfUpdateId);
		it.bind (field);
		it.setTime (_tm.tm_hour, _tm.tm_min, _tm.tm_sec, 0 /* ms */);

		auto add_fidset = [&it](const fidset_t& fids, std::shared_ptr<bin_t>& bin)
		{
			rfa::data::FieldEntry field (false);
/* PCTCHG_10D */
			field.setFieldID (fids.Rdm10DayPercentChangeId);
			it.bind (field);
			it.setReal (portware::mantissa (bin->GetTenDayPercentageChange()), rfa::data::ExponentNeg6);
/* PCTCHG_15D */
			field.setFieldID (fids.Rdm15DayPercentChangeId);
			it.bind (field);
			it.setReal (portware::mantissa (bin->GetFifteenDayPercentageChange()), rfa::data::ExponentNeg6);
/* PCTCHG_20D */
			field.setFieldID (fids.Rdm20DayPercentChangeId);
			it.bind (field);
			it.setReal (portware::mantissa (bin->GetTwentyDayPercentageChange()), rfa::data::ExponentNeg6);
/* PCTCHG_10T */
			field.setFieldID (fids.Rdm10TradingDayPercentChangeId);
			it.bind (field);
			it.setReal (portware::mantissa (bin->GetTenTradingDayPercentageChange()), rfa::data::ExponentNeg6);
/* PCTCHG_15T */
			field.setFieldID (fids.Rdm15TradingDayPercentChangeId);
			it.bind (field);
			it.setReal (portware::mantissa (bin->GetFifteenTradingDayPercentageChange()), rfa::data::ExponentNeg6);
/* PCTCHG_20T */
			field.setFieldID (fids.Rdm20TradingDayPercentChangeId);
			it.bind (field);
			it.setReal (portware::mantissa (bin->GetTwentyTradingDayPercentageChange()), rfa::data::ExponentNeg6);
/* VMA_20D */
			field.setFieldID (fids.RdmAverageVolumeId);
			it.bind (field);
			it.setReal (bin->GetAverageVolume(), rfa::data::Exponent0);
/* VMA_20TD */
			field.setFieldID (fids.RdmAverageNonZeroVolumeId);
			it.bind (field);
			it.setReal (bin->GetAverageNonZeroVolume(), rfa::data::Exponent0);
/* TRDCNT_20D */
			field.setFieldID (fids.RdmTotalMovesId);
			it.bind (field);
			it.setReal (bin->GetTotalMoves(), rfa::data::Exponent0);
/* HICNT_20D */
			field.setFieldID (fids.RdmMaximumMovesId);
			it.bind (field);
			it.setReal (bin->GetMaximumMoves(), rfa::data::Exponent0);
/* LOCNT_20D */
			field.setFieldID (fids.RdmMinimumMovesId);
			it.bind (field);
			it.setReal (bin->GetMinimumMoves(), rfa::data::Exponent0);
/* SMCNT_20D */
			field.setFieldID (fids.RdmSmallestMovesId);
			it.bind (field);
			it.setReal (bin->GetSmallestMoves(), rfa::data::Exponent0);
		};

/* every special named bin analytic */
		std::for_each (stream->special.begin(), stream->special.end(), [&](std::pair<fidset_t, std::shared_ptr<archive_stream_t>> archive)
		{
			add_fidset (archive.first, archive.second->bin);
		});

/* last 10-minute special bin */
		if (has_last_10min_bin)
			add_fidset (stream->last_10min.first, stream->last_10min.second[last_10min_bin]->bin);

/* ACTIV_DATE */
		field.setFieldID (kRdmActiveDateId);
		it.bind (field);
		const uint16_t year  = /* rfa(yyyy) */ 1900 + _tm.tm_year /* tm(yyyy-1900 */;
		const uint8_t  month = /* rfa(1-12) */    1 + _tm.tm_mon  /* tm(0-11) */;
		const uint8_t  day   = /* rfa(1-31) */        _tm.tm_mday /* tm(1-31) */;
		it.setDate (year, month, day);

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
			LOG(ERROR) << "respMsg::validateMsg: { \"warningText\": \"" << warningText << "\" }";
		} else {
			assert (rfa::message::MsgValidationOk == validation_status);
		}
#endif
		provider_->Send (stream.get(), &response);
	});
	return true;
}

/* eof */