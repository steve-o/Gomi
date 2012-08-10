/* A basic Velocity Analytics User-Plugin to exporting a new Tcl command and
 * periodically publishing out to ADH via RFA using RDM/MarketPrice.
 */

#include "gomi.hh"

#define __STDC_FORMAT_MACROS
#include <cstdint>
#include <inttypes.h>

/* Boost Posix Time */
#include <boost/date_time/gregorian/gregorian_types.hpp>

#include <windows.h>

/* ZeroMQ messaging middleware. */
#include <zmq.h>

#include "chromium/file_util.hh"
#include "chromium/logging.hh"
#include "chromium/string_split.hh"
#include "googleurl/url_parse.h"
#include "business_day_iterator.hh"
#include "gomi_bin.hh"
#include "snmp_agent.hh"
#include "error.hh"
#include "rfa_logging.hh"
#include "rfaostream.hh"
#include "provider.pb.h"
#include "version.hh"
#include "portware.hh"

/* RDM Usage Guide: Section 6.5: Enterprise Platform
 * For future compatibility, the DictionaryId should be set to 1 by providers.
 * The DictionaryId for the RDMFieldDictionary is 1.
 */
static const int kDictionaryId = 1;

/* RDM: Absolutely no idea. */
static const int kFieldListId = 3;

/* RDM FIDs. */
static const int kRdmTimeOfUpdateId		= 5;
static const int kRdmActiveDateId		= 17;

/* FlexRecord Quote identifier. */
static const uint32_t kQuoteId = 40002;

/* Default FlexRecord fields. */
static const char* kDefaultLastPriceField = "LastPrice";
static const char* kDefaultTickVolumeField = "TickVolume";

/* RIC request fields. */
static const char* kOpen     = "open";
static const char* kClose    = "close";
static const char* kTimezone = "tz";
static const char* kOffset   = "offset";
static const char* kDays     = "days";

/* Request limits */
static const unsigned kMaximumDayOffset = 90;
static const unsigned kMaximumDayCount  = 90;

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

/* Is today<date> a business day, per TBSDK.  Assumes local calendar as per TBSDK.
 */
static
bool
is_business_day (
	const boost::gregorian::date d
	)
{
	BusinessDayInfo bd;
	CHECK (!d.is_not_a_date());
	const auto time32 = to_unix_epoch<__time32_t> (boost::posix_time::ptime (d));
	return (0 != TBPrimitives::BusinessDay (time32, &bd));
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

/* Worker thread for processing refresh requests.
 */
class gomi::worker_t
{
public:
	worker_t (
		std::shared_ptr<provider_t> provider,
		const boost::unordered_map<std::string, std::shared_ptr<archive_stream_t>>& directory,
		const boost::local_time::tz_database& tzdb,
		const boost::local_time::time_zone_ptr TZ,
		const config_t& config,
		std::shared_ptr<void>& zmq_context,
		unsigned id
	) :
		id_ (id),
		zmq_context_ (zmq_context),
		manager_ (nullptr),
		response_ (false),	/* reference */
		fields_ (false),
		attribInfo_ (false),
		provider_ (provider),
		directory_ (directory),
		tzdb_ (tzdb),
		TZ_ (TZ),
		config_ (config)
	{
/* Set logger ID */
		std::ostringstream ss;
		ss << "Worker " << std::hex << std::setiosflags (std::ios_base::showbase) << id << ':';
		prefix_.assign (ss.str());
	}

	~worker_t()
	{
/* shutdown socket before context */
		CHECK (receiver_.use_count() <= 1);
		receiver_.reset();
		zmq_context_.reset();
	}

	bool Init()
	{
		std::function<int(void*)> zmq_close_deleter = zmq_close;
		int rc;

		try {
/* Setup 0mq sockets */
			receiver_.reset (zmq_socket (zmq_context_.get(), ZMQ_PULL), zmq_close_deleter);
			CHECK((bool)receiver_);
			rc = zmq_connect (receiver_.get(), "inproc://gomi/refresh");
			CHECK(0 == rc);
/* Also bind for terminating interrupt. */
			rc = zmq_connect (receiver_.get(), "inproc://gomi/abort");
			CHECK(0 == rc);
		} catch (std::exception& e) {
			LOG(ERROR) << prefix_ << "ZeroMQ::Exception: { "
				"\"What\": \"" << e.what() << "\""
				" }";
			return false;
		}

		try {
/* Pre-allocate memory buffer for RFA payload iterator */
			fields_ = std::make_shared<rfa::data::FieldList> ();
			CHECK ((bool)fields_);

			CHECK (config_.maximum_data_size > 0);
			single_write_it_ = std::make_shared<rfa::data::SingleWriteIterator> ();
			CHECK ((bool)single_write_it_);
			single_write_it_->initialize (*fields_.get(), config_.maximum_data_size);
			CHECK (single_write_it_->isInitialized());
		} catch (rfa::common::InvalidUsageException& e) {
			LOG(ERROR) << prefix_ << "InvalidUsageException: { "
				  "\"Severity\": \"" << severity_string (e.getSeverity()) << "\""
				", \"Classification\": \"" << classification_string (e.getClassification()) << "\""
				", \"StatusText\": \"" << e.getStatus().getStatusText() << "\""
				" }";
			return false;
		}

		try {
/* FlexRecord cursor */
			manager_ = FlexRecDefinitionManager::GetInstance (nullptr);
			work_area_.reset (manager_->AcquireWorkArea(), [this](FlexRecWorkAreaElement* work_area){ manager_->ReleaseWorkArea (work_area); });
			view_element_.reset (manager_->AcquireView(), [this](FlexRecViewElement* view_element){ manager_->ReleaseView (view_element); });

			if (!manager_->GetView ("Trade", view_element_->view)) {
				LOG(ERROR) << prefix_ << "FlexRecDefinitionManager::GetView failed";
			}
		} catch (std::exception& e) {
			LOG(ERROR) << prefix_ << "FlexRecord::Exception: { "
				"\"What\": \"" << e.what() << "\""
				" }";
			return false;
		}

		return true;
	}

	void Run (void)
	{
		LOG(INFO) << prefix_ << "Accepting requests.";
		while (true)
		{
			if (!GetRequest (&request_))
				continue;
			if (request_.msg_type() == provider::Request::MSG_ABORT) {
				LOG(INFO) << prefix_ << "Received interrupt request.";
				break;
			}
			if (!(request_.msg_type() == provider::Request::MSG_REFRESH
				&& request_.has_refresh()))
			{
				LOG(ERROR) << prefix_ << "Received unknown request.";
				continue;
			}
			VLOG(1) << prefix_ << "Received request \"" << request_.refresh().item_name() << "\"";
			DVLOG(1) << prefix_ << request_.DebugString();

			try {
/* forward to main application */
				ProcessRequest (*reinterpret_cast<rfa::sessionLayer::RequestToken*> ((uintptr_t)request_.refresh().token()),
						request_.refresh().service_id(),
						request_.refresh().model_type(),
						request_.refresh().item_name().c_str(),
						request_.refresh().rwf_major_version(),
						request_.refresh().rwf_minor_version());
			} catch (std::exception& e) {
				LOG(ERROR) << prefix_ << "ProcessRequest::Exception: { "
					"\"What\": \"" << e.what() << "\""
					" }";
			}
		}

		LOG(INFO) << prefix_ << "Worker closed.";
	}

	unsigned GetId() const { return id_; }

protected:
	bool GetRequest (provider::Request* request)
	{
		int rc;

		rc = zmq_msg_init (&msg_);
		CHECK(0 == rc);
		VLOG(1) << prefix_ << "Awaiting new job.";
		rc = zmq_recv (receiver_.get(), &msg_, 0);
		CHECK(0 == rc);
		if (!request->ParseFromArray (zmq_msg_data (&msg_), (int)zmq_msg_size (&msg_))) {
			LOG(ERROR) << prefix_ << "Received invalid request.";
			rc = zmq_msg_close (&msg_);
			CHECK(0 == rc);
			return false;
		}
		rc = zmq_msg_close (&msg_);
		CHECK(0 == rc);
		return true;
	}

	void ParseRIC (const char* ric, size_t ric_len, std::string* item_name, bin_decl_t* bin_decl, unsigned* day_offset)
	{
		url_.assign ("vta://localhost");
		url_.append (ric, ric_len);
		url_parse::ParseStandardURL (url_.c_str(), static_cast<int>(url_.size()), &parsed_);
		DCHECK(parsed_.path.is_valid());
		url_parse::ExtractFileName (url_.c_str(), parsed_.path, &file_name_);
		DCHECK(file_name_.is_valid());
		item_name->assign (url_.c_str() + file_name_.begin, file_name_.len);

		if (parsed_.query.is_valid()) {
			url_parse::Component query = parsed_.query;
			url_parse::Component key, value;
			while (url_parse::ExtractQueryKeyValue (url_.c_str(), &query, &key, &value)) {
				if (key.len == strlen (kOpen) && !strncmp (url_.c_str() + key.begin, kOpen, key.len))
				{
					std::stringstream ss (std::string (url_.c_str() + value.begin, value.len));
					boost::posix_time::time_duration td;
					if (ss >> td)
						bin_decl->bin_start = td;
				}
				else if (key.len == strlen (kClose) && !strncmp (url_.c_str() + key.begin, kClose, key.len))
				{
					std::stringstream ss (std::string (url_.c_str() + value.begin, value.len));
					boost::posix_time::time_duration td;
					if (ss >> td)
						bin_decl->bin_end = td;
				}
				else if (key.len == strlen (kTimezone) && !strncmp (url_.c_str() + key.begin, kTimezone, key.len))
				{
					const std::string value (url_.c_str() + value.begin, value.len);					
					boost::local_time::time_zone_ptr tz = tzdb_.time_zone_from_region (value);
					if (nullptr != tz)
						bin_decl->bin_tz = tz;
				}
				else if (key.len == strlen (kOffset) && !strncmp (url_.c_str() + key.begin, kOffset, key.len))
				{
					const std::string value (url_.c_str() + value.begin, value.len);
					unsigned offset = (unsigned)std::atol (value.c_str());
/* cap request */
					*day_offset = (offset < kMaximumDayOffset) ? offset : kMaximumDayOffset;
				}
				else if (key.len == strlen (kDays) && !strncmp (url_.c_str() + key.begin, kDays, key.len))
				{
					const std::string value (url_.c_str() + value.begin, value.len);
					unsigned count = (unsigned)std::atol (value.c_str());
/* cap request */
					bin_decl->bin_day_count = (count < kMaximumDayCount) ? count : kMaximumDayCount;
				}
			}
		}
	}

	void Send (
		const bin_t& bin,
		const RFA_String& stream_name,
		uint8_t rwf_major_version,
		uint8_t rwf_minor_version,
		rfa::sessionLayer::RequestToken& token
		)
	{
/* 7.4.8.1 Create a response message (4.2.2) */
		response_.clear();

/* 7.4.8.2 Create or re-use a request attribute object (4.2.4) */
		attribInfo_.clear();
		attribInfo_.setNameType (rfa::rdm::INSTRUMENT_NAME_RIC);
		attribInfo_.setServiceID (provider_->getServiceId());
		attribInfo_.setName (stream_name);
		response_.setAttribInfo (attribInfo_);

/* 7.4.8.3 Set the message model type of the response. */
		response_.setMsgModelType (rfa::rdm::MMT_MARKET_PRICE);
/* 7.4.8.4 Set response type, response type number, and indication mask. */
		response_.setRespType (rfa::message::RespMsg::RefreshEnum);
		response_.setIndicationMask ( rfa::message::RespMsg::DoNotCacheFlag
					    | rfa::message::RespMsg::DoNotFilterFlag
					    | rfa::message::RespMsg::RefreshCompleteFlag
					    | rfa::message::RespMsg::DoNotRippleFlag);

/* 4.3.1 RespMsg.Payload */
// not std::map :(  derived from rfa::common::Data
		fields_->setAssociatedMetaInfo (rwf_major_version, rwf_minor_version);
		fields_->setInfo (kDictionaryId, kFieldListId);

/* TIMEACT & ACTIV_DATE */
		struct tm _tm;
		__time32_t time32 = to_unix_epoch<__time32_t> (bin.GetCloseTime());
		_gmtime32_s (&_tm, &time32);

/* Clear required for SingleWriteIterator state machine. */
		auto& it = *single_write_it_.get();
		DCHECK (it.isInitialized());
		it.clear();
		it.start (*fields_.get());

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
		it.setReal (portware::mantissa (bin.GetTenDayPercentageChange()), rfa::data::ExponentNeg6);
/* PCTCHG_15D */
		field.setFieldID (config_.archive_fids.Rdm15DayPercentChangeId);
		it.bind (field);
		it.setReal (portware::mantissa (bin.GetFifteenDayPercentageChange()), rfa::data::ExponentNeg6);
/* PCTCHG_20D */
		field.setFieldID (config_.archive_fids.Rdm20DayPercentChangeId);
		it.bind (field);
		it.setReal (portware::mantissa (bin.GetTwentyDayPercentageChange()), rfa::data::ExponentNeg6);
/* PCTCHG_10T */
		field.setFieldID (config_.archive_fids.Rdm10TradingDayPercentChangeId);
		it.bind (field);
		it.setReal (portware::mantissa (bin.GetTenTradingDayPercentageChange()), rfa::data::ExponentNeg6);
/* PCTCHG_15T */
		field.setFieldID (config_.archive_fids.Rdm15TradingDayPercentChangeId);
		it.bind (field);
		it.setReal (portware::mantissa (bin.GetFifteenTradingDayPercentageChange()), rfa::data::ExponentNeg6);
/* PCTCHG_20T */
		field.setFieldID (config_.archive_fids.Rdm20TradingDayPercentChangeId);
		it.bind (field);
		it.setReal (portware::mantissa (bin.GetTwentyTradingDayPercentageChange()), rfa::data::ExponentNeg6);
/* VMA_20D */
		field.setFieldID (config_.archive_fids.RdmAverageVolumeId);
		it.bind (field);
		it.setReal (bin.GetAverageVolume(), rfa::data::Exponent0);
/* VMA_20TD */
		field.setFieldID (config_.archive_fids.RdmAverageNonZeroVolumeId);
		it.bind (field);
		it.setReal (bin.GetAverageNonZeroVolume(), rfa::data::Exponent0);
/* TRDCNT_20D */
		field.setFieldID (config_.archive_fids.RdmTotalMovesId);
		it.bind (field);
		it.setReal (bin.GetTotalMoves(), rfa::data::Exponent0);
/* HICNT_20D */
		field.setFieldID (config_.archive_fids.RdmMaximumMovesId);
		it.bind (field);
		it.setReal (bin.GetMaximumMoves(), rfa::data::Exponent0);
/* LOCNT_20D */
		field.setFieldID (config_.archive_fids.RdmMinimumMovesId);
		it.bind (field);
		it.setReal (bin.GetMinimumMoves(), rfa::data::Exponent0);
/* SMCNT_20D */
		field.setFieldID (config_.archive_fids.RdmSmallestMovesId);
		it.bind (field);
		it.setReal (bin.GetSmallestMoves(), rfa::data::Exponent0);
/* ACTIV_DATE */
		field.setFieldID (kRdmActiveDateId);
		it.bind (field);
		const uint16_t year  = /* rfa(yyyy) */ 1900 + _tm.tm_year /* tm(yyyy-1900 */;
		const uint8_t  month = /* rfa(1-12) */    1 + _tm.tm_mon  /* tm(0-11) */;
		const uint8_t  day   = /* rfa(1-31) */        _tm.tm_mday /* tm(1-31) */;
		it.setDate (year, month, day);

		it.complete();
		response_.setPayload (*fields_.get());

/** Optional: but require to replace stale values in cache when stale values are supported. **/
		status_.clear();
/* Item interaction state: Open, Closed, ClosedRecover, Redirected, NonStreaming, or Unspecified. */
		status_.setStreamState (rfa::common::RespStatus::NonStreamingEnum);
/* Data quality state: Ok, Suspect, or Unspecified. */
		status_.setDataState (rfa::common::RespStatus::OkEnum);
		response_.setRespStatus (status_);

#ifdef DEBUG
/* 4.2.8 Message Validation.  RFA provides an interface to verify that
 * constructed messages of these types conform to the Reuters Domain
 * Models as specified in RFA API 7 RDM Usage Guide.
 */
		uint8_t validation_status = rfa::message::MsgValidationError;
		try {
			RFA_String warningText;
			validation_status = response.validateMsg (&warningText);
			if (rfa::message::MsgValidationWarning == validation_status)
				LOG(ERROR) << prefix_ << "validateMsg: { \"warningText\": \"" << warningText << "\" }";
		} catch (rfa::common::InvalidUsageException& e) {
			LOG(ERROR) << prefix_ << "InvalidUsageException: { " <<
					   "\"StatusText\": \"" << e.getStatus().getStatusText() << "\""
					", " << response_ <<
				      " }";
		}
#endif
		provider_->send (response_, token);
		VLOG(3) << prefix_ << "Response sent.";
	}

	void ProcessRequest (
		rfa::sessionLayer::RequestToken& request_token,
		uint32_t service_id,
		uint8_t model_type,
		const char* name_c,
		uint8_t rwf_major_version,
		uint8_t rwf_minor_version
		)
	{
		VLOG(2) << prefix_ << "Bin request: { "
			  "\"RequestToken\": \"" << (intptr_t)&request_token << "\""
			", \"ServiceID\": " << service_id <<
			", \"MsgModelType\": " << (int)model_type <<
			", \"Name\": \"" << name_c << "\""
			", \"RwfMajorVersion\": " << (int)rwf_major_version <<
			", \"RwfMinorVersion\": " << (int)rwf_minor_version <<
			" }";

		DCHECK(service_id == provider_->getServiceId());
		DCHECK(model_type == rfa::rdm::MMT_MARKET_PRICE);

/* derived symbol name */
		const RFA_String stream_name (name_c, 0, false);

/* decompose request */
		std::string item_name;
		bin_decl_t bin_decl;
		unsigned day_offset = 0;

		bin_decl.bin_tz = TZ_;
		bin_decl.bin_day_count = std::stoi (config_.day_count);
		ParseRIC (name_c, strlen (name_c), &item_name, &bin_decl, &day_offset);

/* start of bin */
		using namespace boost::local_time;
		const auto now_in_tz = local_sec_clock::local_time (bin_decl.bin_tz);
		const auto today_in_tz = now_in_tz.local_time().date();
		auto start_date (today_in_tz);
		if (day_offset > 0) {
			while (!is_business_day (start_date))
				start_date -= boost::gregorian::date_duration (1);
			vhayu::business_day_iterator bd_itr (start_date);
			for (unsigned offset = day_offset; offset > 0; --offset)
				--bd_itr;
			start_date = *bd_itr;
		}

/* TREP-VA cached symbol handle */
		auto directory_it = directory_.find (item_name);
		DCHECK(directory_.end() != directory_it);

/* run analytic on bin and send result */
		try {
			bin_t bin (bin_decl, directory_it->second->handle, kDefaultLastPriceField, kDefaultTickVolumeField);
			VLOG(2) << prefix_ << "Processing bin: " << bin_decl;
			bin.Calculate (start_date, work_area_.get(), view_element_.get());
			Send (bin, stream_name, rwf_major_version, rwf_minor_version, request_token);
		} catch (rfa::common::InvalidUsageException& e) {
			LOG(ERROR) << prefix_ << "InvalidUsageException: { " <<
					"\"StatusText\": \"" << e.getStatus().getStatusText() << "\""
					" }";
		} catch (std::exception& e) {
			LOG(ERROR) << prefix_ << "Calculate::Exception: { "
					"\"What\": \"" << e.what() << "\""
					" }";
		}
		VLOG(2) << prefix_ << "Request complete.";
	}

/* worker unique identifier */
	const unsigned id_;
	std::string prefix_;

/* 0mq context */
	std::shared_ptr<void> zmq_context_;

/* Socket to receive refresh requests on. */
	std::shared_ptr<void> receiver_;

/* Incoming 0mq message */
	zmq_msg_t msg_;
	provider::Request request_;

/* RIC decomposition */
	url_parse::Parsed parsed_;
	url_parse::Component file_name_;
	std::string url_;

/* FLexRecord cursor */
	FlexRecDefinitionManager* manager_;
	std::shared_ptr<FlexRecWorkAreaElement> work_area_;
	std::shared_ptr<FlexRecViewElement> view_element_;

/* Outgoing rfa message */
	rfa::message::RespMsg response_;

/* Publish fields. */
	std::shared_ptr<rfa::data::FieldList> fields_;	/* no copy ctor */
	rfa::message::AttribInfo attribInfo_;
	rfa::common::RespStatus status_;

/* Iterator for populating publish fields */
	std::shared_ptr<rfa::data::SingleWriteIterator> single_write_it_;	/* no copy ctor */

/* application reference */
	std::shared_ptr<provider_t> provider_;
	const boost::unordered_map<std::string, std::shared_ptr<archive_stream_t>>& directory_;
	const boost::local_time::tz_database& tzdb_;
	const boost::local_time::time_zone_ptr TZ_;
	const config_t& config_;
};

gomi::gomi_t::gomi_t()
	:
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
gomi::gomi_t::is_special_bin (const gomi::bin_decl_t& bin)
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
	if (!init()) {
		clear();
		is_shutdown_ = true;
		throw vpf::UserPluginException ("Initialization failed, aborting.");
	}
}

bool
gomi::gomi_t::init()
{
	std::vector<std::string> symbolmap;

	LOG(INFO) << config_;

	try {
/* ZeroMQ context. */
		std::function<int(void*)> zmq_term_deleter = zmq_term;
		zmq_context_.reset (zmq_init (0), zmq_term_deleter);
		CHECK((bool)zmq_context_);
	} catch (std::exception& e) {
		LOG(ERROR) << "ZeroMQ::Exception: { "
			"\"What\": \"" << e.what() << "\" }";
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
		if (!(bool)rfa_ || !rfa_->init())
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
		provider_.reset (new provider_t (config_, rfa_, event_queue_, zmq_context_));
		if (!(bool)provider_ || !provider_->init())
			return false;

/* Create state for published instruments: For every instrument, e.g. MSFT.O
 */
		std::for_each (symbolmap.begin(), symbolmap.end(), [&](const std::string& symbol) {
			auto stream = std::make_shared<archive_stream_t> (symbol);
			CHECK ((bool)stream);
			bool status = provider_->createItemStream (symbol.c_str(), stream);
			CHECK (status);
			directory_.emplace (std::make_pair (symbol, std::move (stream)));
		});
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
		std::function<int(void*)> zmq_close_deleter = zmq_close;
/* Worker abort socket. */
		abort_sock_.reset (zmq_socket (zmq_context_.get(), ZMQ_PUSH), zmq_close_deleter);
		CHECK((bool)abort_sock_);
		int rc = zmq_bind (abort_sock_.get(), "inproc://gomi/abort");
		CHECK(0 == rc);
/* Worker threads. */
		for (size_t i = 0; i < config_.worker_count; ++i) {
			const unsigned worker_id = (unsigned)(1 + i);
			LOG(INFO) << "Spawning worker #" << worker_id;
			auto worker = std::make_shared<worker_t> (provider_, directory_, tzdb_, TZ_, config_, zmq_context_, worker_id);
			if (!(bool)worker)
				return false;
			auto thread = std::make_shared<boost::thread> ([worker](){ if (worker->Init()) worker->Run(); });
			if (!(bool)thread)
				return false;
			workers_.emplace_front (std::make_pair (worker, thread));
		}
	} catch (std::exception& e) {
		LOG(ERROR) << "ZeroMQ::Exception: { "
			"\"What\": \"" << e.what() << "\" }";
		return false;
	}

	try {
/* No main loop inside this thread, must spawn new thread for message pump. */
		event_pump_.reset (new event_pump_t (event_queue_));
		if (!(bool)event_pump_)
			return false;

		event_thread_.reset (new boost::thread (*event_pump_.get()));
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
		if (!register_tcl_api (getId()))
			return false;
	} catch (std::exception& e) {
		LOG(ERROR) << "TclApi::Exception: { "
			"\"What\": \"" << e.what() << "\" }";
		return false;
	}

	LOG(INFO) << "Init complete, awaiting queries.";
	return true;
}

void
gomi::gomi_t::clear()
{
/* Interrupt worker threads. */
	if (!workers_.empty()) {
		LOG(INFO) << "Reviewing worker threads.";
		provider::Request request;
		request.set_msg_type (provider::Request::MSG_ABORT);
		zmq_msg_t msg;
		unsigned active_threads = 0;
		for (auto it = workers_.begin(); it != workers_.end(); ++it) {
			if ((bool)it->second && it->second->joinable()) {
				zmq_msg_init_size (&msg, request.ByteSize());
				request.SerializeToArray (zmq_msg_data (&msg), (int)zmq_msg_size (&msg));
				zmq_send (abort_sock_.get(), &msg, 0);
				zmq_msg_close (&msg);
				++active_threads;
			}
		}
		if (active_threads > 0) {
			LOG(INFO) << "Sending interrupt to " << active_threads << " worker threads.";
			for (auto it = workers_.begin(); it != workers_.end(); ++it) {
				if ((bool)it->second && it->second->joinable())
					it->second->join();
				LOG(INFO) << "Thread #" << it->first->GetId() << "joined.";
				it->first.reset();
			}
		}
		LOG(INFO) << "All worker threads joined.";
	}

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
	directory_.clear();
/* Release 0mq sockets before context */
	assert (provider_.use_count() <= 1);
	provider_.reset();
	abort_sock_.reset();
	zmq_context_.reset();
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
	unregister_tcl_api (getId());
	clear();
	LOG(INFO) << "Runtime summary: {"
		    " \"tclQueryReceived\": " << cumulative_stats_[GOMI_PC_TCL_QUERY_RECEIVED] <<
		" }";
	LOG(INFO) << "Instance closed.";
	vpf::AbstractUserPlugin::destroy();
}

/* eof */