/* A basic Velocity Analytics function to calculate basic bin analytics.
 */

#include "get_bin.hh"

#include <forward_list>
#include <functional>
#include <list>

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
#if 1
void
gomi::get_bin (
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

			fr.Open (symbol_set, binding_set, from, till, 0 /* forward */, 0 /* no limit */);
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
#else

namespace hilo {

	class symbol_t : boost::noncopyable
	{
	public:
		symbol_t() :
			is_null (true)
		{
		}

		symbol_t (bool is_null_) :
			is_null (is_null_)
		{
		}

		void set_last_value (size_t idx_, double value_) {
			if (idx_ >= last_value.size())
				last_value.resize (1 + idx_);
			last_value[idx_] = value_;
		}

		std::forward_list<std::shared_ptr<hilo_t>> non_synthetic_list;
/* synthetic members */
		std::vector<double> last_value;
		bool is_null;
		std::list<std::pair<std::shared_ptr<symbol_t>, std::shared_ptr<hilo_t>>> as_first_leg_list, as_second_leg_list;
	};

} /* namespace hilo */

void
hilo::get_hilo (
	std::vector<std::shared_ptr<hilo::hilo_t>>& query,
	__time32_t	from,		/* legacy from before 2003, yay. */
	__time32_t	till
	)
{
	DLOG(INFO) << "get_hilo(from=" << from << " till=" << till << ")";

	std::unordered_map<std::string, std::shared_ptr<symbol_t>> symbol_map;
	std::set<std::string> symbol_set;
	std::set<FlexRecBinding> binding_set;
	std::unordered_map<std::string, size_t> field_map;
	std::vector<double> fields;

/* convert multiple queries into a single query expression */
	std::for_each (query.begin(), query.end(),
		[&](std::shared_ptr<hilo_t>& query_it)
	{
		auto symbol_it = symbol_map.find (query_it->legs.first.symbol_name);
/* add new symbol into set */
		if (symbol_map.end() == symbol_it) {
			std::shared_ptr<symbol_t> new_symbol (new symbol_t (query_it->legs.first.is_null));
			auto status = symbol_map.emplace (std::make_pair (query_it->legs.first.symbol_name, std::move (new_symbol)));
			symbol_it = status.first;
			symbol_set.emplace (symbol_it->first);
		}

/* map field into binding index and set last value cache */
		auto add_field = [&](std::string& name) -> size_t {
			auto it = field_map.find (name);
			size_t idx;
			if (field_map.end() == it) {
				idx = fields.size();
				fields.resize (idx + 1);
				field_map[name] = idx;
			} else {
				idx = it->second;
			}
			return idx;
		};

		query_it->legs.first.bid_field_idx = add_field (query_it->legs.first.bid_field);
		symbol_it->second->set_last_value (query_it->legs.first.bid_field_idx, query_it->legs.first.last_bid);

		query_it->legs.first.ask_field_idx = add_field (query_it->legs.first.ask_field);
		symbol_it->second->set_last_value (query_it->legs.first.ask_field_idx, query_it->legs.first.last_ask);

		if (!query_it->is_synthetic) {
			symbol_it->second->non_synthetic_list.push_front (query_it);
			return;
		}

/* this ric is a synthetic pair, XxxYyy, add to list of Xxx a link to -Yyy and add to Yyy a link to Xxx- */
		assert (query_it->legs.first.symbol_name != query_it->legs.second.symbol_name);

/* Xxx */
		auto first_it = std::ref (symbol_it).get();
/* Yyy */
		auto second_it = symbol_map.find (query_it->legs.second.symbol_name);
		if (symbol_map.end() == second_it) {
			std::shared_ptr<symbol_t> new_symbol (new symbol_t (query_it->legs.second.is_null));
			auto status = symbol_map.emplace (std::make_pair (query_it->legs.second.symbol_name, std::move (new_symbol)));
			second_it = status.first;
			symbol_set.emplace (second_it->first);
		}

		query_it->legs.second.bid_field_idx = add_field (query_it->legs.second.bid_field);
		second_it->second->set_last_value (query_it->legs.second.bid_field_idx, query_it->legs.second.last_bid);

		query_it->legs.second.ask_field_idx = add_field (query_it->legs.second.ask_field);
		second_it->second->set_last_value (query_it->legs.second.ask_field_idx, query_it->legs.second.last_ask);

		first_it->second->as_first_leg_list.emplace_back (std::make_pair (second_it->second, query_it));
		second_it->second->as_second_leg_list.emplace_back (std::make_pair (first_it->second, query_it));
	});

	FlexRecReader fr;
	FlexRecBinding binding (kQuoteId);

/* copy finalized bindings into new set */
	std::for_each (field_map.begin(), field_map.end(),
		[&](std::pair<const std::string, size_t>& field_pair)
	{
		binding.Bind (field_pair.first.c_str(), &fields[field_pair.second]);
	});
	binding_set.insert (binding);

	auto update_non_synthetic = [&](hilo_t& query_item, std::shared_ptr<symbol_t>& symbol) {
		const double bid_price = fields[query_item.legs.first.bid_field_idx];
		const double ask_price = fields[query_item.legs.first.ask_field_idx];
		if (query_item.is_null) {
			query_item.is_null = false;
			query_item.low  = bid_price;
			query_item.high = ask_price;
			DLOG(INFO) << query_item.name << " start low=" << bid_price << " high=" << ask_price;
			return;
		}
		if (bid_price < query_item.low) {
			query_item.low  = bid_price;
			DLOG(INFO) << query_item.name << " new low=" << bid_price;
		}
		if (ask_price > query_item.high) {
			query_item.high = ask_price;
			DLOG(INFO) << query_item.name << " new high=" << ask_price;
		}
	};

	auto update_synthetic = [&fr](hilo_t& query_item, symbol_t& first_leg, symbol_t& second_leg) {
		if (first_leg.is_null || second_leg.is_null)
			return;

/* lambda to function pointer is incomplete in MSVC2010, punt to the compiler to clean up. */
		auto math_func = [&query_item](double a, double b) -> double {
			if (MATH_OP_TIMES == query_item.math_op)
				return (double)(a * b);
			else if (b == 0.0)
				return b;
			else
				return (double)(a / b);
		};

		const double synthetic_bid_price = math_func (first_leg.last_value[query_item.legs.first.bid_field_idx], second_leg.last_value[query_item.legs.second.bid_field_idx]);
		const double synthetic_ask_price = math_func (first_leg.last_value[query_item.legs.first.ask_field_idx], second_leg.last_value[query_item.legs.second.ask_field_idx]);

		if (query_item.is_null) {
			query_item.is_null = false;
			query_item.low  = synthetic_bid_price;
			query_item.high = synthetic_ask_price;
			DLOG(INFO) << "Start low=" << synthetic_bid_price << " high=" << synthetic_ask_price;
			return;
		}

		if (synthetic_bid_price < query_item.low) {
			query_item.low  = synthetic_bid_price;
			DLOG(INFO) << "New low=" << query_item.low;
		}
		if (synthetic_ask_price > query_item.high) {
			query_item.high = synthetic_ask_price;
			DLOG(INFO) << "New high=" << query_item.high;
		}
	};

/* run one single big query */
	fr.Open (symbol_set, binding_set, from, till, 0 /* forward */, 0 /* no limit */);
	while (fr.Next()) {
		auto symbol = symbol_map[fr.GetCurrentSymbolName()];
/* non-synthetic */
		std::for_each (symbol->non_synthetic_list.begin(), symbol->non_synthetic_list.end(),
			[&](std::shared_ptr<hilo_t>& query_it)
		{
			update_non_synthetic (*query_it.get(), symbol);
		});
/* synthetics */
		symbol->is_null = false;
/* cache last value */
		for (int i = fields.size() - 1; i >= 0; i--)
			symbol->last_value[i] = fields[i];
		std::for_each (symbol->as_first_leg_list.begin(), symbol->as_first_leg_list.end(),
			[&](std::pair<std::shared_ptr<symbol_t>, std::shared_ptr<hilo_t>> second_leg)
		{
			update_synthetic (*second_leg.second.get(), *symbol.get(), *second_leg.first.get());
		});
		std::for_each (symbol->as_second_leg_list.begin(), symbol->as_second_leg_list.end(),
			[&](std::pair<std::shared_ptr<symbol_t>, std::shared_ptr<hilo_t>> first_leg)
		{
			update_synthetic (*first_leg.second.get(), *first_leg.first.get(), *symbol.get());
		});
	}	
	fr.Close();

/* cache symbol last values back into query vector, 1:M operation */
	std::for_each (query.begin(), query.end(),
		[&](std::shared_ptr<hilo_t>& query_it)
	{
		auto first_leg = symbol_map[query_it->legs.first.symbol_name];
		query_it->legs.first.is_null  = first_leg->is_null;
		query_it->legs.first.last_bid = first_leg->last_value[query_it->legs.first.bid_field_idx];
		query_it->legs.first.last_ask = first_leg->last_value[query_it->legs.first.ask_field_idx];

		if (!query_it->is_synthetic) return;

		auto second_leg = symbol_map[query_it->legs.second.symbol_name];
		query_it->legs.second.is_null  = second_leg->is_null;
		query_it->legs.second.last_bid = second_leg->last_value[query_it->legs.second.bid_field_idx];
		query_it->legs.second.last_ask = second_leg->last_value[query_it->legs.second.ask_field_idx];
	});

	DLOG(INFO) << "get_hilo() finished.";
}
#endif

/* eof */