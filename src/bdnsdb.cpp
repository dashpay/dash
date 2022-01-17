// Copyright (c) 2021 Alterdot developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "bdnsdb.h"

#include "util.h"

static const char DB_DOMAIN = 'd';

static const char DB_INTERNAL = 'I';
static const char db_height = 'H';
static const char db_last_change = 'L';
static const char db_corruption = 'C';
static const char db_reindexing = 'R';
static const char db_version = 'V';
static const int db_version_num = 1;
static const int db_default_height = -10;

CBDNSDB::CBDNSDB(size_t nCacheSize, bool fMemory, bool fWipe) : CDBWrapper(GetDataDir() / "bdns", nCacheSize, fMemory, fWipe) {}

bool CBDNSDB::GetContentFromBDNSRecord(const std::string &bdnsName, std::string &content) {
    BDNSRecord storedValue;

    if (Read(std::make_pair(DB_DOMAIN, bdnsName), storedValue)) {
        content = storedValue.content;

        return true;
    }

    return false;
}

bool CBDNSDB::HasBDNSRecord(const std::string &bdnsName) {
    return Exists(std::make_pair(DB_DOMAIN, bdnsName));
}

bool CBDNSDB::ReadBDNSRecord(const std::string &bdnsName, BDNSRecord& bdnsRecord) {
    return Read(std::make_pair(DB_DOMAIN, bdnsName), bdnsRecord);
}

bool CBDNSDB::WriteBDNSRecord(const std::string &bdnsName, const BDNSRecord &bdnsRecord) {
    if (Write(std::make_pair(DB_DOMAIN, bdnsName), bdnsRecord)) {
        SetLastChangeHeight();
        return true;
    }

    return false;
}

bool CBDNSDB::UpdateBDNSRecord(const std::string &bdnsName, const std::string &content, const uint256 &updateTxid) {
    BDNSRecord storedValue;

    if (Read(std::make_pair(DB_DOMAIN, bdnsName), storedValue)) {
        storedValue.content = content;
        storedValue.lastUpdateTxid = updateTxid;
        
        if (Write(std::make_pair(DB_DOMAIN, bdnsName), storedValue)) {
            SetLastChangeHeight();
            return true;
        }
    }

    return false;
}

bool CBDNSDB::EraseBDNSRecord(const std::string &bdnsName) {
    if (Erase(std::make_pair(DB_DOMAIN, bdnsName))) {
        SetLastChangeHeight();
        return true;
    }

    return false;
}

// clears all records in the database, old or new format and writes the initial DB internals
bool CBDNSDB::CleanDatabase() {
    std::unique_ptr<CDBIterator> pcursor(NewIterator());
    size_t batch_size = 1 << 20;
    CDBBatch batch(*this);
    bool ret = true;

    pcursor->SeekToFirst();

    if (!CheckVersion()) {
        std::string oldKey;

        while (pcursor->Valid()) {
            if (pcursor->GetKey(oldKey))
                batch.Erase(oldKey);
            
            if (batch.SizeEstimate() > batch_size) {
                ret = ret && WriteBatch(batch);
                batch.Clear();
            }

            pcursor->Next();
        }
    } else {
        std::pair<unsigned char, std::string> newKey;

        while (pcursor->Valid()) {
            if (pcursor->GetKey(newKey))
                batch.Erase(newKey);
            
            if (batch.SizeEstimate() > batch_size) {
                ret = ret && WriteBatch(batch);
                batch.Clear();
            }

            pcursor->Next();
        }
    }

    ret = ret && WriteBatch(batch);
    CompactFull();

    return ret && WriteVersion() && SetHeight(db_default_height) && SetLastChangeHeight() && WriteCorruptionState(false);
}

bool CBDNSDB::CheckVersion() {
    int storedValue;

    if (Read(std::make_pair(DB_INTERNAL, db_version), storedValue)) {
        return storedValue == db_version_num;
    }

    return false;
}

bool CBDNSDB::WriteVersion() {
    return Write(std::make_pair(DB_INTERNAL, db_version), db_version_num);
}

int CBDNSDB::GetLastChangeHeight() {
    int storedValue;

    if (Read(std::make_pair(DB_INTERNAL, db_last_change), storedValue)) {
        return storedValue;
    }

    return db_default_height;
}

bool CBDNSDB::SetLastChangeHeight() {
    int storedValue;

    if (Read(std::make_pair(DB_INTERNAL, db_height), storedValue))
        if (Write(std::make_pair(DB_INTERNAL, db_last_change), storedValue))
            return true;

    // failling to set the last change height correctly leads to corruption
    WriteCorruptionState(true);
    return false;
}

// if the new height is smaller than the height of the last recorded change that means we're dealing with a BDNS index corruption
bool CBDNSDB::SetHeight(const int &nHeight) {
    int prevHeight;

    if (Read(std::make_pair(DB_INTERNAL, db_height), prevHeight)) {
        // if certain heights were skipped that implies a possible corruption
        if (prevHeight != db_default_height && nHeight != db_default_height && !(nHeight == (prevHeight + 1) || nHeight == (prevHeight - 1)))
            WriteCorruptionState(true);
    }

    if (nHeight < GetLastChangeHeight())
        WriteCorruptionState(true);

    if (Write(std::make_pair(DB_INTERNAL, db_height), nHeight))
        return true;
    
    // failling to set the height correctly leads to corruption
    WriteCorruptionState(true);
    return false;
}

bool CBDNSDB::WriteCorruptionState(bool fPossibleCorruption) {
    if (fPossibleCorruption)
        return Write(std::make_pair(DB_INTERNAL, db_corruption), 1);
    else
        return Erase(std::make_pair(DB_INTERNAL, db_corruption));
}

bool CBDNSDB::PossibleCorruption() {
    return Exists(std::make_pair(DB_INTERNAL, db_corruption));
}

bool CBDNSDB::WriteReindexing(bool fReindexing) {
    if (fReindexing)
        return Write(std::make_pair(DB_INTERNAL, db_reindexing), 1);
    else
        return Erase(std::make_pair(DB_INTERNAL, db_reindexing));
}

bool CBDNSDB::AwaitsReindexing() {
    return Exists(std::make_pair(DB_INTERNAL, db_reindexing));
}
