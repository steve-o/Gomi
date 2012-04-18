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
gomi::config_t::validate()
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
		if (it->rssl_servers.empty()) {
			LOG(ERROR) << "Undefined server list for <connection name=\"" << it->connection_name << "\">.";
			return false;
		}
		if (it->application_id.empty()) {
			LOG(ERROR) << "Undefined application ID for <session name=\"" << it->session_name << "\">.";
			return false;
		}
		if (it->instance_id.empty()) {
			LOG(ERROR) << "Undefined instance ID for <session name=\"" << it->session_name << "\">.";
			return false;
		}
		if (it->user_name.empty()) {
			LOG(ERROR) << "Undefined user name for <session name=\"" << it->session_name << "\">.";
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
	if (interval.empty()) {
		LOG(ERROR) << "Undefined interval.";
		return false;
	}
	if (symbolmap.empty()) {
		LOG(ERROR) << "Undefined symbol map.";
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
	if (day_count.empty()) {
		LOG(ERROR) << "Undefined default analytic time period.";
		return false;
	}
	if (archive_fids.empty()) {
		LOG(ERROR) << "Undefined archive FID list.";
		return false;
	}
	if (realtime_fids.empty()) {
		LOG(ERROR) << "Undefined realtime FID list.";
		return false;
	}
	if (bins.empty()) {
		LOG(ERROR) << "Undefined bin list.";
		return false;
	}
	return true;
}

bool
gomi::config_t::parseDomElement (
	const DOMElement*	root
	)
{
	vpf::XMLStringPool xml;
	const DOMNodeList* nodeList;

	LOG(INFO) << "Parsing configuration ...";
/* Plugin configuration wrapped within a <config> node. */
	nodeList = root->getElementsByTagName (L"config");

	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseConfigNode (nodeList->item (i))) {
			LOG(ERROR) << "Failed parsing <config> nth-node #" << (1 + i) << '.';
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <config> nodes found in configuration.";

	if (!validate()) {
		LOG(ERROR) << "Failed validation, malformed configuration file requires correction.";
		return false;
	}

	LOG(INFO) << "Parsing complete.";
	return true;
}

bool
gomi::config_t::parseConfigNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	const DOMNodeList* nodeList;

/* <Snmp> */
	nodeList = elem->getElementsByTagName (L"Snmp");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseSnmpNode (nodeList->item (i))) {
			LOG(ERROR) << "Failed parsing <Snmp> nth-node #" << (1 + i) << '.';
			return false;
		}
	}
/* <Rfa> */
	nodeList = elem->getElementsByTagName (L"Rfa");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseRfaNode (nodeList->item (i))) {
			LOG(ERROR) << "Failed parsing <Rfa> nth-node #" << (1 + i) << '.';
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <Rfa> nodes found in configuration.";
/* <Gomi> */
	nodeList = elem->getElementsByTagName (L"Gomi");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseGomiNode (nodeList->item (i))) {
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
gomi::config_t::parseSnmpNode (
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
		if (!parseAgentXNode (nodeList->item (i))) {
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
gomi::config_t::parseAgentXNode (
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
gomi::config_t::parseRfaNode (
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

/* <service> */
	nodeList = elem->getElementsByTagName (L"service");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseServiceNode (nodeList->item (i))) {
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
		if (!parseSessionNode (nodeList->item (i))) {
			LOG(ERROR) << "Failed parsing <session> nth-node #" << (1 + i) << ".";
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <session> nodes found, RFA behaviour is undefined without a server list.";
/* <monitor> */
	nodeList = elem->getElementsByTagName (L"monitor");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseMonitorNode (nodeList->item (i))) {
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <monitor> nth-node #" << (1 + i) << ": \"" << text_content << "\".";
			return false;
		}
	}
/* <eventQueue> */
	nodeList = elem->getElementsByTagName (L"eventQueue");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseEventQueueNode (nodeList->item (i))) {
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <eventQueue> nth-node #" << (1 + i) << ": \"" << text_content << "\".";
			return false;
		}
	}
/* <vendor> */
	nodeList = elem->getElementsByTagName (L"vendor");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseVendorNode (nodeList->item (i))) {
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <vendor> nth-node #" << (1 + i) << ": \"" << text_content << "\".";
			return false;
		}
	}
	return true;
}

bool
gomi::config_t::parseServiceNode (
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
gomi::config_t::parseSessionNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	const DOMNodeList* nodeList;
	session_config_t session;

/* name="name" */
	session.session_name = xml.transcode (elem->getAttribute (L"name"));
	if (session.session_name.empty()) {
		LOG(ERROR) << "Undefined \"name\" attribute, value cannot be empty.";
		return false;
	}

/* <publisher> */
	nodeList = elem->getElementsByTagName (L"publisher");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parsePublisherNode (nodeList->item (i), session.publisher_name)) {
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <publisher> nth-node #" << (1 + i) << ": \"" << text_content << "\".";
			return false;
		}
	}
/* <connection> */
	nodeList = elem->getElementsByTagName (L"connection");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseConnectionNode (nodeList->item (i), session)) {
			LOG(ERROR) << "Failed parsing <connection> nth-node #" << (1 + i) << '.';
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <connection> nodes found, RFA behaviour is undefined without a server list.";
/* <login> */
	nodeList = elem->getElementsByTagName (L"login");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseLoginNode (nodeList->item (i), session)) {
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <login> nth-node #" << (1 + i) << ": \"" << text_content << "\".";
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <login> nodes found in configuration.";	
		
	sessions.push_back (session);
	return true;
}

bool
gomi::config_t::parseConnectionNode (
	const DOMNode*		node,
	session_config_t&	session
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	const DOMNodeList* nodeList;

/* name="name" */
	session.connection_name = xml.transcode (elem->getAttribute (L"name"));
	if (session.connection_name.empty()) {
		LOG(ERROR) << "Undefined \"name\" attribute, value cannot be empty.";
		return false;
	}
/* defaultPort="port" */
	session.rssl_default_port = xml.transcode (elem->getAttribute (L"defaultPort"));

/* <server> */
	nodeList = elem->getElementsByTagName (L"server");
	for (int i = 0; i < nodeList->getLength(); i++) {
		std::string server;
		if (!parseServerNode (nodeList->item (i), server)) {
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <server> nth-node #" << (1 + i) << ": \"" << text_content << "\".";			
			return false;
		}
		session.rssl_servers.push_back (server);
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <server> nodes found, RFA behaviour is undefined without a server list.";

	return true;
}

bool
gomi::config_t::parseServerNode (
	const DOMNode*		node,
	std::string&		server
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	server = xml.transcode (elem->getTextContent());
	if (server.size() == 0) {
		LOG(ERROR) << "Undefined hostname or IPv4 address.";
		return false;
	}
	return true;
}

bool
gomi::config_t::parseLoginNode (
	const DOMNode*		node,
	session_config_t&	session
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;

/* applicationId="id" */
	session.application_id = xml.transcode (elem->getAttribute (L"applicationId"));
/* instanceId="id" */
	session.instance_id = xml.transcode (elem->getAttribute (L"instanceId"));
/* userName="name" */
	session.user_name = xml.transcode (elem->getAttribute (L"userName"));
/* position="..." */
	session.position = xml.transcode (elem->getAttribute (L"position"));
	return true;
}

bool
gomi::config_t::parseMonitorNode (
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
gomi::config_t::parseEventQueueNode (
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
gomi::config_t::parsePublisherNode (
	const DOMNode*		node,
	std::string&		name
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;

/* name="name" */
	name = xml.transcode (elem->getAttribute (L"name"));
	return true;
}

bool
gomi::config_t::parseVendorNode (
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
gomi::config_t::parseGomiNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	const DOMNodeList* nodeList;
	std::string attr;

/* interval="seconds" */
	attr = xml.transcode (elem->getAttribute (L"interval"));
	if (!attr.empty())
		interval = attr;
/* tolerableDelay="milliseconds" */
	attr = xml.transcode (elem->getAttribute (L"tolerableDelay"));
	if (!attr.empty())
		tolerable_delay = attr;
/* symbolmap="file" */
	attr = xml.transcode (elem->getAttribute (L"symbolmap"));
	if (!attr.empty())
		symbolmap = attr;
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
		day_count = attr;

/* reset all lists */
	archive_fids.clear();
	realtime_fids.clear();
	bins.clear();
/* <fields> */
	nodeList = elem->getElementsByTagName (L"fields");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseFieldsNode (nodeList->item (i))) {
			LOG(ERROR) << "Failed parsing <fields> nth-node #" << (1 + i) << ".";
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <fields> nodes found.";
/* <bins> */
	nodeList = elem->getElementsByTagName (L"bins");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseBinsNode (nodeList->item (i))) {
			LOG(ERROR) << "Failed parsing <bins> nth-node #" << (1 + i) << ".";
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <bins> nodes found.";
	return true;
}

/* <fields> */
bool
gomi::config_t::parseFieldsNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	const DOMNodeList* nodeList;

/* <archive> */
	nodeList = elem->getElementsByTagName (L"archive");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseArchiveNode (nodeList->item (i))) {
			LOG(ERROR) << "Failed parsing <archive> nth-node #" << (1 + i) << ".";
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <archive> nodes found.";
/* <realtime> */
	nodeList = elem->getElementsByTagName (L"realtime");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseRealtimeNode (nodeList->item (i))) {
			LOG(ERROR) << "Failed parsing <realtime> nth-node #" << (1 + i) << ".";
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <realtime> nodes found.";
	return true;
}

/* <archive> */
bool
gomi::config_t::parseArchiveNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	const DOMNodeList* nodeList;

/* <fid> */
	nodeList = elem->getElementsByTagName (L"fid");
	for (int i = 0; i < nodeList->getLength(); i++) {
		std::string fid;
		if (!parseFidNode (nodeList->item (i), fid)) {
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <fid> nth-node #" << (1 + i) << ": \"" << text_content << "\".";
			return false;
		}
		archive_fids.push_back (fid);
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <fid> nodes found.";
	return true;
}

/* Convert Xml node from:
 *
 *	<fid name="TIMACT">5</fid>
 *
 * into:
 *
 *	"TIMACT=5"
 */

bool
gomi::config_t::parseFidNode (
	const DOMNode*		node,
	std::string&		fid
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

	const std::string fid_value = xml.transcode (elem->getTextContent());

	std::ostringstream os;
	os << name << '=' << std::stoi (fid_value);

	fid = os.str();

	return true;
}

/* <realtime> */
bool
gomi::config_t::parseRealtimeNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	const DOMNodeList* nodeList;

/* <bin> */
	nodeList = elem->getElementsByTagName (L"bin");
	for (int i = 0; i < nodeList->getLength(); i++) {
		if (!parseRealtimeBinNode (nodeList->item (i))) {
			LOG(ERROR) << "Failed parsing <bin> nth-node #" << (1 + i) << ".";
			return false;
		}
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <bin> nodes found.";
	return true;
}

/* <realtime><bin> */
bool
gomi::config_t::parseRealtimeBinNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	const DOMNodeList* nodeList;

/* <fid> */
	nodeList = elem->getElementsByTagName (L"fid");
	for (int i = 0; i < nodeList->getLength(); i++) {
		std::string fid;
		if (!parseFidNode (nodeList->item (i), fid)) {
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <fid> nth-node #" << (1 + i) << ": \"" << text_content << "\".";
			return false;
		}
		realtime_fids.push_back (fid);
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <fid> nodes found.";
	return true;
}

/* <bins> */
bool
gomi::config_t::parseBinsNode (
	const DOMNode*		node
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	const DOMNodeList* nodeList;

/* <bin> */
	nodeList = elem->getElementsByTagName (L"bin");
	for (int i = 0; i < nodeList->getLength(); i++) {
		std::string bin;
		if (!parseBinNode (nodeList->item (i), bin)) {
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <bin> nth-node #" << (1 + i) << ": \"" << text_content << "\".";
			return false;
		}
		bins.push_back (bin);
	}
	if (0 == nodeList->getLength())
		LOG(WARNING) << "No <bin> nodes found.";
	return true;
}

/* Convert Xml node from:
 *
 *	<bin name="OPEN">
 *		<time>09:00</time>
 *		<time>09:33</time>
 *	</bin>
 *
 * into:
 *
 *	"OPEN=09:00-09:33"
 */

bool
gomi::config_t::parseBinNode (
	const DOMNode*		node,
	std::string&		bin
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;
	const DOMNodeList* nodeList;

	if (!elem->hasAttributes()) {
		LOG(ERROR) << "No attributes found, a \"name\" attribute is required.";
		return false;
	}
	if (!elem->hasChildNodes()) {
		LOG(ERROR) << "No child nodes found, two <time> nodes are required.";
		return false;
	}

/* name="suffix" */
	const std::string name = xml.transcode (elem->getAttribute (L"name"));
	if (name.empty()) {
		LOG(ERROR) << "Undefined \"name\" attribute, value cannot be empty.";
		return false;
	}

	nodeList = elem->getElementsByTagName (L"time");
	if (2 != nodeList->getLength()) {
		LOG(ERROR) << "Two <time> child nodes are required.";
		return false;
	}

	std::string time[2];
	for (unsigned i = 0; i < 2; ++i) {
		if (!parseTimeNode (nodeList->item (i), time[i])) {
			const std::string text_content = xml.transcode (nodeList->item (i)->getTextContent());
			LOG(ERROR) << "Failed parsing <time> nth-node #" << (1 + i) << ": \"" << text_content << "\".";
			return false;
		}
	}

	std::ostringstream os;
	os << name << '=' << time[0] << '-' << time[1];

	bin = os.str();

	return true;
}

/* Convert Xml node from:
 *
 *	<time>09:00</time>
 *
 * into:
 *
 *	"09:00"
 */

bool
gomi::config_t::parseTimeNode (
	const DOMNode*		node,
	std::string&		time
	)
{
	const DOMElement* elem = static_cast<const DOMElement*>(node);
	vpf::XMLStringPool xml;

	time = xml.transcode (elem->getTextContent());

	return true;
}

/* </Gomi> */
/* </config> */

/* eof */

