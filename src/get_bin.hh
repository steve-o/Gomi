/* A basic Velocity Analytics function to calculate basic bin analytics.
 */

#ifndef __GET_BIN_HH__
#define __GET_BIN_HH__

#pragma once

#include <cstdint>
#include <unordered_map>
#include <string>
#include <vector>

/* Boost noncopyable base class. */
#include <boost/utility.hpp>

/* Boost Date Time */
#include "boost/date_time/local_time/local_time.hpp"

/* Boost Posix Time */
#include "boost/date_time/posix_time/posix_time.hpp"

namespace gomi
{
	class bin_t : boost::noncopyable
	{
	public:
		bin_t (const char* symbol_name_, const char* last_price_field_, const char* tick_volume_field_) :
			symbol_name (symbol_name_),
			last_price_field (last_price_field_),
			tick_volume_field (tick_volume_field_)
		{
			clear();
		}

		void clear() {
			tenday_percentage_change = fifteenday_percentage_change = twentyday_percentage_change = 0.0;
			average_volume = average_nonzero_volume = total_moves = maximum_moves = minimum_moves = smallest_moves = 0;
			is_null = true;
		}

		std::string	symbol_name;
		std::string	last_price_field,
				tick_volume_field;

		double		tenday_percentage_change,
				fifteenday_percentage_change,
				twentyday_percentage_change;
		uint64_t	average_volume, average_nonzero_volume;
		uint64_t	total_moves;
		uint64_t	maximum_moves;
		uint64_t	minimum_moves, smallest_moves;

		bool		is_null;
	};

	void get_bin (std::vector<std::shared_ptr<bin_t>>& bin,
		const boost::posix_time::time_duration& start,
		const boost::posix_time::time_duration& end,
		const unsigned days,
		const boost::local_time::time_zone_ptr& tz);

} /* namespace gomi */

#endif /* __GET_BIN_HH__ */

/* eof */