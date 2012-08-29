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

#include "chromium/chromium_switches.hh"
#include "chromium/command_line.hh"
#include "chromium/file_util.hh"
#include "chromium/logging.hh"
#include "chromium/metrics/histogram.hh"
#include "chromium/metrics/stats_table.hh"
#include "chromium/string_piece.hh"
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

/* RDM: NASD_BIDASK record template as defacto default */
static const int kFieldListId = 3;

/* RDM FIDs. */
static const int kRdmTimeOfUpdateId		= 5;
static const int kRdmActiveDateId		= 17;

/* FlexRecord Quote identifier. */
static const uint32_t kQuoteId = 40002;

/* Default FlexRecord fields. */
static const char* kDefaultLastPriceField  = "LastPrice";
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

static const char* kStatsFileName = "gomi.stats";
static int kStatsFileThreads = 20;
static int kStatsFileCounters = 200;

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

/* Worker thread for processing refresh requests.
 */
class gomi::worker_t
{
public:
	worker_t (
		const boost::local_time::tz_database& tzdb,
		const boost::local_time::time_zone_ptr TZ,
		const config_t& config,
		std::shared_ptr<void>& zmq_context,
		unsigned id
	) :
		id_ (id),
		zmq_context_ (zmq_context),
		manager_ (nullptr),
		respmsg_ (false),	/* reference */
		fields_ (false),
		attribInfo_ (false),
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
		CHECK (request_sock_.use_count() <= 1);
		request_sock_.reset();
		CHECK (response_sock_.use_count() <= 1);
		response_sock_.reset();
		zmq_context_.reset();
	}

	bool Init()
	{
		std::function<int(void*)> zmq_close_deleter = zmq_close;
		int rc;

		try {
/* Setup 0mq sockets */
			request_sock_.reset (zmq_socket (zmq_context_.get(), ZMQ_PULL), zmq_close_deleter);
			CHECK((bool)request_sock_);
			rc = zmq_connect (request_sock_.get(), "inproc://gomi/rfa/request");
			CHECK(0 == rc);
/* Also bind for terminating interrupt. */
			rc = zmq_connect (request_sock_.get(), "inproc://gomi/worker/abort");
			CHECK(0 == rc);
/* Response image socket */
			response_sock_.reset (zmq_socket (zmq_context_.get(), ZMQ_PUSH), zmq_close_deleter);
			CHECK((bool)response_sock_);
			rc = zmq_connect (response_sock_.get(), "inproc://gomi/rfa/response");
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
			fields_->setInfo (kDictionaryId, kFieldListId);

			CHECK (config_.maximum_data_size > 0);
			single_write_it_ = std::make_shared<rfa::data::SingleWriteIterator> ();
			CHECK ((bool)single_write_it_);
			single_write_it_->initialize (*fields_.get(), static_cast<int> (config_.maximum_data_size));
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
			
			switch (request_.msg_type()) {
			case provider::Request::MSG_SNAPSHOT:
				break;
			case provider::Request::MSG_SUBSCRIPTION:
			case provider::Request::MSG_REFRESH:
				LOG(ERROR) << prefix_ << "Received unsupported request.";
				continue;
			case provider::Request::MSG_ABORT:
				LOG(INFO) << prefix_ << "Received interrupt request.";
				goto close_worker;
			default:
				LOG(ERROR) << prefix_ << "Received unknown request.";
				continue;
			}
			VLOG(1) << prefix_ << "Received request \"" << request_.refresh().item_name() << "\"";
			DVLOG(1) << prefix_ << request_.DebugString();

			try {
/* forward to main application */
				OnRequest (reinterpret_cast<rfa::sessionLayer::RequestToken*> (static_cast<uintptr_t> (request_.refresh().token())),
					   request_.refresh().service_id(),
					   request_.refresh().model_type(),
					   request_.refresh().item_name().c_str(),
					   request_.msg_type(),
					   request_.refresh().rwf_major_version(),
					   request_.refresh().rwf_minor_version());
			} catch (std::exception& e) {
				LOG(ERROR) << prefix_ << "OnRequest::Exception: { "
					"\"What\": \"" << e.what() << "\""
					" }";
			}
		}

close_worker:
		LOG(INFO) << prefix_ << "Worker closed.";
	}

	unsigned GetId() const { return id_; }

protected:
	bool GetRequest (provider::Request*const request)
	{
		int rc;

		rc = zmq_msg_init (&msg_);
		CHECK(0 == rc);
		DVLOG(1) << prefix_ << "Awaiting new job.";
		rc = zmq_recv (request_sock_.get(), &msg_, 0);
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

	void OnRequest (
		rfa::sessionLayer::RequestToken*const request_token,
		uint32_t service_id,
		uint8_t model_type,
		const char* item_name,
		provider::Request_MsgType msg_type,
		uint8_t rwf_major_version,
		uint8_t rwf_minor_version
		)
	{
		using namespace boost::chrono;
		auto checkpoint = high_resolution_clock::now();
		OnBinRequest (request_token, service_id, model_type, item_name, rwf_major_version, rwf_minor_version);
		HISTOGRAM_TIMES("Worker.OnRequest", high_resolution_clock::now() - checkpoint);
	}

	void OnBinRequest (
		rfa::sessionLayer::RequestToken*const token,
		uint32_t service_id,
		uint8_t model_type,
		const char* stream_name_c,
		uint8_t rwf_major_version,
		uint8_t rwf_minor_version
		)
	{
		VLOG(2) << prefix_ << "Bin request: { "
			  "\"RequestToken\": \"" << (uintptr_t)&token << "\""
			", \"ServiceID\": " << service_id <<
			", \"MsgModelType\": " << (int)model_type <<
			", \"Name\": \"" << stream_name_c << "\""
			", \"RwfMajorVersion\": " << (int)rwf_major_version <<
			", \"RwfMinorVersion\": " << (int)rwf_minor_version <<
			" }";

/* derived symbol name */
		const RFA_String stream_name (stream_name_c, 0, false);

/* decompose request */
		bin_decl_t bin_decl;
		unsigned day_offset = 0;

		bin_decl.bin_tz = TZ_;
		bin_decl.bin_day_count = config_.day_count;
		ParseRIC (stream_name_c, strlen (stream_name_c), &item_name_, &bin_decl, &day_offset);

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

/* run analytic on bin and send result */
		try {
			bin_t bin (bin_decl, item_name_.c_str(), kDefaultLastPriceField, kDefaultTickVolumeField);
			VLOG(2) << prefix_ << "Processing bin: " << bin_decl;
			bin.Calculate (start_date, work_area_.get(), view_element_.get());
			SendSnapshot (bin, service_id, stream_name, rwf_major_version, rwf_minor_version, token);
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

/* Decompose RIC of the form:
 *
 * /VTA/MSFT.O?open=10:00:00&close=10:10:00&days=20&offset=1&tz=EST
 *
 * item_name              = MSFT.O     // underlying symbol
 * bin_decl.bin_start     = 10:00:00   // bar open time
 * bin_decl.bin_end       = 10:10:00   // bar close time
 * bin_decl.bin_day_count = 20         // length of bin in days
 * bin_decl.bin_tz        = EST        // time zone of bar times
 * day_offset             = 1          // business day offset to today
 */
	void ParseRIC (
		const char* ric,
		size_t ric_len,
		std::string* item_name,
		bin_decl_t* bin_decl,
		unsigned* day_offset
		)
	{
/* std::string for convenience of appending, store fixed on heap inside object,
 * The URL prefix is required to capture standard behaviour of the URL parsing library.
 */
		url_.assign ("vta://localhost");
		url_.append (ric, ric_len);
/* Pass through Google URL http://code.google.com/p/google-url/
 */
		url_parse::ParseStandardURL (url_.c_str(), static_cast<int>(url_.size()), &parsed_);
		DCHECK(parsed_.path.is_valid());
/* URL file name becomes the underlying symbol
 */
		url_parse::ExtractFileName (url_.c_str(), parsed_.path, &file_name_);
		DCHECK(file_name_.is_valid());
		item_name->assign (url_.c_str() + file_name_.begin, file_name_.len);

/* If a query parameter was found, i.e. ?x suffix
 */
		if (parsed_.query.is_valid())
		{
			url_parse::Component query = parsed_.query;
			url_parse::Component key_range, value_range;			
			boost::posix_time::time_duration td;
/* For each key-value pair, i.e. ?a=x&b=y&c=z -> (a,x) (b,y) (c,z)
 */
			while (url_parse::ExtractQueryKeyValue (url_.c_str(), &query, &key_range, &value_range))
			{
/* Lazy std::string conversion for key
 */
				const chromium::StringPiece key (url_.c_str() + key_range.begin, key_range.len);
/* Value must convert to add NULL terminator for conversion APIs.
 */
				value_.assign (url_.c_str() + value_range.begin, value_range.len);
				if (key == kOpen) {
/* Disabling exceptions in boost::posix_time::time_duration requires stringstream which requires a string to initialise.
 */
					iss_.str (value_);
					if (iss_ >> td) bin_decl->bin_start = td;
				} else if (key == kClose) {
					iss_.str (value_);
					if (iss_ >> td) bin_decl->bin_end = td;
				} else if (key == kOffset) {
/* Numeric value parsing requires a NULL terminated char array.
 */
					const unsigned offset = (unsigned)std::atol (value_.c_str());
					*day_offset = (offset < kMaximumDayOffset) ? offset : kMaximumDayOffset;
				} else if (key == kDays) {
					const unsigned count = (unsigned)std::atol (value_.c_str());
					bin_decl->bin_day_count = (count < kMaximumDayCount) ? count : kMaximumDayCount;
				} else if (key == kTimezone ) {
/* Time zone lookup in an C++11 unordered map using a std::string index.
 */
					const boost::local_time::time_zone_ptr tzptr = tzdb_.time_zone_from_region (value_);
					if (nullptr != tzptr) bin_decl->bin_tz = tzptr;
				}
 			}
		}
	}

	void SendSnapshot (
		const bin_t& bin,
		uint32_t service_id,
		const RFA_String& stream_name,
		uint8_t rwf_major_version,
		uint8_t rwf_minor_version,
		rfa::sessionLayer::RequestToken*const token
		)
	{
/* 7.4.8.1 Create a response message (4.2.2) */
		respmsg_.clear();

/* 7.4.8.2 Create or re-use a request attribute object (4.2.4) */
		attribInfo_.clear();
		attribInfo_.setNameType (rfa::rdm::INSTRUMENT_NAME_RIC);
		attribInfo_.setServiceID (service_id);
		attribInfo_.setName (stream_name);
		respmsg_.setAttribInfo (attribInfo_);

/* 7.4.8.3 Set the message model type of the response. */
		respmsg_.setMsgModelType (rfa::rdm::MMT_MARKET_PRICE);
/* 7.4.8.4 Set response type, response type number, and indication mask. */
		respmsg_.setRespType (rfa::message::RespMsg::RefreshEnum);

/* for snapshot images do not cache */
		respmsg_.setIndicationMask (rfa::message::RespMsg::DoNotFilterFlag     |
					    rfa::message::RespMsg::RefreshCompleteFlag |
					    rfa::message::RespMsg::DoNotRippleFlag     |
					    rfa::message::RespMsg::DoNotCacheFlag);
/* 4.3.1 RespMsg.Payload */
// not std::map :(  derived from rfa::common::Data
		fields_->setAssociatedMetaInfo (rwf_major_version, rwf_minor_version);

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
		respmsg_.setPayload (*fields_.get());

/** Optional: but require to replace stale values in cache when stale values are supported. **/
		status_.clear();
/* Item interaction state: Open, Closed, ClosedRecover, Redirected, NonStreaming, or Unspecified. */
		status_.setStreamState (rfa::common::RespStatus::NonStreamingEnum);
/* Data quality state: Ok, Suspect, or Unspecified. */
		status_.setDataState (rfa::common::RespStatus::OkEnum);
/* Error code, e.g. NotFound, InvalidArgument, ... */
		status_.setStatusCode (rfa::common::RespStatus::NoneEnum);
		respmsg_.setRespStatus (status_);

#ifdef DEBUG
/* 4.2.8 Message Validation.  RFA provides an interface to verify that
 * constructed messages of these types conform to the Reuters Domain
 * Models as specified in RFA API 7 RDM Usage Guide.
 */
		uint8_t validation_status = rfa::message::MsgValidationError;
		try {
			RFA_String warningText;
			validation_status = respmsg_.validateMsg (&warningText);
			if (rfa::message::MsgValidationWarning == validation_status)
				LOG(ERROR) << prefix_ << "validateMsg: { \"warningText\": \"" << warningText << "\" }";
		} catch (rfa::common::InvalidUsageException& e) {
			LOG(ERROR) << prefix_ << "InvalidUsageException: { " <<
					   "\"StatusText\": \"" << e.getStatus().getStatusText() << "\""
					", " << respmsg_ <<
				      " }";
		}
#endif
/* Pack RFA message into buffer and embed within Protobuf and enqueue to RFA publisher */
		const rfa::common::Buffer& buffer = respmsg_.getEncodedBuffer();
		response_.set_msg_type (provider::Response::MSG_SNAPSHOT);
		response_.set_token (reinterpret_cast<uintptr_t> (token));
		response_.set_encoded_buffer (buffer.c_buf(), buffer.size());
		int rc = zmq_msg_init_size (&msg_, response_.ByteSize());
		CHECK(0 == rc);
		response_.SerializeToArray (zmq_msg_data (&msg_), static_cast<int> (zmq_msg_size (&msg_)));
		rc = zmq_send (response_sock_.get(), &msg_, 0);
		CHECK(0 == rc);
		rc = zmq_msg_close (&msg_);
		CHECK(0 == rc);
	}

/* worker unique identifier */
	const unsigned id_;
	std::string prefix_;

/* 0mq context */
	std::shared_ptr<void> zmq_context_;

/* Socket to receive requests on. */
	std::shared_ptr<void> request_sock_;

/* response socket for sending images back. */
	std::shared_ptr<void> response_sock_;

/* Incoming 0mq message */
	zmq_msg_t msg_;
	provider::Request request_;

/* RIC decomposition */
	url_parse::Parsed parsed_;
	url_parse::Component file_name_;
	std::string url_, value_, item_name_;
	std::istringstream iss_;

/* FLexRecord cursor */
	FlexRecDefinitionManager* manager_;
	std::shared_ptr<FlexRecWorkAreaElement> work_area_;
	std::shared_ptr<FlexRecViewElement> view_element_;

/* Outgoing rfa message */
	rfa::message::RespMsg respmsg_;
	provider::Response response_;

/* Publish fields. */
	std::shared_ptr<rfa::data::FieldList> fields_;	/* no copy ctor */
	rfa::message::AttribInfo attribInfo_;
	rfa::common::RespStatus status_;

/* Iterator for populating publish fields */
	std::shared_ptr<rfa::data::SingleWriteIterator> single_write_it_;	/* no copy ctor */

/* application reference */
	const boost::local_time::tz_database& tzdb_;
	const boost::local_time::time_zone_ptr TZ_;
	const config_t& config_;
};

gomi::gomi_t::gomi_t()
	:
	is_shutdown_ (false),
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

	if (0 == instance_) {
/* Histogram singleton */
		recorder_.reset (new chromium::StatisticsRecorder());

		const CommandLine& command_line = *CommandLine::ForCurrentProcess();
/* Histogram debug log dump */
		if (command_line.HasSwitch (switches::kDumpHistogramsOnExit))
			chromium::StatisticsRecorder::set_dump_on_exit (true);
	}

#ifdef USE_STATS_TABLE
// Load and initialize the stats table.  No unique name - instances share table.
	statstable_ = new chromium::StatsTable(kStatsFileName, kStatsFileThreads, kStatsFileCounters);
	chromium::StatsTable::set_current (statstable_);
#endif
}

gomi::gomi_t::~gomi_t()
{
/* Remove from list before clearing. */
	boost::unique_lock<boost::shared_mutex> (global_list_lock_);
	global_list_.remove (this);

	Clear();

#ifdef USE_STATS_TABLE
// Tear down the shared StatsTable.
	chromium::StatsTable::set_current (nullptr);
	delete statstable_;
	statstable_ = nullptr;
#endif
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

	if (!config_.ParseDomElement (vpf_config.getXmlConfigData())) {
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
	std::function<int(void*)> zmq_term_deleter = zmq_term;
	std::function<int(void*)> zmq_close_deleter = zmq_close;

	LOG(INFO) << config_;

	try {
/* FlexRecord cursor for Tcl thread processing. */
		manager_ = FlexRecDefinitionManager::GetInstance (nullptr);
		work_area_.reset (manager_->AcquireWorkArea(), [this](FlexRecWorkAreaElement* work_area){ manager_->ReleaseWorkArea (work_area); });
		view_element_.reset (manager_->AcquireView(), [this](FlexRecViewElement* view_element){ manager_->ReleaseView (view_element); });

		if (!manager_->GetView ("Trade", view_element_->view)) {
			LOG(ERROR) << "FlexRecDefinitionManager::GetView failed";
		}
	} catch (std::exception& e) {
		LOG(ERROR) << "FlexRecord::Exception: { "
			"\"What\": \"" << e.what() << "\""
			" }";
		return false;
	}

	try {
/* ZeroMQ context. */
		zmq_context_.reset (zmq_init (0), zmq_term_deleter);
		CHECK((bool)zmq_context_);

/* push for event loop interrupt */
		event_pump_abort_sock_.reset (zmq_socket (zmq_context_.get(), ZMQ_PUSH), zmq_close_deleter);
		CHECK((bool)event_pump_abort_sock_);
		int rc = zmq_bind (event_pump_abort_sock_.get(), "inproc://gomi/event/abort");
		CHECK(0 == rc);

/* pull for RFA responses */
		response_sock_.reset (zmq_socket (zmq_context_.get(), ZMQ_PULL), zmq_close_deleter);
		CHECK((bool)response_sock_);
		rc = zmq_bind (response_sock_.get(), "inproc://gomi/rfa/response");
		CHECK(0 == rc);
	} catch (const std::exception& e) {
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
	} catch (const boost::local_time::data_not_accessible& e) {
		LOG(ERROR) << "Time zone specifications cannot be loaded: " << e.what();
		return false;
	} catch (const boost::local_time::bad_field_count& e) {
		LOG(ERROR) << "Time zone specifications malformed: " << e.what();
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
		provider_.reset (new provider_t (config_, rfa_, event_queue_, zmq_context_));
		if (!(bool)provider_ || !provider_->Init())
			return false;
	} catch (const rfa::common::InvalidUsageException& e) {
		LOG(ERROR) << "InvalidUsageException: { "
			  "\"Severity\": \"" << severity_string (e.getSeverity()) << "\""
			", \"Classification\": \"" << classification_string (e.getClassification()) << "\""
			", \"StatusText\": \"" << e.getStatus().getStatusText() << "\""
			" }";
		return false;
	} catch (const rfa::common::InvalidConfigurationException& e) {
		LOG(ERROR) << "InvalidConfigurationException: { "
			  "\"Severity\": \"" << severity_string (e.getSeverity()) << "\""
			", \"Classification\": \"" << classification_string (e.getClassification()) << "\""
			", \"StatusText\": \"" << e.getStatus().getStatusText() << "\""
			", \"ParameterName\": \"" << e.getParameterName() << "\""
			", \"ParameterValue\": \"" << e.getParameterValue() << "\""
			" }";
		return false;
	} catch (const std::exception& e) {
		LOG(ERROR) << "Rfa::Exception: { "
			"\"What\": \"" << e.what() << "\" }";
		return false;
	}

	try {
		std::function<int(void*)> zmq_close_deleter = zmq_close;
/* Worker abort socket. */
		worker_abort_sock_.reset (zmq_socket (zmq_context_.get(), ZMQ_PUSH), zmq_close_deleter);
		CHECK((bool)worker_abort_sock_);
		int rc = zmq_bind (worker_abort_sock_.get(), "inproc://gomi/worker/abort");
		CHECK(0 == rc);
/* Worker threads. */
		for (size_t i = 0; i < config_.worker_count; ++i) {
			const unsigned worker_id = (unsigned)(1 + i);
			LOG(INFO) << "Spawning worker #" << worker_id;
			auto worker = std::make_shared<worker_t> (tzdb_, TZ_, config_, zmq_context_, worker_id);
			if (!(bool)worker)
				return false;
			auto thread = std::make_shared<boost::thread> ([worker](){ if (worker->Init()) worker->Run(); });
			if (!(bool)thread)
				return false;
			workers_.emplace_front (std::make_pair (worker, thread));
		}
	} catch (const std::exception& e) {
		LOG(ERROR) << "ZeroMQ::Exception: { "
			"\"What\": \"" << e.what() << "\" }";
		return false;
	}

	try {
/* No main loop inside this thread, must spawn new thread for message pump. */
		event_pump_.reset (new event_pump_t (zmq_context_, response_sock_, provider_, event_queue_));
		if (!(bool)event_pump_)
			return false;

		event_thread_.reset (new boost::thread ([this]() { if (event_pump_->Init()) event_pump_->Run(); }));
		if (!(bool)event_thread_)
			return false;
	} catch (const std::exception& e) {
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
	} catch (const std::exception& e) {
		LOG(ERROR) << "SnmpAgent::Exception: { "
			"\"What\": \"" << e.what() << "\" }";
		return false;
	}

	try {
/* Register Tcl commands with TREP-VA search engine and await callbacks. */
		if (!RegisterTclApi (getId()))
			return false;
	} catch (const std::exception& e) {
		LOG(ERROR) << "TclApi::Exception: { "
			"\"What\": \"" << e.what() << "\" }";
		return false;
	}

	LOG(INFO) << "Init complete, awaiting queries.";
	return true;
}

void
gomi::gomi_t::Clear()
{
/* Stop generating new messages */
	if ((bool)event_queue_)
		event_queue_->deactivate();

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
				request.SerializeToArray (zmq_msg_data (&msg), static_cast<int> (zmq_msg_size (&msg)));
				zmq_send (worker_abort_sock_.get(), &msg, 0);
				zmq_msg_close (&msg);
				++active_threads;
			}
		}
		if (active_threads > 0) {
			LOG(INFO) << "Sending interrupt to " << active_threads << " worker threads.";
			for (auto it = workers_.begin(); it != workers_.end(); ++it) {
				if ((bool)it->second && it->second->joinable())
					it->second->join();
				LOG(INFO) << "Thread #" << it->first->GetId() << " joined.";
				it->first.reset();
			}
		}
		LOG(INFO) << "All worker threads joined.";
	}
	CHECK (worker_abort_sock_.use_count() <= 1);
	worker_abort_sock_.reset();

/* Close SNMP agent. */
	snmp_agent_.reset();

/* Drain and close event queue. */
	if ((bool)event_thread_ && event_thread_->joinable()) {
		provider::Request request;
		zmq_msg_t msg;
		LOG(INFO) << "Sending interrupt to event pump thread.";
		request.set_msg_type (provider::Request::MSG_ABORT);
		zmq_msg_init_size (&msg, request.ByteSize());
		request.SerializeToArray (zmq_msg_data (&msg), static_cast<int> (zmq_msg_size (&msg)));
		zmq_send (event_pump_abort_sock_.get(), &msg, 0);
		zmq_msg_close (&msg);	
		event_thread_->join();
		LOG(INFO) << "Event pump thread joined.";
		if ((bool)event_pump_)
			event_pump_->Clear();
	}
	CHECK (event_pump_abort_sock_.use_count() <= 1);
	event_pump_abort_sock_.reset();

/* Release everything with an 0mq & RFA dependency. */
	event_thread_.reset();
	event_pump_.reset();
/* Release 0mq sockets before context */
	if ((bool)provider_)
		provider_->Clear();
	CHECK (provider_.use_count() <= 1);
	provider_.reset();
	CHECK (response_sock_.use_count() <= 1);
	response_sock_.reset();
/* Bring down 0mq context as all sockets are closed. */
	CHECK (zmq_context_.use_count() <= 1);
	zmq_context_.reset();
/* Release everything remaining with an RFA dependency. */
	CHECK (log_.use_count() <= 1);
	log_.reset();
	CHECK (event_queue_.use_count() <= 1);
	event_queue_.reset();
	CHECK (rfa_.use_count() <= 1);
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
		" }";
	LOG(INFO) << "Instance closed.";
	vpf::AbstractUserPlugin::destroy();
}

class rfa_dispatcher_t : public rfa::common::DispatchableNotificationClient
{
public:
	rfa_dispatcher_t (std::shared_ptr<void> zmq_context) : zmq_context_ (zmq_context) {}
	~rfa_dispatcher_t()
	{
		Clear();
		LOG(INFO) << "RFA event dispatcher closed.";
	}

	int Init (void)
	{
		std::function<int(void*)> zmq_close_deleter = zmq_close;
		CHECK((bool)zmq_context_);
		event_sock_.reset (zmq_socket (zmq_context_.get(), ZMQ_PUSH), zmq_close_deleter);
		CHECK((bool)event_sock_);
		return zmq_connect (event_sock_.get(), "inproc://gomi/rfa/event");
	}

	void Clear (void)
	{
		CHECK (event_sock_.use_count() <= 1);
		event_sock_.reset();
		zmq_context_.reset();
	}

	void notify (rfa::common::Dispatchable& eventSource, void* closure) override
	{
		DCHECK((bool)event_sock_);
		int rc = zmq_msg_init_size (&msg_, 0);
		CHECK(0 == rc);
		rc = zmq_send (event_sock_.get(), &msg_, 0);
		CHECK(0 == rc);
		rc = zmq_msg_close (&msg_);
		CHECK(0 == rc);
	}

protected:
	std::shared_ptr<void> zmq_context_;
	std::shared_ptr<void> event_sock_;
	zmq_msg_t msg_;
};

bool
gomi::event_pump_t::Init (void)
{
	std::function<int(void*)> zmq_close_deleter = zmq_close;

/* pull RFA events */
	event_sock_.reset (zmq_socket (zmq_context_.get(), ZMQ_PULL), zmq_close_deleter);
	CHECK((bool)event_sock_);
	int rc = zmq_bind (event_sock_.get(), "inproc://gomi/rfa/event");
	CHECK(0 == rc);
	poll_items_[0].socket = event_sock_.get();
	poll_items_[0].events = ZMQ_POLLIN;

/* pull RFA responses */
	poll_items_[1].socket = response_sock_.get();
	poll_items_[1].events = ZMQ_POLLIN;

/* pull abort event */
	abort_sock_.reset (zmq_socket (zmq_context_.get(), ZMQ_PULL), zmq_close_deleter);
	CHECK((bool)abort_sock_);
	rc = zmq_connect (abort_sock_.get(), "inproc://gomi/event/abort");
	CHECK(0 == rc);
	poll_items_[2].socket = abort_sock_.get();
	poll_items_[2].events = ZMQ_POLLIN;

	return true;
}

void
gomi::event_pump_t::Clear (void)
{
/* cleanup 0mq sockets before context. */
	CHECK (abort_sock_.use_count() <= 1);
	abort_sock_.reset();
	CHECK (event_sock_.use_count() <= 1);
	event_sock_.reset();
	response_sock_.reset();
	zmq_context_.reset();
/* cleanup RFA dependencies */
	provider_.reset();
	event_queue_.reset();
}

void
gomi::event_pump_t::Run (void)
{
	rfa_dispatcher_t dispatcher (zmq_context_);
	zmq_msg_t msg;
	rfa::message::RespMsg respmsg (false);
	rfa::sessionLayer::RequestToken* token;
	provider::Response response;

/* proxy RFA events through 0mq */
	int rc = dispatcher.Init();
	CHECK(0 == rc);
	event_queue_->registerNotificationClient (dispatcher, nullptr);

	LOG(INFO) << "Entering event pump loop.";

	do {
		rc = zmq_poll (poll_items_, _countof (poll_items_), -1);
		if (rc <= 0)
			continue;
/* #0 - RFA event */
		if (0 != (poll_items_[0].revents & ZMQ_POLLIN))
			event_queue_->dispatch (rfa::common::Dispatchable::NoWait);
/* #1 - RFA response message */
		if (0 != (poll_items_[1].revents & ZMQ_POLLIN))
		{
			rc = zmq_msg_init (&msg);
			CHECK(0 == rc);
			rc = zmq_recv (poll_items_[1].socket, &msg, 0);
			CHECK(0 == rc);
			if (!response.ParseFromArray (zmq_msg_data (&msg), static_cast<int> (zmq_msg_size (&msg)))) {
				LOG(ERROR) << "Received invalid response.";
				rc = zmq_msg_close (&msg);
				CHECK(0 == rc);
				continue;
			}

			DVLOG(2) << response.DebugString();
/* token */
			token = reinterpret_cast<rfa::sessionLayer::RequestToken*> (response.token());
			try {
				DVLOG(1) << "Received RFA response message, size: " << response.encoded_buffer().size();
/* encoded buffer */
				rfa::common::Buffer buffer (const_cast<unsigned char*> (reinterpret_cast<const unsigned char*> (response.encoded_buffer().c_str())),
							    static_cast<int> (response.encoded_buffer().size()),
							    static_cast<int> (response.encoded_buffer().size()),
							    false);
				respmsg.setEncodedBuffer (buffer);
/* forward to RFA */
				using namespace boost::chrono;
				auto checkpoint = high_resolution_clock::now();
				provider_->Submit (&respmsg, token, nullptr);
				HISTOGRAM_TIMES("Provider.Submit", high_resolution_clock::now() - checkpoint);
				DVLOG(1) << "Response forwarded to RFA.";
				respmsg.clear();
			} catch (rfa::common::InvalidUsageException& e) {
				LOG(ERROR) << "EncodedBuffer::InvalidUsageException: { " <<
						"\"StatusText\": \"" << e.getStatus().getStatusText() << "\""
						" }";
			}
			rc = zmq_msg_close (&msg);
			CHECK(0 == rc);
		}
/* #2 - Abort request */
	} while (0 == (poll_items_[2].revents & ZMQ_POLLIN));

	LOG(INFO) << "Event pump received interrupt request.";

/* cleanup */
	event_queue_->unregisterNotificationClient (dispatcher);
	dispatcher.Clear();
}

/* eof */