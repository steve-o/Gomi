/* RFA context.
 */

#ifndef __RFA_HH__
#define __RFA_HH__

#pragma once

#include <memory>

/* Boost noncopyable base class */
#include <boost/utility.hpp>

/* RFA 7.2 */
#include <rfa/rfa.hh>

#include "config.hh"
#include "deleter.hh"

namespace gomi
{

	class rfa_t :
		boost::noncopyable
	{
	public:
		rfa_t (const config_t& config);
		~rfa_t();

		bool Init() throw (rfa::common::InvalidUsageException);

	private:

		const config_t& config_;		

/* Live config database */
		std::unique_ptr<rfa::config::ConfigDatabase, internal::release_deleter> rfa_config_;
	};

} /* namespace gomi */

#endif /* __RFA_HH__ */

/* eof */
