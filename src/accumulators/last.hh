/* Last value accumulator.
 */

#ifndef __ACCUMULATORS_LAST_HH__
#define __ACCUMULATORS_LAST_HH__
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
	struct last_impl
		: accumulator_base
	{
// for boost::result_of
		typedef Sample result_type;

		template<typename Args>
		last_impl (Args const &args)
			: last (args[sample | Sample()])
		{}

		template<typename Args>
		void operator() (Args const &args)
		{
			last = args[sample];
		}

		result_type result (dont_care) const
		{
			return this->last;
		}

	private:
		Sample last;
	};

} // namespace impl

namespace tag
{
	struct last
		: depends_on<>
	{
		typedef accumulators::impl::last_impl<mpl::_1> impl;
	};
}

namespace extract
{
	extractor<tag::last> const last = {};

	BOOST_ACCUMULATORS_IGNORE_GLOBAL(last)
}

using extract::last;

}} // namespace boost::accumulators

#endif /* __ACCUMULATORS_LAST_HH__ */

/* eof */
