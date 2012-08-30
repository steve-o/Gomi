/* User-configurable settings.
 */

#include "config.hh"

#include "chromium/logging.hh"

gomi::config_t::config_t() :
/* default values */
	is_snmp_enabled (false),
	is_agentx_subagent (true)
{
/* C++11 initializer lists not supported in MSVC2010 */
}

/* Minimal error handling parsing of an Xml node pulled from the
 * Analytics Engine.
 *
 * Returns true if configuration is valid, returns false on invalid content.
 */

using namespace xercesc;

/** L"" prefix is used in preference to u"" because of MSVC2010 **/

bool
gomi::config_t::Validate()
{
	if (service_name.empty()) {
		LOG(ERROR) << "Undefined service name.";
		return false;
	}
	if (sessions.empty()) {
		LOG(ERROR) << "Undefined session, expecting one or more session node.";
		return false;
	}
	for (auto it = sessions.begin();
		it != sessions.end();
		++it)
	{
		if (it->session_name.empty()) {
			LOG(ERROR) << "Undefined session name.";
			return false;
		}
		if (it->connection_name.empty()) {
			LOG(ERROR) << "Undefined connection name for <session name=\"" << it->session_name << "\">.";
			return false;
		}
		if (it->publisher_name.empty()) {
			LOG(ERROR) << "Undefined publisher name for <session name=\"" << it->session_name << "\">.";
			return false;
		}
		if (it->rssl_port.empty()) {
			LOG(ERROR) << "Undefined RSSL port for <session name=\"" << it->session_name << "\">.";
			return false;
		}
		if (0 == it->session_capacity) {
			LOG(ERROR) << "Undefined session capacity for <session name=\"" << it->session_name << "\">.";
			return false;
		}
	}
	if (monitor_name.empty()) {
		LOG(ERROR) << "Undefined monitor name.";
		return false;
	}
	if (event_queue_name.empty()) {
		LOG(ERROR) << "Undefined event queue name.";
		return false;
	}
	if (vendor_name.empty()) {
		LOG(ERROR) << "Undefined vendor name.";
		return false;
	}

/* Maximum data size must be provided for buffer allocation. */
	if (0 == maximum_data_size) {
		LOG(ERROR) << "Invalid maximum data size \"" << maximum_data_size << "\".";
		return false;
	}
	if (0 == worker_count) {
		LOG(ERROR) << "Invalid worker count \"" << worker_count << "\".";
		return false;
	}
	if (tz.empty()) {
		LOG(ERROR) << "Undefined time zone.";
		return false;
	}
	if (tzdb.empty()) {
		LOG(ERROR) << "Undefined time zone database.";
		return false;
	}
	if (0 == day_count) {
		LOG(ERROR) << "Invalid default analytic time period.";
		return false;
	}
	if (!archive_fids.RdmAverageVolumeId ||
	    !archive_fids.RdmAverageNonZeroVolumeId ||
	    !archive_fids.RdmTotalMovesId ||
	    !archive_fids.RdmMaximumMovesId ||
	    !archive_fids.RdmMinimumMovesId ||
	    !archive_fids.RdmSmallestMovesId ||
	    !archive_fids.Rdm10DayPercentChangeId ||
	    !archive_fids.Rdm15DayPercentChangeId ||
	    !archive_fids.Rdm20DayPercentChangeId ||
	    !archive_fids.Rdm10TradingDayPercentChangeId ||
	    !archive_fids.Rdm15TradingDayPercentChangeId ||
	    !archive_fids.Rdm20TradingDayPercentChangeId )
	{
		LOG(ERROR) << "Undefined archive FID set.";
		return false;
	}
	return true;
}

bool
gomi::config_t::ParseDomElement (
	const DOMElement*	root
	)
{
	vpf::XMLStringPool xml;
	const DOMNodeList* nodeList;

	LOG(INFO) << "Parsing configuration ...";
/* Plugin configuration wrapped within a <config> node. */
	nodeList = root->getElementsByTagName (L"config");

	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!ParseConfigNode (nodeList->item (i))) {
			LOG(ERROR) << "Failed parsing <config> nth-node #" << (1 + i) << '.';
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <config> nodes found in configuration.";

	if (!Validate()) {
		LOG(ERROR) << "Failed validation, malformed configuration file requires correction.";
		return false;
	}

	LOG(INFO) << "Parsing complete.";
	return true;
}

bool
gomi::config_t::ParseConfigNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	const DOMNodeList* nodeList;

/* <Snmp> */
	nodeList = elem->getElementsByTagName (L"Snmp");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!ParseSnmpNode (nodeList->item (i))) {
			LOG(ERROR) << "Failed parsing <Snmp> nth-node #" << (1 + i) << '.';
			return false;
		}
	}
/* <Rfa> */
	nodeList = elem->getElementsByTagName (L"Rfa");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!ParseRfaNode (nodeList->item (i))) {
			LOG(ERROR) << "Failed parsing <Rfa> nth-node #" << (1 + i) << '.';
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <Rfa> nodes found in configuration.";
/* <Gomi> */
	nodeList = elem->getElementsByTagName (L"Gomi");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!ParseGomiNode (nodeList->item (i))) {
			LOG(ERROR) << "Failed parsing <Gomi> nth-node #" << (1 + i) << '.';
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <Gomi> nodes found in configuration.";
	return true;
}

/* <Snmp> */
bool
gomi::config_t::ParseSnmpNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	const DOMNodeList* nodeList;
	vpf::XMLStringPool xml;
	std::string attr;

/* logfile="file path" */
	attr = xml.transcode (elem->getAttribute (L"filelog"));
	if (!attr.empty())
		snmp_filelog = attr;

/* <agentX> */
	nodeList = elem->getElementsByTagName (L"agentX");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!ParseAgentXNode (nodeList->item (i))) {
			vpf::XMLStringPool xml;
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <agentX> nth-node #" << (1 + i) << ": \"" << text_content << "\".";
			return false;
		}
	}
	this->is_snmp_enabled = true;
	return true;
}

bool
gomi::config_t::ParseAgentXNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	std::string attr;

/* subagent="bool" */
	attr = xml.transcode (elem->getAttribute (L"subagent"));
	if (!attr.empty())
		is_agentx_subagent = (0 == attr.compare ("true"));

/* socket="..." */
	attr = xml.transcode (elem->getAttribute (L"socket"));
	if (!attr.empty())
		agentx_socket = attr;
	return true;
}

/* </Snmp> */

/* <Rfa> */
bool
gomi::config_t::ParseRfaNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	const DOMNodeList* nodeList;
	std::string attr;

/* key="name" */
	attr = xml.transcode (elem->getAttribute (L"key"));
	if (!attr.empty())
		key = attr;

/* maximumDataSize="bytes" */
	attr = xml.transcode (elem->getAttribute (L"maximumDataSize"));
	if (!attr.empty())
		maximum_data_size = (size_t)std::atol (attr.c_str());

/* historyTableSize="rows" */
	attr = xml.transcode (elem->getAttribute (L"historyTableSize"));
	if (!attr.empty())
		history_table_size = (unsigned)std::atol (attr.c_str());

/* <service> */
	nodeList = elem->getElementsByTagName (L"service");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!ParseServiceNode (nodeList->item (i))) {
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <service> nth-node #" << (1 + i) << ": \"" << text_content << "\".";
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <service> nodes found in configuration.";
/* <session> */
	nodeList = elem->getElementsByTagName (L"session");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!ParseSessionNode (nodeList->item (i))) {
			LOG(ERROR) << "Failed parsing <session> nth-node #" << (1 + i) << ".";
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <session> nodes found, RFA behaviour is undefined without a server list.";
/* <monitor> */
	nodeList = elem->getElementsByTagName (L"monitor");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!ParseMonitorNode (nodeList->item (i))) {
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <monitor> nth-node #" << (1 + i) << ": \"" << text_content << "\".";
			return false;
		}
	}
/* <eventQueue> */
	nodeList = elem->getElementsByTagName (L"eventQueue");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!ParseEventQueueNode (nodeList->item (i))) {
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <eventQueue> nth-node #" << (1 + i) << ": \"" << text_content << "\".";
			return false;
		}
	}
/* <vendor> */
	nodeList = elem->getElementsByTagName (L"vendor");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!ParseVendorNode (nodeList->item (i))) {
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <vendor> nth-node #" << (1 + i) << ": \"" << text_content << "\".";
			return false;
		}
	}
	return true;
}

bool
gomi::config_t::ParseServiceNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	std::string attr;

/* name="name" */
	attr = xml.transcode (elem->getAttribute (L"name"));
	if (attr.empty()) {
/* service name cannot be empty */
		LOG(ERROR) << "Undefined \"name\" attribute, value cannot be empty.";
		return false;
	}
	service_name = attr;
	return true;
}

bool
gomi::config_t::ParseSessionNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	const DOMNodeList* nodeList;
	std::string attr;
	session_config_t session;

/* name="name" */
	session.session_name = xml.transcode (elem->getAttribute (L"name"));
	if (session.session_name.empty()) {
		LOG(ERROR) << "Undefined \"name\" attribute, value cannot be empty.";
		return false;
	}

/* capacity="count" */
	attr = xml.transcode (elem->getAttribute (L"capacity"));
	if (attr.empty()) {
		LOG(ERROR) << "Undefined \"capacity\" attribute, value cannot be empty or 0.";
		return false;
	}
	session.session_capacity = (unsigned)std::atol (attr.c_str());

/* <publisher> */
	nodeList = elem->getElementsByTagName (L"publisher");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!ParsePublisherNode (nodeList->item (i), &session.publisher_name)) {
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <publisher> nth-node #" << (1 + i) << ": \"" << text_content << "\".";
			return false;
		}
	}
/* <connection> */
	nodeList = elem->getElementsByTagName (L"connection");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!ParseConnectionNode (nodeList->item (i), &session)) {
			LOG(ERROR) << "Failed parsing <connection> nth-node #" << (1 + i) << '.';
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <connection> nodes found, RFA behaviour is undefined without a server list.";
		
	sessions.push_back (session);
	return true;
}

bool
gomi::config_t::ParseConnectionNode (
	const DOMNode*		node,
	session_config_t*const	session
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	const DOMNodeList* nodeList;

/* name="name" */
	session->connection_name = xml.transcode (elem->getAttribute (L"name"));
	if (session->connection_name.empty()) {
		LOG(ERROR) << "Undefined \"name\" attribute, value cannot be empty.";
		return false;
	}
/* port="port" */
	session->rssl_port = xml.transcode (elem->getAttribute (L"port"));

/* <client name="username">
 * Setting is optional.
 */
	nodeList = elem->getElementsByTagName (L"client");
	for (int i = 0; i < nodeList->getLength(); i++) {
		client_config_t client;
		if (!ParseClientNode (nodeList->item (i), &client)) {
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <client> nth-node #" << (1 + i) << ": \"" << text_content << "\".";			
			return false;
		}
		clients.push_back (client);
	}

	return true;
}

bool
gomi::config_t::ParseClientNode (
	const DOMNode*		node,
	client_config_t*const	client
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;

/* name="username" */
	client->name = xml.transcode (elem->getAttribute (L"name"));
	if (client->name.empty()) {
		LOG(ERROR) << "Undefined \"name\" attribute, value cannot be empty.";
		return false;
	}
	return true;
}

bool
gomi::config_t::ParseMonitorNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	std::string attr;

/* name="name" */
	attr = xml.transcode (elem->getAttribute (L"name"));
	if (!attr.empty())
		monitor_name = attr;
	return true;
}

bool
gomi::config_t::ParseEventQueueNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	std::string attr;

/* name="name" */
	attr = xml.transcode (elem->getAttribute (L"name"));
	if (!attr.empty())
		event_queue_name = attr;
	return true;
}

bool
gomi::config_t::ParsePublisherNode (
	const DOMNode*		node,
	std::string*const	name
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;

/* name="name" */
	*name = xml.transcode (elem->getAttribute (L"name"));
	return true;
}

bool
gomi::config_t::ParseVendorNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	std::string attr;

/* name="name" */
	attr = xml.transcode (elem->getAttribute (L"name"));
	if (!attr.empty())
		vendor_name = attr;
	return true;
}

/* </Rfa> */

/* <Gomi> */
bool
gomi::config_t::ParseGomiNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	const DOMNodeList* nodeList;
	std::string attr;

/* workerCount="threads" */
	attr = xml.transcode (elem->getAttribute (L"workerCount"));
	if (!attr.empty())
		worker_count = (unsigned)std::atol (attr.c_str());
/* suffix="text" */
	attr = xml.transcode (elem->getAttribute (L"suffix"));
	if (!attr.empty())
		suffix = attr;
/* tz="text" */
	attr = xml.transcode (elem->getAttribute (L"TZ"));
	if (!attr.empty())
		tz = attr;
/* tzdb="file" */
	attr = xml.transcode (elem->getAttribute (L"TZDB"));
	if (!attr.empty())
		tzdb = attr;
/* dayCount="days" */
	attr = xml.transcode (elem->getAttribute (L"dayCount"));
	if (!attr.empty())
		day_count = (unsigned)std::atol (attr.c_str());

/* reset all lists */
	ZeroMemory (&archive_fids, sizeof (archive_fids));
/* <fields> */
	nodeList = elem->getElementsByTagName (L"fields");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!ParseFieldsNode (nodeList->item (i))) {
			LOG(ERROR) << "Failed parsing <fields> nth-node #" << (1 + i) << ".";
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <fields> nodes found.";
	return true;
}

/* <fields> */
bool
gomi::config_t::ParseFieldsNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	const DOMNodeList* nodeList;

/* <archive> */
	nodeList = elem->getElementsByTagName (L"archive");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!ParseArchiveNode (nodeList->item (i))) {
			LOG(ERROR) << "Failed parsing <archive> nth-node #" << (1 + i) << ".";
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <archive> nodes found.";
	return true;
}

/* <archive> */
bool
gomi::config_t::ParseArchiveNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	const DOMNodeList* nodeList;

/* <fid> */
	nodeList = elem->getElementsByTagName (L"fid");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!ParseFidNode (nodeList->item (i), &archive_fids)) {
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <fid> nth-node #" << (1 + i) << ": \"" << text_content << "\".";
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <fid> nodes found.";
	return true;
}

/* Convert Xml node from:
 *
 *	<fid name="TIMACT" value="5"/>
 */

bool
gomi::config_t::ParseFidNode (
	const DOMNode*		node,
	gomi::fidset_t*const	fidset
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;

	if (!elem->hasAttributes()) {
		LOG(ERROR) << "No attributes found, \"name\" attribute required.";
		return false;
	}
/* name="field name" */
	const std::string name = xml.transcode (elem->getAttribute (L"name"));
	if (name.empty()) {
		LOG(ERROR) << "Undefined \"name\" attribute, value cannot be empty.";
		return false;
	}

/* convenient inlining */
	int* fid = nullptr;
	if ("VMA" == name)		fid = &fidset->RdmAverageVolumeId;
	else if ("NZERO_VMA" == name)	fid = &fidset->RdmAverageNonZeroVolumeId;
	else if ("NUM_MOVES" == name)	fid = &fidset->RdmTotalMovesId;
	else if ("NM_HIGH" == name)	fid = &fidset->RdmMaximumMovesId;
	else if ("NM_LOW" == name)	fid = &fidset->RdmMinimumMovesId;
	else if ("NM_SMALL" == name)	fid = &fidset->RdmSmallestMovesId;
	else if ("PCTCHG_10D" == name)	fid = &fidset->Rdm10DayPercentChangeId;
	else if ("PCTCHG_15D" == name)	fid = &fidset->Rdm15DayPercentChangeId;
	else if ("PCTCHG_20D" == name)	fid = &fidset->Rdm20DayPercentChangeId;
	else if ("PCTCHG_10T" == name)	fid = &fidset->Rdm10TradingDayPercentChangeId;
	else if ("PCTCHG_15T" == name)	fid = &fidset->Rdm15TradingDayPercentChangeId;
	else if ("PCTCHG_20T" == name)	fid = &fidset->Rdm20TradingDayPercentChangeId;
	else {
		LOG(ERROR) << "Unknown \"name\" attribute value \"" << name << "\".";
		return false;
	}

/* value="fid" */
	const std::string value = xml.transcode (elem->getAttribute (L"value"));
	if (value.empty()) {
		LOG(ERROR) << "Undefined \"value\" attribute, value cannot be empty.";
		return false;
	}

	*fid = std::stoi (value);
	return true;
}

/* </Gomi> */
/* </config> */

/* eof */