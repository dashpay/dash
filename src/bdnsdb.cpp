// Copyright (c) 2020-2021 Alterdot Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "bdnsdb.h"

#include "util.h"

static const char DB_DOMAIN = 'd';
static const char DB_VERSION = 'V';
static const int db_version = 1;

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
    return Write(std::make_pair(DB_DOMAIN, bdnsName), bdnsRecord);
}

bool CBDNSDB::UpdateBDNSRecord(const std::string &bdnsName, const std::string &content, const uint256 &updateTxid) {
    BDNSRecord storedValue;

    if (Read(std::make_pair(DB_DOMAIN, bdnsName), storedValue)) {
        storedValue.content = content;
        storedValue.lastUpdateTxid = updateTxid;
        
        return Write(std::make_pair(DB_DOMAIN, bdnsName), storedValue);
    }

    return false;
}

bool CBDNSDB::EraseBDNSRecord(const std::string &bdnsName) {
    return Erase(std::make_pair(DB_DOMAIN, bdnsName));
}

// clears all records in the database, old and new format and writes the DB version
bool CBDNSDB::ClearRecords() {
    std::unique_ptr<CDBIterator> pcursor(NewIterator());
    size_t batch_size = 1 << 20;
    CDBBatch batch(*this);

    pcursor->SeekToFirst();

    if (!CheckVersion()) {
        std::string oldKey;

        while (pcursor->Valid()) {
            if (pcursor->GetKey(oldKey))
                batch.Erase(oldKey);
            
            if (batch.SizeEstimate() > batch_size) {
                WriteBatch(batch);
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
                WriteBatch(batch);
                batch.Clear();
            }

            pcursor->Next();
        }
    }

    WriteBatch(batch);
    CompactFull();
    WriteVersion();
}

bool CBDNSDB::CheckVersion() {
    int storedValue;

    if (Read(DB_VERSION, storedValue)) {
        return storedValue == db_version;
    }

    return false;
}

bool CBDNSDB::WriteVersion() {
    return Write(DB_VERSION, db_version);
}
