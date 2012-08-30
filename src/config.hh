/* User-configurable settings.
 *
 * NB: all strings are locale bound, RFA provides no Unicode support.
 */

#ifndef __CONFIG_HH__
#define __CONFIG_HH__

#pragma once

#include <string>
#include <vector>

/* Velocity Analytics Plugin Framework */
#include <vpf/vpf.h>

namespace gomi
{
	struct session_config_t
	{
//  RFA session name, one session contains a horizontal scaling set of connections.
		std::string session_name;

//  RFA connection name, used for logging.
		std::string connection_name;

//  RFA publisher name, used for logging.
		std::string publisher_name;

//  Default TREP-RT RSSL port, e.g. 14002 (interactive), 14003 (non-interactive).
		std::string rssl_port;

//  Client session capacity.
		unsigned session_capacity;
	};

	struct client_config_t
	{
//  RFA login user name.
		std::string name;
	};

	struct fidset_t
	{
/* VMA_20D: Volume moving average. */
		int	RdmAverageVolumeId;
/* VMA_20TD: Volume moving average for non-zero trading days, i.e. no halts. */
		int	RdmAverageNonZeroVolumeId;
/* TRDCNT_20D: Trade count */
		int	RdmTotalMovesId;
/* HICNT_20D: Highest days trade count */
		int	RdmMaximumMovesId;
/* LOCNT_20D: Lowest days trade count */
		int	RdmMinimumMovesId;
/* SMCNT_20D: Smallest days trade count */
		int	RdmSmallestMovesId;
/* PCTCHG_10D: 10-day percentage change in price */
		int	Rdm10DayPercentChangeId;
/* PCTCHG_15D: 15-day percentage change in price */
		int	Rdm15DayPercentChangeId;
/* PCTCHG_20D: 20-day percentage change in price */
		int	Rdm20DayPercentChangeId;
/* PCTCHG_10TD: 10-trading-day percentage change in price */
		int	Rdm10TradingDayPercentChangeId;
/* PCTCHG_15TD: 15-trading-day percentage change in price */
		int	Rdm15TradingDayPercentChangeId;
/* PCTCHG_20TD: 20-trading-day percentage change in price */
		int	Rdm20TradingDayPercentChangeId;
	};

	struct config_t
	{
		config_t();

		bool ParseDomElement (const xercesc::DOMElement* elem);
		bool ParseConfigNode (const xercesc::DOMNode* node);
		bool ParseSnmpNode (const xercesc::DOMNode* node);
		bool ParseAgentXNode (const xercesc::DOMNode* node);
		bool ParseRfaNode (const xercesc::DOMNode* node);
		bool ParseServiceNode (const xercesc::DOMNode* node);
		bool ParseConnectionNode (const xercesc::DOMNode* node, session_config_t*const session);
		bool ParseClientNode (const xercesc::DOMNode* node, client_config_t*const client);
		bool ParsePublisherNode (const xercesc::DOMNode* node, std::string*const publisher);
		bool ParseSessionNode (const xercesc::DOMNode* node);
		bool ParseMonitorNode (const xercesc::DOMNode* node);
		bool ParseEventQueueNode (const xercesc::DOMNode* node);
		bool ParseVendorNode (const xercesc::DOMNode* node);
		bool ParseGomiNode (const xercesc::DOMNode* node);
		bool ParseFieldsNode (const xercesc::DOMNode* node);
		bool ParseArchiveNode (const xercesc::DOMNode* node);
		bool ParseFidNode (const xercesc::DOMNode* node, fidset_t*const fidset);

		bool Validate();

//  SNMP implant.
		bool is_snmp_enabled;

//  Net-SNMP agent or sub-agent.
		bool is_agentx_subagent;

//  Net-SNMP file log target.
		std::string snmp_filelog;

//  AgentX port number to connect to master agent.
		std::string agentx_socket;

//  Windows registry key path.
		std::string key;

//  TREP-RT service name, e.g. IDN_RDF.
		std::string service_name;

//  RFA sessions comprising of session names, connection names,
//  RSSL hostname or IP address and default RSSL port, e.g. 14002, 14003.
		std::vector<session_config_t> sessions;

//  Maximum number of historical outage events.
		unsigned history_table_size;

//  Reserved client slots for outage recording.
		std::vector<client_config_t> clients;

//  RFA application logger monitor name.
		std::string monitor_name;

//  RFA event queue name.
		std::string event_queue_name;

//  RFA vendor name.
		std::string vendor_name;

//  RFA maximum data buffer size for SingleWriteIterator.
		size_t maximum_data_size;

//  Count of request worker threads.
		unsigned worker_count;

//  RFA symbol name suffix for every publish.
		std::string suffix;

//  File path for time zone database that Boost::DateTimes likes.
		std::string tzdb;

//  Default time zone.
		std::string tz;

//  Default analytic time period
		unsigned day_count;

//  FIDs for archival records.
		fidset_t archive_fids;
	};

	inline
	std::ostream& operator<< (std::ostream& o, const session_config_t& session) {
		o << "{ "
			  "\"session_name\": \"" << session.session_name << "\""
			", \"connection_name\": \"" << session.connection_name << "\""
			", \"publisher_name\": \"" << session.publisher_name << "\""
			", \"rssl_port\": \"" << session.rssl_port << "\""
			", \"session_capacity\": " << session.session_capacity << 
			" }";
		return o;
	}

	inline
	std::ostream& operator<< (std::ostream& o, const client_config_t& client) {
		o << "{ "
			  "\"name\": \"" << client.name << "\""
			" }";
		return o;
	}

	inline
	std::ostream& operator<< (std::ostream& o, const fidset_t& fidset) {
		o << "{ "
			  "\"VMA\": " << fidset.RdmAverageVolumeId <<
			", \"NZERO_VMA\": " <<  fidset.RdmAverageNonZeroVolumeId <<
			", \"NUM_MOVES\": " << fidset.RdmTotalMovesId <<
			", \"NM_HIGH\": " << fidset.RdmMaximumMovesId <<
			", \"NM_LOW\": " << fidset.RdmMinimumMovesId <<
			", \"NM_SMALL\": " << fidset.RdmSmallestMovesId <<
			", \"PCTCHG_10D\": " << fidset.Rdm10DayPercentChangeId <<
			", \"PCTCHG_15D\": " << fidset.Rdm15DayPercentChangeId <<
			", \"PCTCHG_20D\": " << fidset.Rdm20DayPercentChangeId <<
			", \"PCTCHG_10T\": " << fidset.Rdm10TradingDayPercentChangeId <<
			", \"PCTCHG_15T\": " << fidset.Rdm15TradingDayPercentChangeId <<
			", \"PCTCHG_20T\": " << fidset.Rdm20TradingDayPercentChangeId <<
			" }";
		return o;
	}

	inline
	std::ostream& operator<< (std::ostream& o, const config_t& config) {
		o << "config_t: { "
			  "\"is_snmp_enabled\": " << (0 == config.is_snmp_enabled ? "false" : "true") << ""
			", \"is_agentx_subagent\": " << (0 == config.is_agentx_subagent ? "false" : "true") << ""
			", \"agentx_socket\": \"" << config.agentx_socket << "\""
			", \"key\": \"" << config.key << "\""
			", \"service_name\": \"" << config.service_name << "\""
			", \"sessions\": [";
		for (auto it = config.sessions.begin();
			it != config.sessions.end();
			++it)
		{
			if (it != config.sessions.begin())
				o << ", ";
			o << *it;
		}
		o << " ]"
			", \"history_table_size\": " << config.history_table_size <<
			", \"clients\": [";
		for (auto it = config.clients.begin();
			it != config.clients.end();
			++it)
		{
			if (it != config.clients.begin())
				o << ", ";
			o << *it;
		}
		o << " ]"
			", \"monitor_name\": \"" << config.monitor_name << "\""
			", \"event_queue_name\": \"" << config.event_queue_name << "\""
			", \"vendor_name\": \"" << config.vendor_name << "\""
			", \"maximum_data_size\": " << config.maximum_data_size <<
			", \"worker_count\": " << config.worker_count << 
			", \"suffix\": \"" << config.suffix << "\""
			", \"tz\": \"" << config.tz << "\""
			", \"tzdb\": \"" << config.tzdb << "\""
			", \"day_count\": " << config.day_count <<
			", \"archive_fids\": " << config.archive_fids <<
			" ] }";
		return o;
	}

} /* namespace gomi */

#endif /* __CONFIG_HH__ */

/* eof */