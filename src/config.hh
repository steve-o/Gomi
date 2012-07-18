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

//  TREP-RT ADH hostname or IP address.
		std::vector<std::string> rssl_servers;

//  Default TREP-RT RSSL port, e.g. 14002 (interactive), 14003 (non-interactive).
		std::string rssl_default_port;

/* DACS application Id.  If the server authenticates with DACS, the consumer
 * application may be required to pass in a valid ApplicationId.
 * Range: "" (None) or 1-511 as an Ascii string.
 */
		std::string application_id;

/* InstanceId is used to differentiate applications running on the same host.
 * If there is more than one noninteractive provider instance running on the
 * same host, they must be set as a different value by the provider
 * application. Otherwise, the infrastructure component which the providers
 * connect to will reject a login request that has the same InstanceId value
 * and cut the connection.
 * Range: "" (None) or any Ascii string, presumably to maximum RFA_String length.
 */
		std::string instance_id;

/* DACS username, frequently non-checked and set to similar: user1.
 */
		std::string user_name;

/* DACS position, the station which the user is using.
 * Range: "" (None) or "<IPv4 address>/hostname" or "<IPv4 address>/net"
 */
		std::string position;
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
	};

	struct config_t
	{
		config_t();

		bool parseDomElement (const xercesc::DOMElement* elem);
		bool parseConfigNode (const xercesc::DOMNode* node);
		bool parseSnmpNode (const xercesc::DOMNode* node);
		bool parseAgentXNode (const xercesc::DOMNode* node);
		bool parseRfaNode (const xercesc::DOMNode* node);
		bool parseServiceNode (const xercesc::DOMNode* node);
		bool parseConnectionNode (const xercesc::DOMNode* node, session_config_t& session);
		bool parseServerNode (const xercesc::DOMNode* node, std::string& server);
		bool parsePublisherNode (const xercesc::DOMNode* node, std::string& publisher);
		bool parseLoginNode (const xercesc::DOMNode* node, session_config_t& session);
		bool parseSessionNode (const xercesc::DOMNode* node);
		bool parseMonitorNode (const xercesc::DOMNode* node);
		bool parseEventQueueNode (const xercesc::DOMNode* node);
		bool parseVendorNode (const xercesc::DOMNode* node);
		bool parseGomiNode (const xercesc::DOMNode* node);
		bool parseFieldsNode (const xercesc::DOMNode* node);
		bool parseArchiveNode (const xercesc::DOMNode* node);
		bool parseRealtimeNode (const xercesc::DOMNode* node);
		bool parseRealtimeBinNode (const xercesc::DOMNode* node);
		bool parseFidNode (const xercesc::DOMNode* node, fidset_t& fidset);
		bool parseBinsNode (const xercesc::DOMNode* node);
		bool parseBinNode (const xercesc::DOMNode* node, std::string& bin);
		bool parseTimeNode (const xercesc::DOMNode* node, std::string& time);

		bool validate();

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

//  RFA application logger monitor name.
		std::string monitor_name;

//  RFA event queue name.
		std::string event_queue_name;

//  RFA vendor name.
		std::string vendor_name;

//  RFA maximum data buffer size for SingleWriteIterator.
		std::string maximum_data_size;

//  Time quantum interval in seconds for checking bin boundaries.
		std::string interval;

//  Windows timer coalescing tolerable delay.
//  At least 32ms, corresponding to two 15.6ms platform timer interrupts.
//  Appropriate values are 10% to timer period.
//  Specify tolerable delay values and timer periods in multiples of 50 ms.
//  http://www.microsoft.com/whdc/system/pnppwr/powermgmt/TimerCoal.mspx
		std::string tolerable_delay;

//  RFA symbol name suffix for every publish.
		std::string suffix;

//  File path for symbol list a.k.a symbolmap.
		std::string symbolmap;

//  File path for time zone database that Boost::DateTimes likes.
		std::string tzdb;

//  Local time zone.
		std::string tz;

//  Default analytic time period
		std::string day_count;

//  FIDs for archival and realtime records.
		fidset_t archive_fids;
		std::map<std::string, fidset_t> realtime_fids;

//  Bin definitions.
		std::vector<std::string> bins;
	};

	inline
	std::ostream& operator<< (std::ostream& o, const session_config_t& session) {
		o << "{ "
			  "\"session_name\": \"" << session.session_name << "\""
			", \"connection_name\": \"" << session.connection_name << "\""
			", \"publisher_name\": \"" << session.publisher_name << "\""
			", \"rssl_servers\": [ ";
		for (auto it = session.rssl_servers.begin();
			it != session.rssl_servers.end();
			++it)
		{
			if (it != session.rssl_servers.begin())
				o << ", ";
			o << '"' << *it << '"';
		}
		o << " ]"
			", \"rssl_default_port\": \"" << session.rssl_default_port << "\""
			", \"application_id\": \"" << session.application_id << "\""
			", \"instance_id\": \"" << session.instance_id << "\""
			", \"user_name\": \"" << session.user_name << "\""
			", \"position\": \"" << session.position << "\""
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
			", \"monitor_name\": \"" << config.monitor_name << "\""
			", \"event_queue_name\": \"" << config.event_queue_name << "\""
			", \"vendor_name\": \"" << config.vendor_name << "\""
			", \"maximum_data_size\": \"" << config.maximum_data_size << "\""
			", \"interval\": \"" << config.interval << "\""
			", \"tolerable_delay\": \"" << config.tolerable_delay << "\""
			", \"suffix\": \"" << config.suffix << "\""
			", \"symbolmap\": \"" << config.symbolmap << "\""
			", \"tz\": \"" << config.tz << "\""
			", \"tzdb\": \"" << config.tzdb << "\""
			", \"day_count\": \"" << config.day_count << "\""
			", \"archive_fids\": " << config.archive_fids <<
			", \"realtime_fids\": { ";
		for (auto it = config.realtime_fids.begin();
			it != config.realtime_fids.end();
			++it)
		{
			if (it != config.realtime_fids.begin())
				o << ", ";
			o << it->first << ": " << it->second;
		}
		o << " }, \"bins\": [ ";
		for (auto it = config.bins.begin();
			it != config.bins.end();
			++it)
		{
			if (it != config.bins.begin())
				o << ", ";
			o << '"' << *it << '"';
		}
		o << " ] }";
		return o;
	}

} /* namespace gomi */

#endif /* __CONFIG_HH__ */

/* eof */
