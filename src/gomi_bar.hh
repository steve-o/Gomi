/* A basic Velocity Analytics function to calculate basic bar analytics.
 */

#ifndef __GOMI_BAR_HH__
#define __GOMI_BAR_HH__
#pragma once

#include <cstdint>

/* Boost noncopyable base class. */
#include <boost/utility.hpp>

/* Boost Posix Time */
#include <boost/date_time/posix_time/posix_time.hpp>

/* Boost Accumulators */
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/count.hpp>
#include <boost/accumulators/statistics/sum.hpp>

/* Velocity Analytics Plugin Framework */
#include <vpf/vpf.h>
#include <TBPrimitives.h>

#include "accumulators/first.hh"
#include "accumulators/last.hh"

namespace gomi
{
/* definition of a gummy bar */
	class bar_t
	{
	public:
		bar_t() :
			tp_ (boost::posix_time::not_a_date_time, boost::posix_time::hours (0)),
			is_null_ (true)
		{
		}

		bar_t (const boost::posix_time::time_period& tp) :
			tp_ (tp),
			is_null_ (true)
		{
		}

		bool Calculate (const char* symbol_name);
		bool Calculate (const TBSymbolHandle& handle, FlexRecWorkAreaElement* work_area, FlexRecViewElement* view_element);

		void SetTimePeriod (const boost::posix_time::time_period tp) { tp_ = tp; }
		double GetOpenPrice() const { return boost::accumulators::first (last_price_); }
		double GetClosePrice() const { return boost::accumulators::last (last_price_); }
		uint64_t GetNumberMoves() const { return boost::accumulators::count (last_price_); }
		uint64_t GetAccumulatedVolume() const { return boost::accumulators::sum (tick_volume_); }

		static int processFlexRecord (FRTreeCallbackInfo* info);

		void Clear()
		{
			static const boost::accumulators::accumulator_set<double,
				boost::accumulators::features<boost::accumulators::tag::first,
							      boost::accumulators::tag::last,
							      boost::accumulators::tag::count>> null_last_price_;
			static const boost::accumulators::accumulator_set<uint64_t,
				boost::accumulators::features<boost::accumulators::tag::sum>> null_tick_volume_;
			last_price_ = null_last_price_;
			tick_volume_ = null_tick_volume_;
			is_null_ = true;
		}

		operator bool() const { return !is_null_; }

	private:
		friend struct bar_compare_t;

		boost::posix_time::time_period tp_;
		boost::accumulators::accumulator_set<double,
			boost::accumulators::features<boost::accumulators::tag::first,
						      boost::accumulators::tag::last,
						      boost::accumulators::tag::count>> last_price_;
		boost::accumulators::accumulator_set<uint64_t,
			boost::accumulators::features<boost::accumulators::tag::sum>> tick_volume_;
		bool is_null_;
	};

	struct bar_compare_t
	{
		bool operator() (const bar_t& lhs, const bar_t& rhs) const {
			return lhs.tp_ < rhs.tp_;
		}
	};

} /* namespace gomi */

#endif /* __GOMI_BAR_HH__ */

/* eof */