/* First value accumulator.
 */

#ifndef __ACCUMULATOR_FIRST_HH__
#define __ACCUMULATOR_FIRST_HH__
#pragma once

#include <boost/mpl/always.hpp>
#include <boost/accumulators/framework/accumulator_base.hpp>
#include <boost/accumulators/framework/extractor.hpp>
#include <boost/accumulators/framework/depends_on.hpp>
#include <boost/accumulators/statistics_fwd.hpp>

namespace boost { namespace accumulators
{

namespace impl
{

	template<typename Sample>
	struct first_impl
		: accumulator_base
	{
// for boost::result_of
		typedef Sample result_type;

		template<typename Args>
		first_impl (Args const &args)
			: first (args[sample | Sample()])
			, is_first (true)
		{
			if (first == Sample())
				is_first = true;
		}

		template<typename Args>
		void operator() (Args const &args)
		{
			if (is_first) {
				first = args[sample];
				is_first = false;
			}
		}

		result_type result (dont_care) const
		{
			return this->first;
		}

	private:
		Sample first;
		bool is_first;
	};

} // namespace impl

namespace tag
{
	struct first
		: depends_on<>
	{
		typedef accumulators::impl::first_impl<mpl::_1> impl;
	};
}

namespace extract
{
	extractor<tag::first> const first = {};

	BOOST_ACCUMULATORS_IGNORE_GLOBAL(first)
}

using extract::first;

}} // namespace boost::accumulators

#endif /* __ACCUMULATOR_FIRST_HH__ */

/* eof */
