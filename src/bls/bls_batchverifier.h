// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DASH_CRYPTO_BLS_BATCHVERIFIER_H
#define DASH_CRYPTO_BLS_BATCHVERIFIER_H

#include <bls/bls.h>

#include <array>
#include <map>
#include <set>
#include <tuple>
#include <vector>

template<typename SourceId, typename MessageId>
class CBLSBatchVerifier
{
private:
    struct Message {
        MessageId msgId;
        uint256 msgHash;
        CBLSSignature sig;
        CBLSPublicKey pubKey;
    };

    using MessageKey = std::tuple<MessageId, uint256, std::array<uint8_t, CBLSSignature::SerSize>,
                                  std::array<uint8_t, CBLSPublicKey::SerSize>>;
    using MessageMap = std::map<MessageKey, Message>;
    using MessageMapIterator = typename MessageMap::iterator;
    using MessagesBySourceMap = std::map<SourceId, std::vector<MessageMapIterator>>;

    bool perMessageFallback;
    size_t subBatchSize;

    MessageMap messages;
    MessagesBySourceMap messagesBySource;

public:
    std::set<SourceId> badSources;
    std::set<MessageId> badMessages;

public:
    // Individual verification is safe in both modes; retain the parameter for existing callers.
    CBLSBatchVerifier(bool /*secureVerification*/, bool _perMessageFallback, size_t _subBatchSize = 0) :
            perMessageFallback(_perMessageFallback),
            subBatchSize(_subBatchSize)
    {
    }

    void PushMessage(const SourceId& sourceId, const MessageId& msgId, const uint256& msgHash, const CBLSSignature& sig, const CBLSPublicKey& pubKey)
    {
        assert(sig.IsValid() && pubKey.IsValid());

        // Only identical verification inputs may share a verdict.
        const MessageKey key{msgId, msgHash, sig.ToBytes(false), pubKey.ToBytes(false)};
        auto it = messages.emplace(key, Message{msgId, msgHash, sig, pubKey}).first;
        messagesBySource[sourceId].emplace_back(it);

        if (subBatchSize != 0 && messages.size() >= subBatchSize) {
            Verify();
            ClearMessages();
        }
    }

    void ClearMessages()
    {
        messages.clear();
        messagesBySource.clear();
    }

    size_t GetUniqueSourceCount() const
    {
        return messagesBySource.size();
    }

    void Verify()
    {
        // Aggregate validity does not authenticate the individual signatures retained by callers.
        const bool legacy_scheme = bls::bls_legacy_scheme.load();
        std::set<const Message*> invalid_messages;
        for (const auto& [key, msg] : messages) {
            if (!msg.sig.VerifyInsecure(msg.pubKey, msg.msgHash, legacy_scheme)) {
                invalid_messages.emplace(&msg);
            }
        }

        for (const auto& [source, source_messages] : messagesBySource) {
            for (const auto& msg_it : source_messages) {
                const auto& msg = msg_it->second;
                if (invalid_messages.count(&msg) == 0) {
                    continue;
                }
                badSources.emplace(source);
                if (perMessageFallback) {
                    badMessages.emplace(msg.msgId);
                }
            }
        }
    }
};

#endif //DASH_CRYPTO_BLS_BATCHVERIFIER_H
