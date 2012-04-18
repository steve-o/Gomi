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
       #define COLUMN_GOMIPLUGINPUBLISHIVL		8
       #define COLUMN_GOMIPLUGINTOLERABLEDELAY		9
       #define COLUMN_GOMIPLUGINRESETTIME		10
       #define COLUMN_GOMIPLUGINRICSUFFIX		11

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

/* column number definitions for table gomiSessionTable */
       #define COLUMN_GOMISESSIONPLUGINID		1
       #define COLUMN_GOMISESSIONPLUGINUNIQUEINSTANCE		2
       #define COLUMN_GOMISESSIONUNIQUEINSTANCE		3
       #define COLUMN_GOMISESSIONRSSLSERVERS		4
       #define COLUMN_GOMISESSIONRSSLDEFAULTPORT		5
       #define COLUMN_GOMISESSIONAPPLICATIONID		6
       #define COLUMN_GOMISESSIONINSTANCEID		7
       #define COLUMN_GOMISESSIONUSERNAME		8
       #define COLUMN_GOMISESSIONPOSITION		9
       #define COLUMN_GOMISESSIONSESSIONNAME		10
       #define COLUMN_GOMISESSIONCONNECTIONNAME		11
       #define COLUMN_GOMISESSIONPUBLISHERNAME		12

/* column number definitions for table gomiSessionPerformanceTable */
       #define COLUMN_GOMISESSIONPERFORMANCEID		1
       #define COLUMN_GOMISESSIONPERFORMANCEPLUGINUNIQUEINSTANCE		2
       #define COLUMN_GOMISESSIONPERFORMANCEUNIQUEINSTANCE		3
       #define COLUMN_GOMISESSIONLASTACTIVITY		4
       #define COLUMN_GOMISESSIONRFAMSGSSENT		5
       #define COLUMN_GOMIRFAEVENTSRECEIVED		6
       #define COLUMN_GOMIRFAEVENTSDISCARDED		7
       #define COLUMN_GOMIOMMITEMEVENTSRECEIVED		8
       #define COLUMN_GOMIOMMITEMEVENTSDISCARDED		9
       #define COLUMN_GOMIRESPONSEMSGSRECEIVED		10
       #define COLUMN_GOMIRESPONSEMSGSDISCARDED		11
       #define COLUMN_GOMIMMTLOGINRESPONSERECEIVED		12
       #define COLUMN_GOMIMMTLOGINRESPONSEDISCARDED		13
       #define COLUMN_GOMIMMTLOGINSUCCESSRECEIVED		14
       #define COLUMN_GOMIMMTLOGINSUSPECTRECEIVED		15
       #define COLUMN_GOMIMMTLOGINCLOSEDRECEIVED		16
       #define COLUMN_GOMIOMMCMDERRORS		17
       #define COLUMN_GOMIMMTLOGINSVALIDATED		18
       #define COLUMN_GOMIMMTLOGINSMALFORMED		19
       #define COLUMN_GOMIMMTLOGINSSENT		20
       #define COLUMN_GOMIMMTDIRECTORYSVALIDATED		21
       #define COLUMN_GOMIMMTDIRECTORYSMALFORMED		22
       #define COLUMN_GOMIMMTDIRECTORYSSENT		23
       #define COLUMN_GOMITOKENSGENERATED		24
       #define COLUMN_GOMIMMTLOGINSTREAMSTATE		25
       #define COLUMN_GOMIMMTLOGINDATASTATE		26

} /* namespace gomi */

#endif /* GOMIMIB_H */
