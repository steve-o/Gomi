/* A basic Velocity Analytics function to calculate basic bin analytics.
 */

#include "get_bin.hh"

#include <forward_list>
#include <functional>
#include <list>

/* Boost Gregorian calendar */
#include "boost/date_time/gregorian/gregorian.hpp"

/* Velocity Analytics Plugin Framework */
#include <vpf/vpf.h>
#include <FlexRecReader.h>

#include "chromium/logging.hh"
#include "business_day_iterator.hh"

/* FlexRecord Trade identifier. */
static const uint32_t kTradeId = 40001;

/* http://en.wikipedia.org/wiki/Unix_epoch */
static const boost::gregorian::date kUnixEpoch (1970, 1, 1);

/* Is today<date> a business day, per TBSDK.  Assumes local calendar as per TBSDK.
 */
static
bool
is_business_day (
	const boost::gregorian::date d
	)
{
	using namespace boost::posix_time;
	assert (!d.is_not_a_date());
	const __time32_t time32 = (ptime (d) - ptime (kUnixEpoch)).total_seconds();
	BusinessDayInfo bd;
	return (0 != TBPrimitives::BusinessDay (time32, &bd));
}

/* Calcaulte the __time32_t of the start and end of each time slice for
 * bin parameters provided.
 */
static
void
get_bin_window (
	const boost::gregorian::date& date,
	const boost::local_time::time_zone_ptr& tz,
	const boost::posix_time::time_duration& start,	/* locale time */
	const boost::posix_time::time_duration& end,	/* locale time */
	__time32_t* from,				/* UTC */
	__time32_t* till				/* UTC */
	)
{
	using namespace boost;
	using namespace local_time;
	using namespace posix_time;

/* start: apply provided time-of-day */
	const local_date_time start_ldt (date, start, tz, local_date_time::NOT_DATE_TIME_ON_ERROR);
	assert (!start_ldt.is_not_a_date_time());
	*from = (start_ldt.utc_time() - ptime (kUnixEpoch)).total_seconds();

/* end */
	const local_date_time end_ldt (date, end, tz, local_date_time::NOT_DATE_TIME_ON_ERROR);
	assert (!end_ldt.is_not_a_date_time());
	*till = (end_ldt.utc_time() - ptime (kUnixEpoch)).total_seconds();

	DVLOG(4) << "converted from locale:" << to_simple_string (start)
		<< " to UTC:" << to_simple_string (start_ldt.utc_time());
	DVLOG(4) << "converted from locale:" << to_simple_string (end)
		<< " to UTC:" << to_simple_string (end_ldt.utc_time());
}

/*  IN: bin populated with symbol names.
 * OUT: bin populated with analytic values from start to end.
 *
 * caller must reset analytic values, is_null values if clean query is required,
 * existing values can be used to extended a previous query.
 */

/* Flex Record Cursor API reference implementation.
 *
 * slow.
 */

namespace gomi {
namespace reference {

void
get_bin (
	const bin_t& bin,
	std::vector<std::shared_ptr<janku_t>>& query
	)
{
// BUG: FlexRecReader caches last cursor binding_set, create new reader per iteration.
//	FlexRecReader fr;

	DLOG(INFO) << "get_bin ("
		"bin: { "
			"start: " << bin.bin_start << ", "
			"end: " << bin.bin_end << ", "
			"tz: " << bin.bin_tz->std_zone_abbrev() << ", "
			"day_count: " << bin.bin_day_count << " "
		"}"
		")";

/* no-op */
	if (query.empty() || 0 == bin.bin_day_count) {
		DVLOG(4) << "empty query";
		return;
	}

	std::set<FlexRecBinding> binding_set;
	FlexRecBinding binding (kTradeId);
	double last_price;
	uint64_t tick_volume;

/* take fields from first query, re-use for every other query */
	const std::string& last_price_field = query[0]->last_price_field,
		tick_volume_field = query[0]->tick_volume_field;
	assert (!last_price_field.empty());
	assert (!tick_volume_field.empty());
	binding.Bind (last_price_field.c_str(), &last_price);
	binding.Bind (tick_volume_field.c_str(), &tick_volume);
	binding_set.insert (binding);

	DVLOG(4) << "binding with fields: last_price=" << last_price_field << " tick_volume=" << tick_volume_field;

	std::for_each (query.begin(), query.end(),
		[&](std::shared_ptr<janku_t>& it)
	{
		FlexRecReader fr;

		DVLOG(4) << "iteration: symbol=" << it->symbol_name;

/* source instruments */
		std::set<std::string> symbol_set;
		symbol_set.insert (it->symbol_name);

		double close_price;           bool has_close = false;
		double tenday_open_price;
		double fifteenday_open_price;
		double twentyday_open_price;
		uint64_t accumulated_volume, num_moves, day_count = 0;

/* reset state */
		it->clear();

/* do not assume today is a business day */
		using namespace boost::local_time;
		const auto& now_in_tz = local_sec_clock::local_time (bin.bin_tz);
		const auto& today_in_tz = now_in_tz.local_time().date();
		auto start_date (today_in_tz);
		
		while (!is_business_day (start_date))
			start_date -= boost::gregorian::date_duration (1);

/* save close of first business-day of analytic period */
		const local_date_time close_ldt (start_date, bin.bin_end, bin.bin_tz, local_date_time::NOT_DATE_TIME_ON_ERROR);
		assert (!close_ldt.is_not_a_date_time());
		it->close_time = close_ldt.utc_time();
		it->trading_day_count = 0;

		vhayu::business_day_iterator bd_itr (start_date);
		for (unsigned i = 0; i < bin.bin_day_count; ++i, --bd_itr)
		{
			__time32_t from, till;

			get_bin_window (*bd_itr, bin.bin_tz, bin.bin_start, bin.bin_end, &from, &till);

			DVLOG(4) << "#" << i << " from=" << from << " till=" << till;

/* reset for each day */
			bool has_tenday = false, has_fifteenday = false, has_twentyday = false;
			uint64_t day_volume = 0;
			num_moves = 0;

			try {
				char error_text[1024];
				const int cursor_status = fr.Open (symbol_set, binding_set, from, till, 0 /* forward */, 0 /* no limit */, error_text);
				if (1 != cursor_status) {
					LOG(ERROR) << "FlexRecReader::Open failed { \"code\": " << cursor_status
						<< ", \"text\": \"" << error_text << "\" }";
					continue;
				}
			} catch (std::exception& e) {
				LOG(ERROR) << "FlexRecReader::Open raised exception " << e.what();
				continue;
			}
			if (fr.Next())
			{
/* first trade */
				if (!has_twentyday && i < 20) {
					twentyday_open_price = last_price;
					has_twentyday = true;
				}
				if (!has_fifteenday && i < 15) {
					fifteenday_open_price = last_price;
					has_fifteenday = true;
				}
				if (!has_tenday && i < 10) {
					tenday_open_price = last_price;
					has_tenday = true;
				}
				day_volume += tick_volume;
				++num_moves;
/* till end */
				while (fr.Next()) {
					day_volume += tick_volume;
					++num_moves;
				}
			}
			fr.Close();

/* test for closing price */
			if (!has_close && num_moves > 0) {
				close_price = last_price;
				has_close = true;
			}

/* test for zero-trade day */
			if (num_moves > 0)
				++day_count;

			DVLOG(4) << "day " << to_simple_string (*bd_itr) <<
				" acvol_1=" << day_volume << 
				" num_moves=" << num_moves;

			it->total_moves += num_moves;
			accumulated_volume += day_volume;
			if (it->is_null) {
				it->is_null = false;
/* sets smallest-moves to zero */
				it->maximum_moves = it->minimum_moves = it->smallest_moves = num_moves;
			} else {
				if (num_moves > 0) {
/* edge case: smallest-moves should not be zero if a trade-day is available */
					if (0 == it->maximum_moves) it->smallest_moves = num_moves;
					else if (num_moves < it->smallest_moves) it->smallest_moves = num_moves;
					if (num_moves > it->maximum_moves) it->maximum_moves = num_moves;
				}
				if (num_moves < it->minimum_moves) it->minimum_moves = num_moves;
			}
		}

/* finalize */
		if (day_count > 0) {
			if (tenday_open_price > 0.0)
				it->tenday_percentage_change     = (100.0 * (last_price - tenday_open_price)) / tenday_open_price;
			if (fifteenday_open_price > 0.0)
				it->fifteenday_percentage_change = (100.0 * (last_price - fifteenday_open_price)) / fifteenday_open_price;
			if (twentyday_open_price > 0.0)
				it->twentyday_percentage_change  = (100.0 * (last_price - twentyday_open_price)) / twentyday_open_price;
			if (accumulated_volume > 0) {
				it->average_volume = accumulated_volume / bin.bin_day_count;
				it->average_nonzero_volume = accumulated_volume / day_count;
			}
		}

/* discovered day count with trades */
		it->trading_day_count = day_count;

		DVLOG(1) << "iteration complete,"
			" day_count=" << it->trading_day_count <<
			" acvol=" << accumulated_volume << 
			" avgvol=" << it->average_volume <<
			" avgrvl=" << it->average_nonzero_volume <<	/* average real volume */
			" count=" << it->total_moves <<
			" hicnt=" << it->maximum_moves <<
			" locnt=" << it->minimum_moves <<
			" smcnt=" << it->smallest_moves <<
			" pctchg_10d=" << it->tenday_percentage_change <<
			" pctchg_15d=" << it->fifteenday_percentage_change <<
			" pctchg_20d=" << it->twentyday_percentage_change;
	});

	DLOG(INFO) << "get_bin() finished.";
}

} // namespace reference
} // namespace gomi

/* Flex Record Primitive API version.
 *
 * Faster API that sits underneath documented cursor API.
 */

namespace gomi {
namespace primitive {

static
int
on_flexrecord (
	FRTreeCallbackInfo* info
	)
{
	if (nullptr == info->callersData) {
		LOG(ERROR) << "Invalid closure on FlexRecordTreeCallback";
		return 2;
	}
	gomi::analytic_state_t* state = static_cast<gomi::analytic_state_t*> (info->callersData);
	const VarFieldsView* view = info->theView;

	const double last_price    =   *(const double*)view[kFRFixedFields +  0].data;
	const uint64_t tick_volume = *(const uint64_t*)view[kFRFixedFields + 19].data;

	if (0 == state->num_moves)
		state->open_price = last_price;
	state->accumulated_volume += tick_volume;
	++(state->num_moves);
	state->close_price = last_price;

	return 1;
}

void
get_bin (
	const bin_t& bin,
	std::vector<std::shared_ptr<janku_t>>& query,
	FlexRecWorkAreaElement* work_area,
	FlexRecViewElement* view_element
	)
{
	DLOG(INFO) << "get_bin ("
		"bin: { "
			"start: " << bin.bin_start << ", "
			"end: " << bin.bin_end << ", "
			"tz: " << bin.bin_tz->std_zone_abbrev() << ", "
			"day_count: " << bin.bin_day_count << " "
		"}"
		")";

/* no-op */
	if (query.empty() || 0 == bin.bin_day_count) {
		DVLOG(4) << "empty query";
		return;
	}

/* do not assume today is a business day */
	using namespace boost::local_time;
	auto now_in_tz = local_sec_clock::local_time (bin.bin_tz);

/* verify bin is not in the future */
	if (now_in_tz.local_time().time_of_day() < bin.bin_end) {
		LOG(WARNING) << "bin in future, adjusting to recalculate yesterdays analytic.";
		now_in_tz -= boost::gregorian::days (1);
	}

	const auto today_in_tz = now_in_tz.local_time().date();
	auto start_date (today_in_tz);
	const local_date_time close_ldt (start_date, bin.bin_end, bin.bin_tz, local_date_time::NOT_DATE_TIME_ON_ERROR);
	assert (!close_ldt.is_not_a_date_time());
	while (!is_business_day (start_date))
		start_date -= boost::gregorian::date_duration (1);
	vhayu::business_day_iterator bd_it (start_date);

/* pre-iterate days due to slow API */
	std::vector<boost::gregorian::date> business_days (bin.bin_day_count);
	for (unsigned i = 0; i < bin.bin_day_count; ++i, --bd_it)
		business_days[i] = *bd_it;

	std::for_each (query.begin(), query.end(),
		[&](std::shared_ptr<janku_t>& it)
	{
		DVLOG(4) << "iteration: symbol=" << it->symbol_name;

		if (nullptr == it->handle) {
			LOG(WARNING) << "Skipping invalid symbol pointer.";
			return;
		}

		double tenday_open_price, fifteenday_open_price, twentyday_open_price;
		uint64_t accumulated_volume, num_moves, day_count = 0;

/* reset state */
		it->clear();

/* save close of first business-day of analytic period */
		it->close_time = close_ldt.utc_time();

/* keep state pointer to open closing price */
		auto state_it = it->analytic_state.begin();
		if (it->analytic_state.end() == state_it) {
/* clean cache */
			it->analytic_state.resize (bin.bin_day_count);
			state_it = it->analytic_state.begin();
		} else if (!state_it->is_null) {
/* potential cached state */
			__time32_t from, till;
			get_bin_window (business_days[0], bin.bin_tz, bin.bin_start, bin.bin_end, &from, &till);
			if (till != state_it->close_time) {
/* first days iteration, otherwise overwrite */
				it->analytic_state.pop_back();
				analytic_state_t empty;
				it->analytic_state.push_front (empty);
			}
		} else {
/* overwrite */
		}

		for (unsigned i = 0; i < bin.bin_day_count; ++i, ++state_it)
		{
			__time32_t from, till;
			get_bin_window (business_days[i], bin.bin_tz, bin.bin_start, bin.bin_end, &from, &till);
			DVLOG(4) << "#" << i << " from=" << from << " till=" << till;

			if (!state_it->open (till))
			{
				try {
					char error_text[1024];
					U64 numRecs = FlexRecPrimitives::GetFlexRecords (
						it->handle, 
						"Trade",
						from, till, 0 /* forward */,
						0 /* no limit */,
						view_element->view,
						work_area->data,
						on_flexrecord,
						&(*state_it) /* closure */
					);
				} catch (std::exception& e) {
					LOG(ERROR) << "FlexRecPrimitives::GetFlexRecords raised exception " << e.what();
					continue;
				}
			}

			if (state_it->num_moves > 0) {
/* first trade */
				if (i < 20)
					twentyday_open_price = state_it->open_price;
				if (i < 15)
					fifteenday_open_price = state_it->open_price;
				if (i < 10)
					tenday_open_price = state_it->open_price;
/* non-zero-trade day count */
				++day_count;
				it->total_moves    += state_it->num_moves;
				accumulated_volume += state_it->accumulated_volume;
			}

			DVLOG(4) << "day " << to_simple_string (business_days[i]) <<
				" acvol_1=" << state_it->accumulated_volume << 
				" num_moves=" << state_it->num_moves;

/* first analytic result */
			if (it->is_null) {
				it->is_null = false;
/* sets smallest-moves to zero */
				it->maximum_moves = it->minimum_moves = it->smallest_moves = state_it->num_moves;
			} else {
				if (state_it->num_moves > 0) {
/* edge case: smallest-moves should not be zero if a trade-day is available */
					if (0 == it->maximum_moves) it->smallest_moves = state_it->num_moves;
					else if (state_it->num_moves < it->smallest_moves) it->smallest_moves = state_it->num_moves;
					if (state_it->num_moves > it->maximum_moves) it->maximum_moves = state_it->num_moves;
				}
				if (state_it->num_moves < it->minimum_moves) it->minimum_moves = state_it->num_moves;
			}
		}

/* finalize */
		if (day_count > 0) {
			if (tenday_open_price > 0.0)
				it->tenday_percentage_change     = (100.0 * (state_it->close_price - tenday_open_price)) / tenday_open_price;
			if (fifteenday_open_price > 0.0)
				it->fifteenday_percentage_change = (100.0 * (state_it->close_price - fifteenday_open_price)) / fifteenday_open_price;
			if (twentyday_open_price > 0.0)
				it->twentyday_percentage_change  = (100.0 * (state_it->close_price - twentyday_open_price)) / twentyday_open_price;
			if (accumulated_volume > 0) {
				it->average_volume = accumulated_volume / bin.bin_day_count;
				it->average_nonzero_volume = accumulated_volume / day_count;
			}
/* discovered day count with trades */
			it->trading_day_count = day_count;
		}

		DVLOG(1) << "iteration complete,"
			" day_count=" << it->trading_day_count <<
			" acvol=" << accumulated_volume << 
			" avgvol=" << it->average_volume <<
			" avgrvl=" << it->average_nonzero_volume <<	/* average real volume */
			" count=" << it->total_moves <<
			" hicnt=" << it->maximum_moves <<
			" locnt=" << it->minimum_moves <<
			" smcnt=" << it->smallest_moves <<
			" pctchg_10d=" << it->tenday_percentage_change <<
			" pctchg_15d=" << it->fifteenday_percentage_change <<
			" pctchg_20d=" << it->twentyday_percentage_change;
	});

	DLOG(INFO) << "get_bin() finished.";
}

} // namespace primitive
} // namespace gomi


/* single iterator implementation.
 *
 * incorrectly assumes cursor walks single timeline of datastore.  iterate once through timeline picking
 * up all trades as and when they occur.
 */

namespace gomi {
namespace single_iterator {

	class symbol_t : boost::noncopyable
	{
	public:
		symbol_t (std::shared_ptr<janku_t> janku_, const boost::posix_time::ptime& close_ptime) :
			has_close (false),
			day_count (0),
			janku (janku_)
		{
/* reset state */
			janku->clear();

/* save close of first business-day of analytic period */
			janku->close_time = close_ptime;
		}

		double last_price;

		double close_price;		bool has_close;
		double tenday_open_price;	bool has_tenday;
		double fifteenday_open_price;	bool has_fifteenday;
		double twentyday_open_price;	bool has_twentyday;
		uint64_t accumulated_volume, num_moves, day_count, day_volume;

		std::shared_ptr<janku_t> janku;
	};

void
get_bin (
	const bin_t& bin,
	std::vector<std::shared_ptr<janku_t>>& query
	)
{
	using namespace boost::posix_time;

	DLOG(INFO) << "get_bin ("
		"bin: { "
			"start: " << bin.bin_start << ", "
			"end: " << bin.bin_end << ", "
			"tz: " << bin.bin_tz->std_zone_abbrev() << ", "
			"day_count: " << bin.bin_day_count << " "
		"}"
		")";

/* no-op */
	if (query.empty() || 0 == bin.bin_day_count) {
		DVLOG(4) << "empty query";
		return;
	}

// BUG: FlexRecReader caches last cursor binding_set, create new reader per iteration if different binding is required.
	FlexRecReader fr;

/* prepare query data sets */
	std::unordered_map<std::string, std::shared_ptr<symbol_t>> symbol_map;
	std::set<std::string> symbol_set;
	std::set<FlexRecBinding> binding_set;
	FlexRecBinding binding (kTradeId);
	double last_price;
	uint64_t tick_volume;

/* take fields from first query, re-use for every other query */
	const std::string& last_price_field = query[0]->last_price_field,
		tick_volume_field = query[0]->tick_volume_field;
	assert (!last_price_field.empty());
	assert (!tick_volume_field.empty());
	binding.Bind (last_price_field.c_str(), &last_price);
	binding.Bind (tick_volume_field.c_str(), &tick_volume);
	binding_set.insert (binding);

	DVLOG(4) << "binding with fields: last_price=" << last_price_field << " tick_volume=" << tick_volume_field;

/* do not assume today is a business day */
	using namespace boost::local_time;
	const auto& now_in_tz = local_sec_clock::local_time (bin.bin_tz);
	const auto& today_in_tz = now_in_tz.local_time().date();
	auto start_date (today_in_tz);
		
	while (!is_business_day (start_date))
		start_date -= boost::gregorian::date_duration (1);

/* save close of first business-day of analytic period */
	const local_date_time close_ldt (start_date, bin.bin_end, bin.bin_tz, local_date_time::NOT_DATE_TIME_ON_ERROR);
	assert (!close_ldt.is_not_a_date_time());

/* convert multiple queries into a single query expression */
	std::for_each (query.begin(), query.end(), [&symbol_set, &symbol_map, &close_ldt](std::shared_ptr<janku_t>& it)
	{
		auto new_symbol = std::make_shared<symbol_t> (it, close_ldt.utc_time());
		auto status = symbol_map.emplace (std::make_pair (it->symbol_name, std::move (new_symbol)));
		symbol_set.emplace (status.first->first);
	});

/* run one single big query for each day*/
	vhayu::business_day_iterator bd_itr (start_date);
	for (unsigned i = 0; i < bin.bin_day_count; ++i, --bd_itr)
	{
const ptime t0 (microsec_clock::universal_time());
		__time32_t from, till;
		get_bin_window (*bd_itr, bin.bin_tz, bin.bin_start, bin.bin_end, &from, &till);

		DVLOG(4) << "#" << i << " from=" << from << " till=" << till;

/* reset for each day */
		std::for_each (symbol_map.begin(), symbol_map.end(), [](std::pair<const std::string, std::shared_ptr<symbol_t>>& it)
		{
			auto symbol = it.second.get();
			symbol->has_tenday = symbol->has_fifteenday = symbol->has_twentyday = false;
			symbol->day_volume = symbol->num_moves = 0;
		});

const ptime t1 (microsec_clock::universal_time());
//		FlexRecReader fr;
/*
LOG(INFO) << "symbol_set: " << symbol_set.size()
	<< ", binding_set: " << binding_set.size()
	<< ", from: " << from
	<< ", till: " << till;
	*/
		try {
			char error_text[1024];
			const int cursor_status = fr.Open (symbol_set, binding_set, from, till, 0 /* forward */, 0 /* no limit */, error_text);
			if (1 != cursor_status) {
				LOG(ERROR) << "FlexRecReader::Open failed { \"code\": " << cursor_status
					<< ", \"text\": \"" << error_text << "\" }";
				continue;
			}
		} catch (std::exception& e) {
			LOG(ERROR) << "FlexRecReader::Open raised exception " << e.what();
			continue;
		}
const ptime t2 (microsec_clock::universal_time());
#if 1
		while (fr.Next()) {
			auto symbol = symbol_map[fr.GetCurrentSymbolName()];
/* first trade */
			if (0 == symbol->num_moves) {
				if (!symbol->has_twentyday && i < 20) {
					symbol->twentyday_open_price = last_price;
					symbol->has_twentyday = true;
				}
				if (!symbol->has_fifteenday && i < 15) {
					symbol->fifteenday_open_price = last_price;
					symbol->has_fifteenday = true;
				}
				if (!symbol->has_tenday && i < 10) {
					symbol->tenday_open_price = last_price;
					symbol->has_tenday = true;
				}
			}
			symbol->last_price = last_price;
			symbol->day_volume += tick_volume;
			++(symbol->num_moves);
		}
#endif
const ptime t3 (microsec_clock::universal_time());
		fr.Close();
const ptime t4 (microsec_clock::universal_time());

/* close of day */
		std::for_each (symbol_map.begin(), symbol_map.end(), [](std::pair<const std::string, std::shared_ptr<symbol_t>>& it)
		{
			auto symbol = it.second.get();
/* test for closing price */
			if (!symbol->has_close && symbol->num_moves > 0) {
				symbol->close_price = symbol->last_price;
				symbol->has_close = true;
			}

/* test for zero-trade day */
			if (symbol->num_moves > 0)
				++symbol->day_count;

			symbol->janku->total_moves += symbol->num_moves;
			symbol->accumulated_volume += symbol->day_volume;
			if (symbol->janku->is_null) {
				symbol->janku->is_null = false;
/* sets smallest-moves to zero */
				symbol->janku->maximum_moves = symbol->janku->minimum_moves = symbol->janku->smallest_moves = symbol->num_moves;
			} else {
				if (symbol->num_moves > 0) {
/* edge case: smallest-moves should not be zero if a trade-day is available */
					if (0 == symbol->janku->maximum_moves) symbol->janku->smallest_moves = symbol->num_moves;
					else if (symbol->num_moves < symbol->janku->smallest_moves) symbol->janku->smallest_moves = symbol->num_moves;
					if (symbol->num_moves > symbol->janku->maximum_moves) symbol->janku->maximum_moves = symbol->num_moves;
				}
				if (symbol->num_moves < symbol->janku->minimum_moves) symbol->janku->minimum_moves = symbol->num_moves;
			}
		});
const ptime t5 (microsec_clock::universal_time());
LOG(INFO) << "timing: t0-t1=" << (t1-t0).total_milliseconds() << "ms"
	            " t1-t2=" << (t2-t1).total_milliseconds() << "ms"
	            " t2-t3=" << (t3-t2).total_milliseconds() << "ms"
	            " t3-t4=" << (t4-t3).total_milliseconds() << "ms"
	            " t4-t5=" << (t5-t4).total_milliseconds() << "ms";
	}

/* finalize */
	std::for_each (symbol_map.begin(), symbol_map.end(), [&bin](std::pair<const std::string, std::shared_ptr<symbol_t>>& it)
	{
		auto symbol = it.second.get();
		if (symbol->day_count > 0) {
			if (symbol->tenday_open_price > 0.0)
				symbol->janku->tenday_percentage_change     = (100.0 * (symbol->last_price - symbol->tenday_open_price)) / symbol->tenday_open_price;
			if (symbol->fifteenday_open_price > 0.0)
				symbol->janku->fifteenday_percentage_change = (100.0 * (symbol->last_price - symbol->fifteenday_open_price)) / symbol->fifteenday_open_price;
			if (symbol->twentyday_open_price > 0.0)
				symbol->janku->twentyday_percentage_change  = (100.0 * (symbol->last_price - symbol->twentyday_open_price)) / symbol->twentyday_open_price;
			if (symbol->accumulated_volume > 0) {
				symbol->janku->average_volume = symbol->accumulated_volume / bin.bin_day_count;
				symbol->janku->average_nonzero_volume = symbol->accumulated_volume / symbol->day_count;
			}
		}

/* discovered day count with trades */
		symbol->janku->trading_day_count = symbol->day_count;
	});

	DLOG(INFO) << "get_bin() finished.";
}

} // namespace single_iterator
} // namespace gomi

/* eof */