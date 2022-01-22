// Copyright (c) 2021 Alterdot developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef ADOT_BDNSDB_H
#define ADOT_BDNSDB_H

#include "uint256.h"
#include "dbwrapper.h"

#include <string>
#include <utility>

struct BDNSRecord {
    std::string content;
    uint256 regTxid, lastUpdateTxid;
    
    template<typename Stream>
    void Serialize(Stream &s) const {
        s << content;
        s << regTxid;
        s << lastUpdateTxid;
    }

    template<typename Stream>
    void Unserialize(Stream& s) {
        s >> content;
        s >> regTxid;
        s >> lastUpdateTxid;
    }
};

/** Access to the BDNS database (bdns/) */
class CBDNSDB : public CDBWrapper
{
private:
    bool WriteVersion();
    int GetLastChangeHeight();
    bool SetLastChangeHeight();

public:
    CBDNSDB(size_t nCacheSize, bool fMemory = false, bool fWipe = false);

    bool GetContentFromBDNSRecord(const std::string &bdnsName, std::string &content);
    bool HasBDNSRecord(const std::string &bdnsName);
    bool ReadBDNSRecord(const std::string &bdnsName, BDNSRecord &bdnsRecord);
    bool WriteBDNSRecord(const std::string &bdnsName, const BDNSRecord &bdnsRecord);
    bool UpdateBDNSRecord(const std::string &bdnsName, const std::string &content, const uint256 &updateTxid);
    bool EraseBDNSRecord(const std::string &bdnsName);
    
    bool CleanDatabase();
    bool CheckVersion();

    bool SetHeight(const int &nHeight);

    bool WriteReindexing(bool fReindexing);
    bool AwaitsReindexing();

    bool WriteCorruptionState(bool fPossibleCorruption);
    bool PossibleCorruption();
};

#endif // ADOT_BDNSDB_H
