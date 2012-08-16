/* A basic Velocity Analytics User-Plugin to exporting a new Tcl command and
 * periodically publishing out to ADH via RFA using RDM/MarketPrice.
 */

#ifndef __GOMI_HH__
#define __GOMI_HH__
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <tuple>

/* Boost Chrono. */
#include <boost/chrono.hpp>

/* Boost Date Time */
#include <boost/date_time/local_time/local_time.hpp>

/* Boost Posix Time */
#include <boost/date_time/posix_time/posix_time.hpp>

/* Boost instrusive containers */
#include <boost/intrusive/circular_list_algorithms.hpp>

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
		archive_stream_t (std::shared_ptr<bin_t> bin_) :
			bin (bin_)
		{
		}

		std::shared_ptr<bin_t> bin;
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
		std::pair<fidset_t, std::map<bin_decl_t, std::shared_ptr<archive_stream_t>, bin_decl_openclose_compare_t>> last_10min;
	};

	class event_pump_t
	{
	public:
		event_pump_t (std::shared_ptr<rfa::common::EventQueue> event_queue) :
			event_queue_ (event_queue)
		{
		}

		void Run (void)
		{
			while (event_queue_->isActive()) {
				event_queue_->dispatch (rfa::common::Dispatchable::InfiniteWait);
			}
		}

	private:
		std::shared_ptr<rfa::common::EventQueue> event_queue_;
	};

/* Periodic timer event source */
	template<class Clock, class Duration = typename Clock::duration>
	class time_base_t
	{
	public:
		virtual bool OnTimer (const boost::chrono::time_point<Clock, Duration>& t) = 0;
	};

	template<class Clock, class Duration = typename Clock::duration>
	class time_pump_t
	{
	public:
		time_pump_t (const boost::chrono::time_point<Clock, Duration>& due_time, Duration td, time_base_t<Clock, Duration>* cb) :
			due_time_ (due_time),
			td_ (td),
			cb_ (cb)
		{
			CHECK(nullptr != cb_);
		}

		void Run (void)
		{
			try {
				while (true) {
					boost::this_thread::sleep_until (due_time_);
					if (!cb_->OnTimer (due_time_))
						break;
					due_time_ += td_;
				}
			} catch (boost::thread_interrupted const&) {
				LOG(INFO) << "Timer thread interrupted.";
			}
		}

	private:
		boost::chrono::time_point<Clock, Duration> due_time_;
		Duration td_;
		time_base_t<Clock, Duration>* cb_;
	};

	class gomi_t :
		public time_base_t<boost::chrono::system_clock>,
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

/* Configured period timer entry point. */
		bool OnTimer (const boost::chrono::time_point<boost::chrono::system_clock>& t) override;

/* Global list of all plugin instances.  AE owns pointer. */
		static std::list<gomi_t*> global_list_;
		static boost::shared_mutex global_list_lock_;

	private:

/* Run core event loop. */
		void MainLoop();

		bool RegisterTclApi (const char* id);
		bool UnregisterTclApi (const char* id);
		int TclGomiQuery (const vpf::CommandInfo& cmdInfo, vpf::TCLCommandData& cmdData);
		int TclFeedLogQuery (const vpf::CommandInfo& cmdInfo, vpf::TCLCommandData& cmdData);
		int TclRepublishQuery (const vpf::CommandInfo& cmdInfo, vpf::TCLCommandData& cmdData);
		int TclRepublishLastBinQuery (const vpf::CommandInfo& cmdInfo, vpf::TCLCommandData& cmdData);
		int TclRecalculateQuery (const vpf::CommandInfo& cmdInfo, vpf::TCLCommandData& cmdData);

		bool IsSpecialBin (const bin_decl_t& bin);

		bool GetDueTime (const boost::local_time::time_zone_ptr& tz, boost::posix_time::ptime* t);
		bool GetNextBinClose (const boost::local_time::time_zone_ptr& tz, boost::posix_time::ptime* t);
		bool GetLastBinClose (const boost::local_time::time_zone_ptr& tz, boost::posix_time::time_duration* last_close);

/* Broadcast out messages. */
		bool TimeRefresh() throw (rfa::common::InvalidUsageException);
		bool DayRefresh() throw (rfa::common::InvalidUsageException);
		bool Recalculate() throw (rfa::common::InvalidUsageException);
		bool BinRefresh (const bin_decl_t& bin) throw (rfa::common::InvalidUsageException);
		bool SummaryRefresh (const boost::posix_time::time_duration& time_of_day) throw (rfa::common::InvalidUsageException);

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
		std::set<bin_decl_t, bin_decl_close_compare_t> bins_;

/* last refresh time-of-day, default to not_a_date_time */
		boost::posix_time::time_duration last_refresh_;

/* Publish instruments. */
		std::map<bin_decl_t, std::pair<std::vector<std::shared_ptr<bin_t>>,
					       std::vector<std::shared_ptr<archive_stream_t>>>, bin_decl_openclose_compare_t> query_vector_;
		boost::shared_mutex query_mutex_;
		std::vector<std::shared_ptr<realtime_stream_t>> stream_vector_;

/* analytic state */

/* Event pump and thread. */
		std::unique_ptr<event_pump_t> event_pump_;
		std::unique_ptr<boost::thread> event_thread_;

/* Publish fields. */
		rfa::data::FieldList fields_;

/* Iterator for populating publish fields */
		rfa::data::SingleWriteIterator single_write_it_;

/* Thread timer. */
		std::unique_ptr<time_pump_t<boost::chrono::system_clock>> timer_;
		std::unique_ptr<boost::thread> timer_thread_;

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
