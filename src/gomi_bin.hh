/* A basic Velocity Analytics function to calculate basic bin analytics.
 */

#ifndef __GOMI_BIN_HH__
#define __GOMI_BIN_HH__
#pragma once

#include <cstdint>
#include <list>
#include <unordered_map>
#include <string>
#include <vector>

/* Boost noncopyable base class. */
#include <boost/utility.hpp>

/* Boost Date Time */
#include <boost/date_time/local_time/local_time.hpp>

/* Boost Posix Time */
#include <boost/date_time/posix_time/posix_time.hpp>

/* Boost Gregorian Calendar */
#include <boost/date_time/gregorian/gregorian_types.hpp>

#include <TBPrimitives.h>

#include "gomi_bar.hh"

namespace gomi
{
/* definition of a /bin/ */
	struct bin_decl_t
	{
		std::string bin_name;
		boost::posix_time::time_duration bin_start, bin_end;
		boost::local_time::time_zone_ptr bin_tz;
		unsigned bin_day_count;
	};

	inline
	std::ostream& operator<< (std::ostream& o, const bin_decl_t& bin_decl) {
		o << "{ "
			  "name: \"" << bin_decl.bin_name << "\""
			", start: \"" << boost::posix_time::to_simple_string (bin_decl.bin_start) << "\""
			", end: \"" << boost::posix_time::to_simple_string (bin_decl.bin_end) << "\""
			", tz: \"" << bin_decl.bin_tz->std_zone_abbrev() << "\""
			", day_count: " << bin_decl.bin_day_count <<
			" }";
		return o;
	}

/* sorted by close time */
	struct bin_decl_close_compare_t
	{
		bool operator() (const bin_decl_t& lhs, const bin_decl_t& rhs) const {
			return lhs.bin_end < rhs.bin_end;
		}
	};

	struct bin_decl_openclose_compare_t
	{
		bool operator() (const bin_decl_t& lhs, const bin_decl_t& rhs) const {
			if (lhs.bin_end < rhs.bin_end)
				return true;
			return (lhs.bin_end == rhs.bin_end) && (lhs.bin_start < rhs.bin_start);
		}
	};

/* result of analytics applied to a /bin/ */
	class bin_t : boost::noncopyable
	{
	public:
		bin_t (const bin_decl_t& bin_decl, const char* symbol_name, const char* last_price_field, const char* tick_volume_field) :
			bin_decl_ (bin_decl),
			symbol_name_ (symbol_name),
			last_price_field_ (last_price_field),
			tick_volume_field_ (tick_volume_field),
			bars_ (bin_decl_.bin_day_count)
		{
			Clear();
			handle_ = TBPrimitives::GetSymbolHandle (symbol_name_.c_str(), 1);
		}

		void Clear() {
			tenday_percentage_change_ = fifteenday_percentage_change_ = twentyday_percentage_change_ = 0.0;
			average_volume_ = average_nonzero_volume_ = total_moves_ = maximum_moves_ = minimum_moves_ = smallest_moves_ = 0;
			close_time_ = boost::posix_time::not_a_date_time;
			trading_day_count_ = 0;
			is_null_ = true;
		}

/* calculate this bin for a given date /date/ */
		bool Calculate (const boost::gregorian::date& date, FlexRecWorkAreaElement* work_area, FlexRecViewElement* view_element);

		const char* GetSymbolName() { return symbol_name_.c_str(); }
		const double GetTenDayPercentageChange() { return tenday_percentage_change_; }
		const double GetFifteenDayPercentageChange() { return fifteenday_percentage_change_; }
		const double GetTwentyDayPercentageChange() { return twentyday_percentage_change_; }
		const uint64_t GetAverageVolume() { return average_volume_; }
		const uint64_t GetAverageNonZeroVolume() { return average_nonzero_volume_; }
		const uint64_t GetTotalMoves() { return total_moves_; }
		const uint64_t GetMaximumMoves() { return maximum_moves_; }
		const uint64_t GetMinimumMoves() { return minimum_moves_; }
		const uint64_t GetSmallestMoves() { return smallest_moves_; }

		const boost::posix_time::ptime GetCloseTime() { return close_time_; }

		operator bool() const { return !is_null_; }

	private:
		const bin_decl_t&	bin_decl_;
/* Vhayu symbol name */
		const std::string	symbol_name_;
/* TBPrimitives handle */
		TBSymbolHandle		handle_;
/* Vhayu field names */
		const std::string	last_price_field_,
					tick_volume_field_;
/* analytic state */
		std::vector<bar_t>	bars_;
/* analytic results */
		double			tenday_percentage_change_,
					fifteenday_percentage_change_,
					twentyday_percentage_change_;
		uint64_t		average_volume_, average_nonzero_volume_;
		uint64_t		total_moves_;
		uint64_t		maximum_moves_;
		uint64_t		minimum_moves_, smallest_moves_;

/* end or close time or last effective business day of analytic, in UTC. */
		boost::posix_time::ptime close_time_;
/* count of days with trades within bin parameters */
		unsigned		trading_day_count_;

		bool			is_null_;
	};

} /* namespace gomi */

#endif /* __GOMI_BIN_HH__ */

/* eof */
