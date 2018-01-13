#include "testlogger.h"

/* Example

 Original log invocation:
   LogPrint("gobject", "CGovernanceManager::UpdateCachesAndClean -- Checking object for deletion: %s, deletion time = %d, time since deletion = %d, delete flag = %d, expired flag = %d\n",
            strHash, pObj->GetDeletionTime(), nTimeSinceDeletion, pObj->IsSetCachedDelete(), pObj->IsSetExpired());

 mapTestMessages filter entry:
   {"CGovernanceManager::UpdateCachesAndClean -- Checking object for deletion: %s, deletion time = %d, time since deletion = %d, delete flag = %d, expired flag = %d\n",
    TestFormat("checking_for_deletion",0,4)}

 output message: checking_for_deletion:77d790e590cb0323d9e77eabb565d3720de5744426c4238d84bb4da3be636aca:1
   where "77d7....6aca" and "1" are first and fifth parameters, respectively

*/


const std::map<std::string, TestFormat> mapTestMessages = {

    // Network messages

    {"PushInventory --  inv: %s peer=%d\n",
     TestFormat("push_inventory",0)},

    {"SendMessages -- GETDATA -- requesting inv = %s peer=%d\n",
     TestFormat("get_data",0)},

    {"CGovernanceManager::UpdatedBlockTip -- nCachedBlockHeight: %d\n",
     TestFormat("update_block_tip",0)},

    // Messages related to governance object manager

    {"CNode::AskFor -- WARNING: inventory message dropped: mapAskFor.size = %d, setAskFor.size = %d, MAPASKFOR_MAX_SZ = %d, SETASKFOR_MAX_SZ = %d, nSkipped = %d, peer=%d\n",
     TestFormat("inv_dropped",0)},

    {"MNGOVERNANCEOBJECT -- Received object: %s\n",
     TestFormat("govobj_received",0)},

    {"MNGOVERNANCEOBJECT -- Too many orphan objects, missing masternode=%s\n",
     TestFormat("too_many_orphans",0)},

    {"MNGOVERNANCEOBJECT -- Missing masternode for: %s, strError = %s\n",
     TestFormat("missing_mn",0)},

    {"CGovernanceManager::MasternodeRateCheck -- Rate too high: object hash = %s, masternode vin = %s, object timestamp = %d, rate = %f, max rate = %f\n",
     TestFormat("rate_too_high",0)},

    {"CGovernanceManager::MasternodeRateCheck -- object %s rejected due to too old timestamp, masternode vin = %s, timestamp = %d, current time = %d\n",
     TestFormat("govobj_invalid_time",0)},

    {"CGovernanceManager::MasternodeRateCheck -- object %s rejected due to too new (future) timestamp, masternode vin = %s, timestamp = %d, current time = %d\n",
     TestFormat("govobj_invalid_time",0)},

    {"MNGOVERNANCEOBJECT -- Received unrequested object: %s\n",
     TestFormat("govobj_unrequested_received",0)},

    {"MNGOVERNANCEOBJECT -- Received already seen object: %s\n",
     TestFormat("govobj_seen_received",0)},

    {"MNGOVERNANCEOBJECT -- Governance object is invalid - %s\n",
     TestFormat("govobj_invalid_received")},

    {"MNGOVERNANCEOBJECT -- Not enough fee confirmations for: %s, strError = %s\n",
     TestFormat("govobj_missing_confs",0)},

    {"AddGovernanceObject -- %s new, received form %s\n",
     TestFormat("govobj_accepted",0)},

    {"CGovernanceManager::CheckPostponedObjects -- additional relay: hash = %s\n",
     TestFormat("postponed_relay",0)},

    {"CGovernanceManager::CheckPostponedObjects -- additional relay of unknown object: %s\n",
     TestFormat("postponed_unknown_relay",0)}
};

CTestLogger& CTestLogger::GetInstance()
{
    static CTestLogger* testlogger = new CTestLogger(mapTestMessages);
    return *testlogger;

    //static CTestLogger testlogger(mapTestMessages);
    //return testlogger;
}
