/* A basic Velocity Analytics function to calculate basic bin analytics.
 */

#ifndef __GET_BIN_HH__
#define __GET_BIN_HH__

#pragma once

#include <cstdint>
#include <list>
#include <unordered_map>
#include <string>
#include <vector>

/* Boost noncopyable base class. */
#include <boost/utility.hpp>

/* Boost Date Time */
#include "boost/date_time/local_time/local_time.hpp"

/* Boost Posix Time */
#include "boost/date_time/posix_time/posix_time.hpp"

#include <TBPrimitives.h>

namespace gomi
{
/* definition of a /bin/ */
	struct bin_t
	{
		std::string bin_name;
		boost::posix_time::time_duration bin_start, bin_end;
		boost::local_time::time_zone_ptr bin_tz;
		unsigned bin_day_count;
	};

/* sorted by close time */
	struct bin_close_compare_t
	{
		bool operator() (const bin_t& lhs, const bin_t& rhs) const {
			return lhs.bin_end < rhs.bin_end;
		}
	};

	struct bin_openclose_compare_t
	{
		bool operator() (const bin_t& lhs, const bin_t& rhs) const {
			if (lhs.bin_end < rhs.bin_end)
				return true;
			return (lhs.bin_end == rhs.bin_end) && ((lhs.bin_end - lhs.bin_start) < (rhs.bin_end - rhs.bin_start));
		}
	};

/* FlexRecord Primitives implementation state */
	class analytic_state_t {
	public:
		analytic_state_t() :
			is_null (true)
		{
		}

/* returns true if requested time window has already been calculated */
		bool open (__time32_t close_time_)
		{
			if (!is_null && close_time == close_time_)
				return true;
			close_time = close_time_;
			open_price = close_price = 0.0;
			accumulated_volume = num_moves = 0;
			is_null = false;
			return false;
		}

		__time32_t	close_time;

		double		open_price;
		uint64_t	accumulated_volume;
		uint64_t	num_moves;
		double		close_price;

		bool		is_null;
	};

/* result of analytics applied to a /bin/ */
	class janku_t : boost::noncopyable
	{
	public:
		janku_t (const char* symbol_name_, const char* last_price_field_, const char* tick_volume_field_) :
			symbol_name (symbol_name_),
			last_price_field (last_price_field_),
			tick_volume_field (tick_volume_field_)
		{
			clear();
			handle = TBPrimitives::GetSymbolHandle (symbol_name.c_str(), 1);
		}

		void clear() {
			tenday_percentage_change = fifteenday_percentage_change = twentyday_percentage_change = 0.0;
			average_volume = average_nonzero_volume = total_moves = maximum_moves = minimum_moves = smallest_moves = 0;
			close_time = boost::posix_time::not_a_date_time;
			trading_day_count = 0;
			is_null = true;
		}

/* Vhayu symbol name */
		std::string	symbol_name;
/* TBPrimitives handle */
		TBSymbolHandle	handle;
/* analytic state */
		std::list<analytic_state_t> analytic_state;
/* Vhayu field names */
		std::string	last_price_field,
				tick_volume_field;

/* analytic results */
		double		tenday_percentage_change,
				fifteenday_percentage_change,
				twentyday_percentage_change;
		uint64_t	average_volume, average_nonzero_volume;
		uint64_t	total_moves;
		uint64_t	maximum_moves;
		uint64_t	minimum_moves, smallest_moves;

/* end or close time or first effective business day of analytic, in UTC. */
		boost::posix_time::ptime close_time;
/* count of days with trades within bin parameters */
		unsigned	trading_day_count;

		bool		is_null;
	};

/* for a given /bin/ calculate analytics for the set of symbols */
	namespace reference {
		void get_bin (const bin_t& bin, std::vector<std::shared_ptr<janku_t>>& query);
	}
	namespace primitive {
		void get_bin (const bin_t& bin, std::vector<std::shared_ptr<janku_t>>& query, FlexRecWorkAreaElement* work_area, FlexRecViewElement* view_element);
	}
	namespace single_iterator {
		void get_bin (const bin_t& bin, std::vector<std::shared_ptr<janku_t>>& query);
	}

} /* namespace gomi */

#endif /* __GET_BIN_HH__ */

/* eof */