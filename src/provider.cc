/* RFA provider.
 *
 * One single provider, and hence wraps a RFA session for simplicity.
 * Connection events (7.4.7.4, 7.5.8.3) are ignored as they're completely
 * useless.
 *
 * Definition of overlapping terms:
 *   OMM Provider:  Underlying RFA provider object.
 *   Provider:      Application encapsulation of provider functionality.
 *   Session:       RFA session object that contains one or more "Connection"
 *                  objects for horizontal scaling, e.g. RDF, GARBAN, TOPIC3.
 *   Connection:    RFA connection object that contains one or more servers.
 *   Server List:   A list of servers with round-robin failover connectivity.
 */

#include "provider.hh"

#include <algorithm>
#include <utility>

#include "chromium/logging.hh"
#include "error.hh"
#include "rfaostream.hh"
#include "session.hh"

using rfa::common::RFA_String;

/* Reuters Wire Format nomenclature for dictionary names. */
static const RFA_String kRdmFieldDictionaryName ("RWFFld");
static const RFA_String kEnumTypeDictionaryName ("RWFEnum");

gomi::provider_t::provider_t (
	const gomi::config_t& config,
	std::shared_ptr<gomi::rfa_t> rfa,
	std::shared_ptr<rfa::common::EventQueue> event_queue
	) :
	last_activity_ (boost::posix_time::microsec_clock::universal_time()),
	config_ (config),
	rfa_ (rfa),
	event_queue_ (event_queue),
	min_rwf_major_version_ (0),
	min_rwf_minor_version_ (0)
// Workaround for limited C++11 std::unordered_map API
//	directory_ (1048576)
{
	ZeroMemory (cumulative_stats_, sizeof (cumulative_stats_));
	ZeroMemory (snap_stats_, sizeof (snap_stats_));

	sessions_.reserve (config.sessions.size());

/* bucket capacity */
// MSVC 2010 does not fully support C++11 API
//	directory_.reserve (1048576);
	LOG(INFO) << "Provider directory capacity: " << directory_.max_size();
}

gomi::provider_t::~provider_t()
{
}

bool
gomi::provider_t::Init()
{
/* allocate */
	unsigned i = 0;
	for (auto it = config_.sessions.begin();
		it != config_.sessions.end();
		++it)
	{
		std::unique_ptr<session_t> session (new session_t (shared_from_this(), i++, *it, rfa_, event_queue_));
		sessions_.push_back (std::move (session));
	}

/* initialize */
	std::for_each (sessions_.begin(), sessions_.end(),
		[](std::unique_ptr<session_t>& it)
	{
		it->Init ();
	});

/* 6.2.2.1 RFA Version Info.  The version is only available if an application
 * has acquired a Session (i.e., the Session Layer library is loaded).
 */
	LOG(INFO) << "RFA: { productVersion: \"" << rfa::common::Context::getRFAVersionInfo()->getProductVersion() << "\" }";
	return true;
}

/* Create an item stream for a given symbol name.  The Item Stream maintains
 * the provider state on behalf of the application.
 */
bool
gomi::provider_t::CreateItemStream (
	const char* name,
	std::shared_ptr<item_stream_t> item_stream
	)
{
	VLOG(4) << "Creating item stream for RIC \"" << name << "\".";
	CHECK ((1 + directory_.size()) <= directory_.max_size());
	item_stream->rfa_name.set (name, 0, true);
	item_stream->token.resize (sessions_.size());
	item_stream->token.shrink_to_fit();
	unsigned i = 0;
	std::for_each (sessions_.begin(), sessions_.end(),
		[&name, &item_stream, &i](std::unique_ptr<session_t>& it)
	{
		assert ((bool)it);
		assert (!item_stream->token.empty());
		it->CreateItemStream (name, &item_stream->token[i]);
		++i;
	});
	const std::string key (name);
	auto status = directory_.emplace (std::make_pair (key, item_stream));
	CHECK (true == status.second);
	CHECK (directory_.end() != directory_.find (key));
	DVLOG(4) << "Directory size: " << directory_.size();
	last_activity_ = boost::posix_time::microsec_clock::universal_time();
	return true;
}

/* Send an Rfa message through the pre-created item stream.
 */

bool
gomi::provider_t::Send (
	item_stream_t*const stream,
	rfa::message::RespMsg*const msg
)
{
	unsigned i = 0;
	std::for_each (sessions_.begin(), sessions_.end(),
		[stream, msg, &i](std::unique_ptr<session_t>& it)
	{
		assert ((bool)it);
		assert (!stream->token.empty());
		it->Send (msg, stream->token[i], nullptr);
		++i;
	});
	cumulative_stats_[PROVIDER_PC_MSGS_SENT]++;
	last_activity_ = boost::posix_time::microsec_clock::universal_time();
	return true;
}

void
gomi::provider_t::GetServiceDirectory (
	rfa::data::Map*const map
	)
{
	rfa::data::MapWriteIterator it;
	rfa::data::MapEntry mapEntry;
	rfa::data::DataBuffer dataBuffer;
	rfa::data::FilterList filterList;
	const RFA_String serviceName (config_.service_name.c_str(), 0, false);

	map->setAssociatedMetaInfo (GetRwfMajorVersion(), GetRwfMinorVersion());
	it.start (*map);

/* No idea ... */
	map->setKeyDataType (rfa::data::DataBuffer::StringAsciiEnum);
/* One service. */
	map->setTotalCountHint (1);

/* Service name -> service filter list */
	mapEntry.setAction (rfa::data::MapEntry::Add);
	dataBuffer.setFromString (serviceName, rfa::data::DataBuffer::StringAsciiEnum);
	mapEntry.setKeyData (dataBuffer);
	GetServiceFilterList (&filterList);
	mapEntry.setData (static_cast<rfa::common::Data&>(filterList));
	it.bind (mapEntry);

	it.complete();
	last_activity_ = boost::posix_time::microsec_clock::universal_time();
}

void
gomi::provider_t::GetServiceFilterList (
	rfa::data::FilterList*const filterList
	)
{
	rfa::data::FilterListWriteIterator it;
	rfa::data::FilterEntry filterEntry;
	rfa::data::ElementList elementList;

	filterList->setAssociatedMetaInfo (GetRwfMajorVersion(), GetRwfMinorVersion());
	it.start (*filterList);  

/* SERVICE_INFO_ID and SERVICE_STATE_ID */
	filterList->setTotalCountHint (2);

/* SERVICE_INFO_ID */
	filterEntry.setFilterId (rfa::rdm::SERVICE_INFO_ID);
	filterEntry.setAction (rfa::data::FilterEntry::Set);
	GetServiceInformation (&elementList);
	filterEntry.setData (static_cast<const rfa::common::Data&>(elementList));
	it.bind (filterEntry);

/* SERVICE_STATE_ID */
	filterEntry.setFilterId (rfa::rdm::SERVICE_STATE_ID);
	filterEntry.setAction (rfa::data::FilterEntry::Set);
	GetServiceState (&elementList);
	filterEntry.setData (static_cast<const rfa::common::Data&>(elementList));
	it.bind (filterEntry);

	it.complete();
}

/* SERVICE_INFO_ID
 * Information about a service that does not update very often.
 */
void
gomi::provider_t::GetServiceInformation (
	rfa::data::ElementList*const elementList
	)
{
	rfa::data::ElementListWriteIterator it;
	rfa::data::ElementEntry element;
	rfa::data::DataBuffer dataBuffer;
	rfa::data::Array array_;
	const RFA_String serviceName (config_.service_name.c_str(), 0, false);
	const RFA_String vendorName (config_.vendor_name.c_str(), 0, false);

	elementList->setAssociatedMetaInfo (GetRwfMajorVersion(), GetRwfMinorVersion());
	it.start (*elementList);

/* Name<AsciiString>
 * Service name. This will match the concrete service name or the service group
 * name that is in the Map.Key.
 */
	element.setName (rfa::rdm::ENAME_NAME);
	dataBuffer.setFromString (serviceName, rfa::data::DataBuffer::StringAsciiEnum);
	element.setData (dataBuffer);
	it.bind (element);

/* Vendor<AsciiString> (optional)
 * Vendor whom provides the data.
 */
	element.setName (rfa::rdm::ENAME_VENDOR);
	dataBuffer.setFromString (vendorName, rfa::data::DataBuffer::StringAsciiEnum);
	element.setData (dataBuffer);
	it.bind (element);
	
/* Capabilities<Array of UInt>
 * Array of valid MessageModelTypes that the service can provide. The UInt
 * MesageModelType is extensible, using values defined in the RDM Usage Guide
 * (1-255). Login and Service Directory are omitted from this list. This
 * element must be set correctly because RFA will only request an item from a
 * service if the MessageModelType of the request is listed in this element.
 */
	element.setName (rfa::rdm::ENAME_CAPABILITIES);
	GetServiceCapabilities (&array_);
	element.setData (static_cast<const rfa::common::Data&>(array_));
	it.bind (element);

/* DictionariesUsed<Array of AsciiString>
 * List of Dictionary names that may be required to process all of the data 
 * from this service. Whether or not the dictionary is required depends on 
 * the needs of the consumer (e.g. display application, caching application)
 */
	element.setName (rfa::rdm::ENAME_DICTIONARYS_USED);
	GetServiceDictionaries (&array_);
	element.setData (static_cast<const rfa::common::Data&>(array_));
	it.bind (element);

/* src_dist requires a QoS */
#if 1
	element.setName (rfa::rdm::ENAME_QOS);
	GetDirectoryQoS (&array_);
	element.setData (static_cast<const rfa::common::Data&>(array_));
	it.bind (element);
#endif

	it.complete();
}

/* Array of valid MessageModelTypes that the service can provide.
 * rfa::data::Array does not require version tagging according to examples.
 */
void
gomi::provider_t::GetServiceCapabilities (
	rfa::data::Array*const capabilities
	)
{
	rfa::data::ArrayWriteIterator it;
	rfa::data::ArrayEntry arrayEntry;
	rfa::data::DataBuffer dataBuffer;

	it.start (*capabilities);

/* MarketPrice = 6 */
	dataBuffer.setUInt32 (rfa::rdm::MMT_MARKET_PRICE);
	arrayEntry.setData (dataBuffer);
	it.bind (arrayEntry);

	it.complete();
}

void
gomi::provider_t::GetServiceDictionaries (
	rfa::data::Array*const dictionaries
	)
{
	rfa::data::ArrayWriteIterator it;
	rfa::data::ArrayEntry arrayEntry;
	rfa::data::DataBuffer dataBuffer;

	it.start (*dictionaries);

/* RDM Field Dictionary */
	dataBuffer.setFromString (kRdmFieldDictionaryName, rfa::data::DataBuffer::StringAsciiEnum);
	arrayEntry.setData (dataBuffer);
	it.bind (arrayEntry);

/* Enumerated Type Dictionary */
	dataBuffer.setFromString (kEnumTypeDictionaryName, rfa::data::DataBuffer::StringAsciiEnum);
	arrayEntry.setData (dataBuffer);
	it.bind (arrayEntry);

	it.complete();
}

void
gomi::provider_t::GetDirectoryQoS (
	rfa::data::Array*const qos
	)
{
	rfa::data::ArrayWriteIterator it;
	rfa::data::ArrayEntry arrayEntry;
	rfa::data::DataBuffer dataBuffer;
	rfa::common::QualityOfService QoS;
	rfa::common::QualityOfServiceInfo QoSInfo;

	it.start (*qos);

/** Primary service QoS **/

/* Timeliness: age of data, either real-time, unspecified delayed timeliness,
 * unspecified timeliness, or any positive number representing the actual
 * delay in seconds.
 */
	QoS.setTimeliness (rfa::common::QualityOfService::realTime);
/* Rate: minimum period of change in data, either tick-by-tick, just-in-time
 * filtered rate, unspecified rate, or any positive number representing the
 * actual rate in milliseconds.
 */
	QoS.setRate (rfa::common::QualityOfService::tickByTick);

	QoSInfo.setQualityOfService (QoS);
	dataBuffer.setQualityOfServiceInfo (QoSInfo);
	arrayEntry.setData (dataBuffer);
	it.bind (arrayEntry);

	it.complete();
}


/* SERVICE_STATE_ID
 * State of a service.
 */
void
gomi::provider_t::GetServiceState (
	rfa::data::ElementList*const elementList
	)
{
	rfa::data::ElementListWriteIterator it;
	rfa::data::ElementEntry element;
	rfa::data::DataBuffer dataBuffer;

	elementList->setAssociatedMetaInfo (GetRwfMajorVersion(), GetRwfMinorVersion());
	it.start (*elementList);

/* ServiceState<UInt>
 * 1: Up/Yes
 * 0: Down/No
 * Is the original provider of the data responding to new requests. All
 * existing streams are left unchanged.
 */
	element.setName (rfa::rdm::ENAME_SVC_STATE);
	dataBuffer.setUInt32 (1);
	element.setData (dataBuffer);
	it.bind (element);

/* AcceptingRequests<UInt> (optional, interactive-only)
 * 1: Yes
 * 0: No
 * If the value is 0, then consuming applications should not send any new
 * requests to the service provider. (Reissues may still be sent.) If an RFA
 * application makes new requests to the service, they will be queued. All
 * existing streams are left unchanged.
 */
#if 0
	element.setName (rfa::rdm::ENAME_ACCEPTING_REQS);
	dataBuffer.setUInt32 (1);
	element.setData (dataBuffer);
	it.bind (element);
#endif

	it.complete();
}

/* eof */