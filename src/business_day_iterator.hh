/* business day iterator, sourced by Vhayu SDK.
 *
 * Extremely large caveat: SDK is limited to a single market.
 */

#ifndef __BUSINESS_DAY_ITERATOR_HH__
#define __BUSINESS_DAY_ITERATOR_HH__

#pragma once

#include <boost/date_time/date.hpp>
#include <boost/date_time/date_iterator.hpp>
#include "boost/date_time/posix_time/posix_time.hpp"

/* Velocity Analytics Plugin Framework */
#include <vpf/vpf.h>

namespace vhayu {

/* functor to iterate a fixed number of business days */
	template<class date_type>
	class business_day_functor
	{
	public:
		typedef typename date_type::duration_type duration_type;
		business_day_functor (int f) : f_ (f) {}
		duration_type get_offset (const date_type& d) const
		{
			using namespace boost::posix_time;
			__time32_t time32 = (ptime (d) - ptime (date_type (1970, 1, 1))).total_seconds();
			int day_count = f_;
			BusinessDayInfo bd;
			for (int i = 0; i < f_; ++i)
			{
				time32 += 86400;
				while (0 == TBPrimitives::BusinessDay (time32, &bd))
				{
					++day_count;
					time32 += 86400;
				}
			}
			return duration_type (day_count);
		}
		duration_type get_neg_offset (const date_type& d) const
		{
			using namespace boost::posix_time;
			__time32_t time32 = (ptime (d) - ptime (date_type (1970, 1, 1))).total_seconds();
			int day_count = f_;
			BusinessDayInfo bd;
			for (int i = 0; i < f_; ++i)
			{
				time32 -= 86400;
				while (0 == TBPrimitives::BusinessDay (time32, &bd))
				{
					++day_count;
					time32 -= 86400;
				}
			}
			return duration_type (-day_count);
		}
	private:
		int f_;		
	};

/* a business day level iterator */
	typedef boost::date_time::date_itr<business_day_functor<boost::gregorian::date>,
				           boost::gregorian::date> business_day_iterator;

} /* namespace vhayu */

#endif /* __BUSINESS_DAY_ITERATOR_HH__ */

/* eof */
