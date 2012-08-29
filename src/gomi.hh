/* A basic Velocity Analytics User-Plugin to exporting a new Tcl command and
 * periodically publishing out to ADH via RFA using RDM/MarketPrice.
 */

#ifndef __GOMI_HH__
#define __GOMI_HH__
#pragma once

#include <cstdint>
#include <forward_list>
#include <memory>
#include <unordered_map>

/* Boost Chrono. */
#include <boost/chrono.hpp>

/* Boost Date Time */
#include <boost/date_time/local_time/local_time.hpp>

/* Boost Posix Time */
#include <boost/date_time/posix_time/posix_time.hpp>

/* Boost unordered map: bypass 2^19 limit in MSVC std::unordered_map */
#include <boost/unordered_map.hpp>

/* Boost noncopyable base class. */
#include <boost/utility.hpp>

/* Boost threading. */
#include <boost/thread.hpp>

/* RFA 7.2 */
#include <rfa/rfa.hh>

/* Velocity Analytics Plugin Framework */
#include <vpf/vpf.h>

#include "chromium/logging.hh"

#include "config.hh"
#include "provider.hh"
#include "gomi_bin.hh"

namespace chromium
{
	class StatisticsRecorder;
	class StatsTable;
}

namespace logging
{
	class LogEventProvider;
}

namespace portware
{
	double round (double x);
}

namespace gomi
{
/* Performance Counters */
	enum {
		GOMI_PC_TCL_QUERY_RECEIVED,
		GOMI_PC_TIMER_QUERY_RECEIVED,		/* unused: legacy metric */
/*		GOMI_PC_LAST_ACTIVITY,*/
/*		GOMI_PC_TCL_SVC_TIME_MIN,*/
/*		GOMI_PC_TCL_SVC_TIME_MEAN,*/
/*		GOMI_PC_TCL_SVC_TIME_MAX,*/

/* marker */
		GOMI_PC_MAX
	};

	class rfa_t;
	class provider_t;
	class worker_t;
	class snmp_agent_t;

	class event_pump_t
	{
	public:
		event_pump_t (
			std::shared_ptr<void> zmq_context,
			std::shared_ptr<void> response_sock,
			std::shared_ptr<provider_t> provider,
			std::shared_ptr<rfa::common::EventQueue> event_queue
			) :
			zmq_context_ (zmq_context),
			response_sock_ (response_sock),
			provider_ (provider),
			event_queue_ (event_queue)
		{
		}
		~event_pump_t()
		{
			Clear();
			LOG(INFO) << "Event pump thread closed.";
		}

		bool Init();
		void Clear();
		void Run();

	protected:
		std::shared_ptr<void> zmq_context_;
		zmq_pollitem_t poll_items_[3];
		std::shared_ptr<void> abort_sock_;
		std::shared_ptr<void> event_sock_;
		std::shared_ptr<void> response_sock_;

		std::shared_ptr<provider_t> provider_;
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
		virtual void init (const vpf::UserPluginConfig& config_) override;

/* Core initialization. */
		bool Init();

/* Reset state suitable for recalling init(). */
		void Clear();

/* Plugin termination point. */
		virtual void destroy() override;

/* Tcl entry point. */
		virtual int execute (const vpf::CommandInfo& cmdInfo, vpf::TCLCommandData& cmdData) override;

/* Global list of all plugin instances.  AE owns pointer. */
		static std::list<gomi_t*> global_list_;
		static boost::shared_mutex global_list_lock_;

	protected:

/* Run core event loop. */
		void MainLoop();

		bool RegisterTclApi (const char* id);
		bool UnregisterTclApi (const char* id);
		int TclGomiQuery (const vpf::CommandInfo& cmdInfo, vpf::TCLCommandData& cmdData);
		int TclFeedLogQuery (const vpf::CommandInfo& cmdInfo, vpf::TCLCommandData& cmdData);
		int TclHistogramDump (const vpf::CommandInfo& cmdInfo, vpf::TCLCommandData& cmdData);

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

		friend Netsnmp_First_Data_Point gomiClientTable_get_first_data_point;
		friend Netsnmp_Next_Data_Point gomiClientTable_get_next_data_point;

		friend Netsnmp_First_Data_Point gomiClientPerformanceTable_get_first_data_point;
		friend Netsnmp_Next_Data_Point gomiClientPerformanceTable_get_next_data_point;
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

/* Event pump and thread. */
		std::unique_ptr<event_pump_t> event_pump_;
		std::unique_ptr<boost::thread> event_thread_;

/* RFA request thread workers. */
		std::forward_list<std::pair<std::shared_ptr<worker_t>, std::shared_ptr<boost::thread>>> workers_;

/* thread worker shutdown socket. */
		std::shared_ptr<void> zmq_context_;
		std::shared_ptr<void> event_pump_abort_sock_;
		std::shared_ptr<void> worker_abort_sock_;

/* Response socket. */
		std::shared_ptr<void> response_sock_;

/** Performance Counters. **/
		boost::posix_time::ptime last_activity_;
		boost::posix_time::time_duration min_tcl_time_, max_tcl_time_, total_tcl_time_;
		boost::posix_time::time_duration min_refresh_time_, max_refresh_time_, total_refresh_time_;

		uint32_t cumulative_stats_[GOMI_PC_MAX];
		uint32_t snap_stats_[GOMI_PC_MAX];
		boost::posix_time::ptime snap_time_;

		std::unique_ptr<chromium::StatisticsRecorder> recorder_;
		chromium::StatsTable* statstable_;
	};

} /* namespace gomi */

#endif /* __GOMI_HH__ */

/* eof */