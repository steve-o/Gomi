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
#include "get_bin.hh"

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

	class rfa_t;
	class provider_t;
	class snmp_agent_t;

/* Archive streams match a specific bin analytic query. */
	class archive_stream_t : public item_stream_t
	{
	public:
		archive_stream_t (std::shared_ptr<janku_t> janku_) :
			janku (janku_)
		{
		}

		std::shared_ptr<janku_t> janku;
	};

/* Realtime streams are a set of multiple archive streams. */
	class realtime_stream_t : public item_stream_t
	{
	public:
		realtime_stream_t (const std::string& symbol_name_) :
			symbol_name (symbol_name_)
		{
		}

/* source feed name, not the name of the derived feed symbol */
		std::string symbol_name;
		std::vector<std::pair<fidset_t, std::shared_ptr<archive_stream_t>>> special;
/* last 10-minute bin requires custom handling */
		std::pair<fidset_t, std::map<bin_t, std::shared_ptr<archive_stream_t>, bin_openclose_compare_t>> last_10min;
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
		int tclRepublishLastBinQuery (const vpf::CommandInfo& cmdInfo, vpf::TCLCommandData& cmdData);
		int tclRecalculateQuery (const vpf::CommandInfo& cmdInfo, vpf::TCLCommandData& cmdData);

		bool is_special_bin (const bin_t& bin);

		bool get_next_bin_close (boost::local_time::time_zone_ptr tz, FILETIME& ft);
		bool get_last_bin_close (boost::local_time::time_zone_ptr tz, boost::posix_time::time_duration* last_close);

/* Broadcast out messages. */
		bool timeRefresh() throw (rfa::common::InvalidUsageException);
		bool dayRefresh() throw (rfa::common::InvalidUsageException);
		bool Recalculate() throw (rfa::common::InvalidUsageException);
		bool binRefresh (const bin_t& bin) throw (rfa::common::InvalidUsageException);
		bool summaryRefresh() throw (rfa::common::InvalidUsageException);

/* Unique instance number per process. */
		LONG instance_;
		static LONG volatile instance_count_;

/* Plugin Xml identifiers. */
		std::string plugin_id_, plugin_type_;

/* Application configuration. */
		config_t config_;

/* Significant failure has occurred, so ignore all runtime events flag. */
		bool is_shutdown_;

/* FLexRecord cursor */
		FlexRecDefinitionManager* manager_;
		std::shared_ptr<FlexRecWorkAreaElement> work_area_;
		std::shared_ptr<FlexRecViewElement> view_element_;

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

/* Parsed bin decls sorted by close time, not by open-close. */
		std::set<bin_t, bin_close_compare_t> bins_;

/* last refresh time-of-day, default to not_a_date_time */
		boost::posix_time::time_duration last_refresh_;

/* Publish instruments. */
		std::map<bin_t, std::pair<std::vector<std::shared_ptr<janku_t>>,
					  std::vector<std::shared_ptr<archive_stream_t>>
					 >, bin_openclose_compare_t> query_vector_;
		boost::shared_mutex query_mutex_;
		std::vector<std::shared_ptr<realtime_stream_t>> stream_vector_;

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
