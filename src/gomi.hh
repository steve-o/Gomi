/* A basic Velocity Analytics User-Plugin to exporting a new Tcl command and
 * periodically publishing out to ADH via RFA using RDM/MarketPrice.
 */

#ifndef __GOMI_HH__
#define __GOMI_HH__

#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

/* Boost Date Time */
#include "boost/date_time/local_time/local_time.hpp"

/* Boost Posix Time */
#include "boost/date_time/posix_time/posix_time.hpp"

/* Boost noncopyable base class. */
#include <boost/utility.hpp>

/* Boost threading. */
#include <boost/thread.hpp>

/* RFA 7.2 */
#include <rfa.hh>

/* Velocity Analytics Plugin Framework */
#include <vpf/vpf.h>

/* Microsoft wrappers */
#include "microsoft/timer.hh"

#include "config.hh"
#include "provider.hh"

namespace logging
{
	class LogEventProvider;
}

namespace gomi
{
/* Performance Counters */
	enum {
		GOMI_PC_TCL_QUERY_RECEIVED,
		GOMI_PC_TIMER_QUERY_RECEIVED,
/*		GOMI_PC_LAST_ACTIVITY,*/
/*		GOMI_PC_TCL_SVC_TIME_MIN,*/
/*		GOMI_PC_TCL_SVC_TIME_MEAN,*/
/*		GOMI_PC_TCL_SVC_TIME_MAX,*/
/*		GOMI_PC_TIMER_SVC_TIME_MIN,*/
/*		GOMI_PC_TIMER_SVC_TIME_MEAN,*/
/*		GOMI_PC_TIMER_SVC_TIME_MAX,*/

/* marker */
		GOMI_PC_MAX
	};

	class bin_t;
	class rfa_t;
	class provider_t;
	class snmp_agent_t;

/* Basic state for each item stream. */
	class broadcast_stream_t : public item_stream_t
	{
	public:
		broadcast_stream_t (std::shared_ptr<bin_t> bin_) :
			bin (bin_)
		{
		}

		std::shared_ptr<bin_t> bin;
	};

	struct flex_filter_t
	{
		double bid_price;
		double ask_price;
	};

	class event_pump_t
	{
	public:
		event_pump_t (std::shared_ptr<rfa::common::EventQueue> event_queue) :
			event_queue_ (event_queue)
		{
		}

		void operator()()
		{
			while (event_queue_->isActive()) {
				event_queue_->dispatch (rfa::common::Dispatchable::InfiniteWait);
			}
		}

	private:
		std::shared_ptr<rfa::common::EventQueue> event_queue_;
	};

	class gomi_t :
		public vpf::AbstractUserPlugin,
		public vpf::Command,
		boost::noncopyable
	{
	public:
		gomi_t();
		virtual ~gomi_t();

/* Plugin entry point. */
		virtual void init (const vpf::UserPluginConfig& config_);

/* Reset state suitable for recalling init(). */
		void clear();

/* Plugin termination point. */
		virtual void destroy();

/* Tcl entry point. */
		virtual int execute (const vpf::CommandInfo& cmdInfo, vpf::TCLCommandData& cmdData);

/* Configured period timer entry point. */
		void processTimer (void* closure);

/* Global list of all plugin instances.  AE owns pointer. */
		static std::list<gomi_t*> global_list_;
		static boost::shared_mutex global_list_lock_;

	private:

/* Run core event loop. */
		void mainLoop();

		int tclGomiQuery (const vpf::CommandInfo& cmdInfo, vpf::TCLCommandData& cmdData);
		int tclFeedLogQuery (const vpf::CommandInfo& cmdInfo, vpf::TCLCommandData& cmdData);
		int tclRepublishQuery (const vpf::CommandInfo& cmdInfo, vpf::TCLCommandData& cmdData);

		void get_next_interval (FILETIME& ft);
		void get_end_of_last_interval (__time32_t& t);

/* Broadcast out message. */
		bool sendRefresh() throw (rfa::common::InvalidUsageException);

/* Unique instance number per process. */
		LONG instance_;
		static LONG volatile instance_count_;

/* Plugin Xml identifiers. */
		std::string plugin_id_, plugin_type_;

/* Application configuration. */
		config_t config_;

/* Significant failure has occurred, so ignore all runtime events flag. */
		bool is_shutdown_;

/* SNMP implant. */
		std::unique_ptr<snmp_agent_t> snmp_agent_;
		friend class snmp_agent_t;

#ifdef GOMIMIB_H
		friend Netsnmp_Next_Data_Point gomiPluginTable_get_next_data_point;
		friend Netsnmp_Node_Handler gomiPluginTable_handler;

		friend Netsnmp_Next_Data_Point gomiPluginPerformanceTable_get_next_data_point;
		friend Netsnmp_Node_Handler gomiPluginPerformanceTable_handler;

		friend Netsnmp_First_Data_Point gomiSessionTable_get_first_data_point;
		friend Netsnmp_Next_Data_Point gomiSessionTable_get_next_data_point;

		friend Netsnmp_First_Data_Point gomiSessionPerformanceTable_get_first_data_point;
		friend Netsnmp_Next_Data_Point gomiSessionPerformanceTable_get_next_data_point;
#endif /* GOMIMIB_H */

/* RFA context. */
		std::shared_ptr<rfa_t> rfa_;

/* RFA asynchronous event queue. */
		std::shared_ptr<rfa::common::EventQueue> event_queue_;

/* RFA logging */
		std::shared_ptr<logging::LogEventProvider> log_;

/* RFA provider */
		std::shared_ptr<provider_t> provider_;

/* Timezone database */
		boost::local_time::tz_database tzdb_;

/* Configured time zone */
		boost::local_time::time_zone_ptr TZ_;

/* Default length of analytics */
		unsigned day_count_;

/* Publish instruments. */
		std::vector<std::shared_ptr<bin_t>> query_vector_;
		std::vector<std::shared_ptr<broadcast_stream_t>> stream_vector_;
		boost::shared_mutex query_mutex_;

/* Event pump and thread. */
		std::unique_ptr<event_pump_t> event_pump_;
		std::unique_ptr<boost::thread> thread_;

/* Publish fields. */
		rfa::data::FieldList fields_;

/* Threadpool timer. */
		ms::timer timer_;

/** Performance Counters. **/
		boost::posix_time::ptime last_activity_;
		boost::posix_time::time_duration min_tcl_time_, max_tcl_time_, total_tcl_time_;
		boost::posix_time::time_duration min_refresh_time_, max_refresh_time_, total_refresh_time_;

		uint32_t cumulative_stats_[GOMI_PC_MAX];
		uint32_t snap_stats_[GOMI_PC_MAX];
		boost::posix_time::ptime snap_time_;
	};

} /* namespace gomi */

#endif /* __GOMI_HH__ */

/* eof */
