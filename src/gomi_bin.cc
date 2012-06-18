/* A basic Velocity Analytics function to calculate basic bin analytics.
 */

#include "gomi_bin.hh"
#include "gomi_bar.hh"

#include "chromium/logging.hh"
#include "business_day_iterator.hh"

/* FlexRecord Trade identifier. */
static const uint32_t kTradeId = 40001;

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
	const __time32_t time32 = to_unix_epoch (boost::posix_time::ptime (d));
	return (0 != TBPrimitives::BusinessDay (time32, &bd));
}

/* Calculate the start and end of each time of a bin for a given date.
 */
static
boost::posix_time::time_period
to_time_period (
	const gomi::bin_decl_t& bin_decl,
	const boost::gregorian::date& date
	)
{
	using namespace boost;
	using namespace local_time;
	using namespace posix_time;

/* start: apply provided time-of-day */
	const local_date_time start_ldt (date, bin_decl.bin_start, bin_decl.bin_tz, local_date_time::NOT_DATE_TIME_ON_ERROR);
	CHECK (!start_ldt.is_not_a_date_time());

/* end */
	const local_date_time end_ldt (date, bin_decl.bin_end, bin_decl.bin_tz, local_date_time::NOT_DATE_TIME_ON_ERROR);
	CHECK (!end_ldt.is_not_a_date_time());

	time_period tp (start_ldt.utc_time(), end_ldt.utc_time());
	return tp;
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

bool
gomi::bin_t::Calculate (
	const boost::gregorian::date& date,
	FlexRecWorkAreaElement* work_area,
	FlexRecViewElement* view_element
	)
{
	DLOG(INFO) << "Calculate (date: " << to_simple_string (date) << ")";

/* reset state */
	Clear();

/* no-op */
	if (0 == bin_decl_.bin_day_count) {
		DVLOG(4) << "empty query";
		return true;
	}

/* do not assume today is a business day */
	using namespace boost::local_time;
	const auto& now_in_tz = local_sec_clock::local_time (bin_decl_.bin_tz);
	const auto& today_in_tz = now_in_tz.local_time().date();
	auto start_date (today_in_tz);
	while (!is_business_day (start_date))
		start_date -= boost::gregorian::date_duration (1);
	vhayu::business_day_iterator bd_itr (start_date);
/* save close of first business-day of analytic period */
	const local_date_time close_ldt (start_date, bin_decl_.bin_end, bin_decl_.bin_tz, local_date_time::NOT_DATE_TIME_ON_ERROR);
	CHECK (!close_ldt.is_not_a_date_time());
	close_time_ = close_ldt.utc_time();

	for (unsigned t = 0; t < bin_decl_.bin_day_count; ++t, --bd_itr)
	{
		const auto tp = to_time_period (bin_decl_, *bd_itr);

		bars_[t].Clear();
		bars_[t].SetTimePeriod (tp);
#if 0
		bars_[t].Calculate (symbol_name_.c_str());
#else
		bars_[t].Calculate (handle_, work_area, view_element);
#endif
		LOG(INFO) << "bar: { "
			  "symbol: \"" << symbol_name_ << "\""
			", day: " << t <<
			", time_period: \"" << to_simple_string (tp) << "\""
			", open: " << bars_[t].GetOpenPrice() <<
			", close: " << bars_[t].GetClosePrice() <<
			", moves: " << bars_[t].GetNumberMoves() <<
			", volume: " << bars_[t].GetAccumulatedVolume() <<
			" }";
	}

/* collate result set */
	double tenday_open_price, fifteenday_open_price, twentyday_open_price;
	uint64_t accumulated_volume = 0;
	for (unsigned t = 0; t < bin_decl_.bin_day_count; ++t)
	{
		if (t < 20) twentyday_open_price  = bars_[t].GetOpenPrice();
		if (t < 15) fifteenday_open_price = bars_[t].GetOpenPrice();
		if (t < 10) tenday_open_price     = bars_[t].GetOpenPrice();

		accumulated_volume += bars_[t].GetAccumulatedVolume();
		total_moves_       += bars_[t].GetNumberMoves();

/* test for zero-trade day */
		if (bars_[t].GetNumberMoves() > 0)
			++trading_day_count_;

		if (is_null_) {
			is_null_ = false;
/* may or may not be zero */
			maximum_moves_ = minimum_moves_ = smallest_moves_ = bars_[t].GetNumberMoves();
		} else {
			if (bars_[t].GetNumberMoves() > 0)
			{
/* edge case: smallest-moves should not be zero if a trade-day is available */
				if (0 == maximum_moves_)
					maximum_moves_ = smallest_moves_ = bars_[t].GetNumberMoves();
				else if (bars_[t].GetNumberMoves() < smallest_moves_)
					smallest_moves_ = bars_[t].GetNumberMoves();
				else if (bars_[t].GetNumberMoves() > maximum_moves_)
					maximum_moves_ = bars_[t].GetNumberMoves();
			}
			if (bars_[t].GetNumberMoves() < minimum_moves_)
				minimum_moves_ = bars_[t].GetNumberMoves();
		}
	}

/* finalize */
	if (trading_day_count_ > 0) {
		if (tenday_open_price > 0.0)
			tenday_percentage_change_     = (100.0 * (bars_[0].GetClosePrice() - tenday_open_price))     / tenday_open_price;
		if (fifteenday_open_price > 0.0)
			fifteenday_percentage_change_ = (100.0 * (bars_[0].GetClosePrice() - fifteenday_open_price)) / fifteenday_open_price;
		if (twentyday_open_price > 0.0)
			twentyday_percentage_change_  = (100.0 * (bars_[0].GetClosePrice() - twentyday_open_price))  / twentyday_open_price;
		if (accumulated_volume > 0) {
			average_volume_         = accumulated_volume / bin_decl_.bin_day_count;
			average_nonzero_volume_ = accumulated_volume / trading_day_count_;
		}
	}

	DVLOG(1) << "Calculate() complete,"
		" day_count=" << trading_day_count_ <<
		" acvol=" << accumulated_volume << 
		" avgvol=" << average_volume_ <<
		" avgrvl=" << average_nonzero_volume_ <<	/* average real volume */
		" count=" << total_moves_ <<
		" hicnt=" << maximum_moves_ <<
		" locnt=" << minimum_moves_ <<
		" smcnt=" << smallest_moves_ <<
		" pctchg_10d=" << tenday_percentage_change_ <<
		" pctchg_15d=" << fifteenday_percentage_change_ <<
		" pctchg_20d=" << twentyday_percentage_change_;
	return true;
}

/* eof */