// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <script/bitcoinconsensus.h>

#include <primitives/transaction.h>
#include <pubkey.h>
#include <script/interpreter.h>
#include <version.h>

namespace {

/** A class that deserializes a single CTransaction one time. */
class TxInputStream
{
public:
    TxInputStream(int nVersionIn, const unsigned char *txTo, size_t txToLen) :
    m_version(nVersionIn),
    m_data(txTo),
    m_remaining(txToLen)
    {}

    void read(Span<std::byte> dst)
    {
        if (dst.size() > m_remaining) {
            throw std::ios_base::failure(std::string(__func__) + ": end of data");
        }

        if (dst.data() == nullptr) {
            throw std::ios_base::failure(std::string(__func__) + ": bad destination buffer");
        }

        if (m_data == nullptr) {
            throw std::ios_base::failure(std::string(__func__) + ": bad source buffer");
        }

        memcpy(dst.data(), m_data, dst.size());
        m_remaining -= dst.size();
        m_data += dst.size();
    }

    template<typename T>
    TxInputStream& operator>>(T&& obj)
    {
        ::Unserialize(*this, obj);
        return *this;
    }

    [[maybe_unused]] int GetVersion() const { return m_version; }
private:
    const int m_version;
    const unsigned char* m_data;
    size_t m_remaining;
};

inline int set_error(dashconsensus_error* ret, dashconsensus_error serror)
{
    if (ret)
        *ret = serror;
    return 0;
}

} // namespace

/** Check that all specified flags are part of the libconsensus interface. */
static bool verify_flags(unsigned int flags)
{
    return (flags & ~(dashconsensus_SCRIPT_FLAGS_VERIFY_ALL)) == 0;
}

int dashconsensus_verify_script(const unsigned char *scriptPubKey, unsigned int scriptPubKeyLen,
                                    const unsigned char *txTo        , unsigned int txToLen,
                                    unsigned int nIn, unsigned int flags, dashconsensus_error* err)
{
    if (!verify_flags(flags)) {
        return set_error(err, dashconsensus_ERR_INVALID_FLAGS);
    }
    try {
        TxInputStream stream(PROTOCOL_VERSION, txTo, txToLen);
        CTransaction tx(deserialize, stream);
        if (nIn >= tx.vin.size())
            return set_error(err, dashconsensus_ERR_TX_INDEX);
        if (GetSerializeSize(tx, PROTOCOL_VERSION) != txToLen)
            return set_error(err, dashconsensus_ERR_TX_SIZE_MISMATCH);

        // Regardless of the verification result, the tx did not error.
        set_error(err, dashconsensus_ERR_OK);

        PrecomputedTransactionData txdata(tx);
		CAmount am(0);
        return VerifyScript(tx.vin[nIn].scriptSig, CScript(scriptPubKey, scriptPubKey + scriptPubKeyLen), flags, TransactionSignatureChecker(&tx, nIn, am, txdata, MissingDataBehavior::FAIL), nullptr);
    } catch (const std::exception&) {
        return set_error(err, dashconsensus_ERR_TX_DESERIALIZE); // Error deserializing
    }
}

unsigned int dashconsensus_version()
{
    // Just use the API version for now
    return BITCOINCONSENSUS_API_VER;
}
