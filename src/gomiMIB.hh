/*
 * Note: this file originally auto-generated by mib2c using
 *  : mib2c.iterate.conf 17821 2009-11-11 09:00:00Z dts12 $
 */
#ifndef GOMIMIB_H
#define GOMIMIB_H

namespace gomi {

/* function declarations */
bool init_gomiMIB(void);

/* column number definitions for table gomiPluginTable */
       #define COLUMN_GOMIPLUGINID		1
       #define COLUMN_GOMIPLUGINUNIQUEINSTANCE		2
       #define COLUMN_GOMIPLUGINWINDOWSREGISTRYKEY		3
       #define COLUMN_GOMIPLUGINSERVICENAME		4
       #define COLUMN_GOMIPLUGINMONITORNAME		5
       #define COLUMN_GOMIPLUGINEVENTQUEUENAME		6
       #define COLUMN_GOMIPLUGINVENDORNAME		7
       #define COLUMN_GOMIPLUGINMAXIMUMDATASIZE		8
       #define COLUMN_GOMIPLUGINSESSIONCAPACITY		9
       #define COLUMN_GOMIPLUGINWORKERCOUNT		10
       #define COLUMN_GOMIPLUGINRICSUFFIX		11
       #define COLUMN_GOMIPLUGINDEFAULTTIMEZONE		12
       #define COLUMN_GOMIPLUGINTIMEZONEDATABASE		13
       #define COLUMN_GOMIPLUGINDEFAULTDAYCOUNT		14

/* column number definitions for table gomiPluginPerformanceTable */
       #define COLUMN_GOMIPLUGINPERFORMANCEID		1
       #define COLUMN_GOMIPLUGINPERFORMANCEUNIQUEINSTANCE		2
       #define COLUMN_GOMITCLQUERYRECEIVED		3
       #define COLUMN_GOMITIMERQUERYRECEIVED		4
       #define COLUMN_GOMILASTACTIVITY		5
       #define COLUMN_GOMITCLSVCTIMEMIN		6
       #define COLUMN_GOMITCLSVCTIMEMEAN		7
       #define COLUMN_GOMITCLSVCTIMEMAX		8
       #define COLUMN_GOMITIMERSVCTIMEMIN		9
       #define COLUMN_GOMITIMERSVCTIMEMEAN		10
       #define COLUMN_GOMITIMERSVCTIMEMAX		11
       #define COLUMN_GOMIMSGSSENT		12
       #define COLUMN_GOMILASTMSGSSENT		13

/* column number definitions for table gomiClientTable */
       #define COLUMN_GOMICLIENTPLUGINID		1
       #define COLUMN_GOMICLIENTPLUGINUNIQUEINSTANCE		2
       #define COLUMN_GOMICLIENTUNIQUEINSTANCE		3
       #define COLUMN_GOMICLIENTIPADDRESS		4
       #define COLUMN_GOMICLIENTRSSLPORT		5
       #define COLUMN_GOMICLIENTSESSIONNAME		6
       #define COLUMN_GOMICLIENTCONNECTIONNAME		7
       #define COLUMN_GOMICLIENTPUBLISHERNAME		8
       #define COLUMN_GOMICLIENTLOGINNAME		9

/* column number definitions for table gomiClientPerformanceTable */
       #define COLUMN_GOMICLIENTPERFORMANCEID		1
       #define COLUMN_GOMICLIENTPERFORMANCEPLUGINUNIQUEINSTANCE		2
       #define COLUMN_GOMICLIENTPERFORMANCEUNIQUEINSTANCE		3
       #define COLUMN_GOMICLIENTLASTACTIVITY		4
       #define COLUMN_GOMICLIENTRFAMSGSSENT		5
       #define COLUMN_GOMIRFAEVENTSRECEIVED		6
       #define COLUMN_GOMIRFAEVENTSDISCARDED		7
       #define COLUMN_GOMIOMMSOLICITEDITEMEVENTSRECEIVED		8
       #define COLUMN_GOMIOMMSOLICITEDITEMEVENTSDISCARDED		9
       #define COLUMN_GOMIREQUESTMSGSRECEIVED		10
       #define COLUMN_GOMIREQUESTMSGSDISCARDED		11
       #define COLUMN_GOMIMMTLOGINRECEIVED		12
       #define COLUMN_GOMIMMTLOGINVALIDATED		13
       #define COLUMN_GOMIMMTLOGINMALFORMED		14
       #define COLUMN_GOMIMMTLOGINREJECTED		15
       #define COLUMN_GOMIMMTLOGINACCEPTED		16
       #define COLUMN_GOMIMMTLOGINRESPONSEVALIDATED		17
       #define COLUMN_GOMIMMTLOGINRESPONSEMALFORMED		18
       #define COLUMN_GOMIMMTLOGINEXCEPTION		19
       #define COLUMN_GOMIMMTDIRECTORYREQUESTRECEIVED		20
       #define COLUMN_GOMIMMTDIRECTORYREQUESTVALIDATED		21
       #define COLUMN_GOMIMMTDIRECTORYREQUESTMALFORMED		22
       #define COLUMN_GOMIMMTDIRECTORYVALIDATED		23
       #define COLUMN_GOMIMMTDIRECTORYMALFORMED		24
       #define COLUMN_GOMIMMTDIRECTORYSENT		25
       #define COLUMN_GOMIMMTDIRECTORYEXCEPTION		26
       #define COLUMN_GOMIMMTDICTIONARYREQUESTRECEIVED		27
       #define COLUMN_GOMIMMTDICTIONARYREQUESTVALIDATED		28
       #define COLUMN_GOMIMMTDICTIONARYREQUESTMALFORMED		29
       #define COLUMN_GOMIITEMREQUESTRECEIVED		30
       #define COLUMN_GOMIITEMREISSUEREQUESTRECEIVED		31
       #define COLUMN_GOMIITEMCLOSEREQUESTRECEIVED		32
       #define COLUMN_GOMIITEMREQUESTMALFORMED		33
       #define COLUMN_GOMIITEMREQUESTBEFORELOGIN		34
       #define COLUMN_GOMIITEMDUPLICATESNAPSHOT		35
       #define COLUMN_GOMIITEMREQUESTDISCARDED		36
       #define COLUMN_GOMIITEMREQUESTREJECTED		37
       #define COLUMN_GOMIITEMVALIDATED		38
       #define COLUMN_GOMIITEMMALFORMED		39
       #define COLUMN_GOMIITEMNOTFOUND		40
       #define COLUMN_GOMIITEMSENT		41
       #define COLUMN_GOMIITEMCLOSED		42
       #define COLUMN_GOMIITEMEXCEPTION		43
       #define COLUMN_GOMIOMMINACTIVECLIENTSESSIONRECEIVED		44
       #define COLUMN_GOMIOMMINACTIVECLIENTSESSIONEXCEPTION		45

} /* namespace gomi */

#endif /* GOMIMIB_H */