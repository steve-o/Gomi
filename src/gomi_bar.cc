/* A basic Velocity Analytics function to calculate basic bar analytics.
 */

#include "gomi_bar.hh"

/* Velocity Analytics Plugin Framework */
#include <FlexRecReader.h>

#include "chromium/logging.hh"

/* Flex Record Trade identifier. */
static const uint32_t kTradeId = 40001;
/* Flex Record name for trades */
static const char* kTradeRecord = "Trade";

/* Index */
static const int kFRLastPrice  = kFRFixedFields + 0;
static const int kFRTickVolume = kFRFixedFields + 19;

/* Name */
static const char* kLastPriceField = "LastPrice";
static const char* kTickVolumeField = "TickVolume";

/* http://en.wikipedia.org/wiki/Unix_epoch */
static const boost::gregorian::date kUnixEpoch (1970, 1, 1);

/* Convert Posix time to Unix Epoch time.
 */
static
__time32_t
to_unix_epoch (
	const boost::posix_time::ptime t
	)
{
	return (t - boost::posix_time::ptime (kUnixEpoch)).total_seconds();
}

/* Calculate bar data with FlexRecord Cursor API.
 *
 * FlexRecReader::Open is an expensive call, ~250ms and allocates virtual memory pages.
 * FlexRecReader::Close is an expensive call, ~150ms.
 * FlexRecReader::Next copies and filters from FlexRecord Primitives into buffers allocated by Open.
 *
 * Returns false on error, true on success.
 */
bool
gomi::bar_t::Calculate (
	const char* symbol_name
	)
{
/* Symbol names */
	std::set<std::string> symbol_set;
	symbol_set.insert (symbol_name);

/* FlexRecord fields */
	double   last_price;
	uint64_t tick_volume;
	std::set<FlexRecBinding> binding_set;
	FlexRecBinding binding (kTradeId);
	binding.Bind (kLastPriceField, &last_price);
	binding.Bind (kTickVolumeField, &tick_volume);
	binding_set.insert (binding);

/* Time period */
	const __time32_t from = to_unix_epoch (tp_.begin());
	const __time32_t till = to_unix_epoch (tp_.end());

/* Open cursor */
	FlexRecReader fr;
	try {
		char error_text[1024];
		const int cursor_status = fr.Open (symbol_set, binding_set, from, till, 0 /* forward */, 0 /* no limit */, error_text);
		if (1 != cursor_status) {
			LOG(ERROR) << "FlexRecReader::Open failed { \"code\": " << cursor_status
				<< ", \"text\": \"" << error_text << "\" }";
			return false;
		}
	} catch (const std::exception& e) {
/* typically out-of-memory exceptions due to insufficient virtual memory */
		LOG(ERROR) << "FlexRecReader::Open raised exception " << e.what();
		return false;
	}

/* iterate through all ticks */
	while (fr.Next()) {
		last_price_ (last_price);
		tick_volume_ (tick_volume);
	}

/* Cleanup */
	fr.Close();

/* State now represents bar time period, which may be zero trades */
	is_null_ = false;

	return true;
}

/* Calculate bar data with FlexRecord Primitives API.
 *
 * Returns false on error, true on success.
 */
bool
gomi::bar_t::Calculate (
	const TBSymbolHandle& handle,
	FlexRecWorkAreaElement* work_area,
	FlexRecViewElement* view_element
	)
{
/* Time period */
	const __time32_t from = to_unix_epoch (tp_.begin());
	const __time32_t till = to_unix_epoch (tp_.end());

	DVLOG(4) << "from: " << from << " till: " << till;
	try {
		U64 numRecs = FlexRecPrimitives::GetFlexRecords (
							handle, 
							const_cast<char*> (kTradeRecord),
							from, till, 0 /* forward */,
							0 /* no limit */,
							view_element->view,
							work_area->data,
							processFlexRecord,
							this /* closure */
								);
		is_null_ = false;
	} catch (const std::exception& e) {
		LOG(ERROR) << "FlexRecPrimitives::GetFlexRecords raised exception " << e.what();
		return false;
	}
	return true;
}

/* Apply a FlexRecord to a partial bar result.
 *
 * Returns <1> to continue processing, <2> to halt processing due to an error.
 */
int
gomi::bar_t::processFlexRecord (
	FRTreeCallbackInfo* info
	)
{
	CHECK(nullptr != info->callersData);
	auto& bar = *reinterpret_cast<bar_t*> (info->callersData);

/* extract from view */
	const double   last_price  = *reinterpret_cast<double*>   (info->theView[kFRLastPrice].data);
	const uint64_t tick_volume = *reinterpret_cast<uint64_t*> (info->theView[kFRTickVolume].data);

/* add to accumulators */
	bar.last_price_  (last_price);
	bar.tick_volume_ (tick_volume);

/* continue processing */
	return 1;
}

/* eof */