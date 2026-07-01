// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_PROVIDERTX_H
#define BITCOIN_EVO_PROVIDERTX_H

#include <bls/bls.h>
#include <evo/dmn_types.h>
#include <evo/netinfo.h>
#include <evo/specialtx.h>
#include <evo/types.h>
#include <primitives/transaction.h>
#include <util/std23.h>

#include <consensus/validation.h>
#include <key_io.h>
#include <netaddress.h>
#include <pubkey.h>

#include <gsl/pointers.h>
#include <univalue.h>

#include <vector>

class TxValidationState;
struct RPCResult;

class MasternodePayoutShare
{
public:
    static constexpr uint16_t MIN_REWARD = 100;
    static constexpr uint16_t MAX_REWARD = 10000;

    CScript scriptPayout;
    uint16_t reward{MAX_REWARD};

    SERIALIZE_METHODS(MasternodePayoutShare, obj)
    {
        READWRITE(obj.scriptPayout, obj.reward);
    }

    bool operator==(const MasternodePayoutShare& b) const = default;
    bool operator!=(const MasternodePayoutShare& b) const = default;
};

using MasternodePayoutShares = std::vector<MasternodePayoutShare>;

[[nodiscard]] MasternodePayoutShares LegacyPayoutAsList(const CScript& script_payout);
template<class T>
[[nodiscard]] MasternodePayoutShares GetOwnerPayouts(const T& protx);

[[nodiscard]] bool IsPayoutListTriviallyValid(const MasternodePayoutShares& payouts, const CKeyID& keyIDOwner,
                                              const CKeyID& keyIDVoting, TxValidationState& state);
[[nodiscard]] bool IsPayoutListKeySafe(const MasternodePayoutShares& payouts, const CTxDestination& collateral_dest,
                                       const CKeyID& keyIDOwner, const CKeyID& keyIDVoting,
                                       bool check_payout_collateral_reuse, TxValidationState& state);
[[nodiscard]] std::string PayoutListToString(const MasternodePayoutShares& payouts);
[[nodiscard]] UniValue PayoutListToJson(const MasternodePayoutShares& payouts);

class CProRegTx
{
public:
    static constexpr auto SPECIALTX_TYPE = TRANSACTION_PROVIDER_REGISTER;

    uint16_t nVersion{ProTxVersion::LegacyBLS}; // message version
    MnType nType{MnType::Regular};
    uint16_t nMode{0};                                     // only 0 supported for now
    COutPoint collateralOutpoint{uint256(), (uint32_t)-1}; // if hash is null, we refer to a ProRegTx output
    std::shared_ptr<NetInfoInterface> netInfo{nullptr};
    uint160 platformNodeID{};
    uint16_t platformP2PPort{0};
    uint16_t platformHTTPPort{0};
    CKeyID keyIDOwner;
    CBLSLazyPublicKey pubKeyOperator;
    CKeyID keyIDVoting;
    uint16_t nOperatorReward{0};
    CScript scriptPayout;
    MasternodePayoutShares payouts;
    uint256 inputsHash; // replay protection
    std::vector<unsigned char> vchSig;

    SERIALIZE_METHODS(CProRegTx, obj)
    {
        READWRITE(
                obj.nVersion
        );
        if (obj.nVersion == 0 ||
            obj.nVersion > ProTxVersion::GetMax(/*is_basic_scheme_active=*/true, /*is_extended_addr=*/true)) {
            // unknown version, bail out early
            return;
        }

        READWRITE(
                obj.nType,
                obj.nMode,
                obj.collateralOutpoint,
                NetInfoSerWrapper(const_cast<std::shared_ptr<NetInfoInterface>&>(obj.netInfo),
                                  obj.nVersion >= ProTxVersion::ExtAddr),
                obj.keyIDOwner,
                CBLSLazyPublicKeyVersionWrapper(const_cast<CBLSLazyPublicKey&>(obj.pubKeyOperator), (obj.nVersion == ProTxVersion::LegacyBLS)),
                obj.keyIDVoting,
                obj.nOperatorReward
        );
        if (obj.nVersion >= ProTxVersion::ExtAddr) {
            uint8_t payouts_count{0};
            SER_WRITE(obj, payouts_count = static_cast<uint8_t>(obj.payouts.size()));
            READWRITE(payouts_count);
            SER_READ(obj, obj.payouts.resize(payouts_count));
            for (auto& payout : obj.payouts) {
                READWRITE(payout);
            }
        } else {
            READWRITE(obj.scriptPayout);
        }
        READWRITE(
                obj.inputsHash
        );
        if (obj.nType == MnType::Evo) {
            READWRITE(
                obj.platformNodeID);
            if (obj.nVersion < ProTxVersion::ExtAddr) {
                READWRITE(
                obj.platformP2PPort,
                obj.platformHTTPPort);
            }
        }
        if (!(s.GetType() & SER_GETHASH)) {
            READWRITE(obj.vchSig);
        }
    }

    // When signing with the collateral key, we don't sign the hash but a generated message instead
    // This is needed for HW wallet support which can only sign text messages as of now
    std::string MakeSignString() const;

    std::string ToString() const;

    [[nodiscard]] static RPCResult GetJsonHelp(const std::string& key, bool optional);
    [[nodiscard]] UniValue ToJson() const;

    /**
     * Note: this check validates only some trivial consensus rules
     * Use `CheckProRegTx` or GetValidatedPayload<T> helper for full validation
     */
    bool IsTriviallyValid(TxValidationState& state) const;
};

class CProUpServTx
{
public:
    static constexpr auto SPECIALTX_TYPE = TRANSACTION_PROVIDER_UPDATE_SERVICE;

    uint16_t nVersion{ProTxVersion::LegacyBLS}; // message version
    MnType nType{MnType::Regular};
    uint256 proTxHash;
    std::shared_ptr<NetInfoInterface> netInfo{nullptr};
    uint160 platformNodeID{};
    uint16_t platformP2PPort{0};
    uint16_t platformHTTPPort{0};
    CScript scriptOperatorPayout;
    uint256 inputsHash; // replay protection
    CBLSSignature sig;

    SERIALIZE_METHODS(CProUpServTx, obj)
    {
        READWRITE(
                obj.nVersion
        );
        if (obj.nVersion == 0 ||
            obj.nVersion > ProTxVersion::GetMax(/*is_basic_scheme_active=*/true, /*is_extended_addr=*/true)) {
            // unknown version, bail out early
            return;
        }
        if (obj.nVersion >= ProTxVersion::BasicBLS) {
            READWRITE(
                obj.nType);
        }
        READWRITE(
                obj.proTxHash,
                NetInfoSerWrapper(const_cast<std::shared_ptr<NetInfoInterface>&>(obj.netInfo),
                                  obj.nVersion >= ProTxVersion::ExtAddr),
                obj.scriptOperatorPayout,
                obj.inputsHash
        );
        if (obj.nType == MnType::Evo) {
            READWRITE(
                obj.platformNodeID);
            if (obj.nVersion < ProTxVersion::ExtAddr) {
                READWRITE(
                obj.platformP2PPort,
                obj.platformHTTPPort);
            }
        }
        if (!(s.GetType() & SER_GETHASH)) {
            READWRITE(
                    CBLSSignatureVersionWrapper(const_cast<CBLSSignature&>(obj.sig), (obj.nVersion == ProTxVersion::LegacyBLS))
            );
        }
    }

    std::string ToString() const;

    [[nodiscard]] static RPCResult GetJsonHelp(const std::string& key, bool optional);
    [[nodiscard]] UniValue ToJson() const;

    /**
     * Note: this check validates only some trivial consensus rules
     * Use `CheckProUpServTx` or GetValidatedPayload<T> helper for full validation
     */
    bool IsTriviallyValid(TxValidationState& state) const;
};

class CProUpRegTx
{
public:
    static constexpr auto SPECIALTX_TYPE = TRANSACTION_PROVIDER_UPDATE_REGISTRAR;

    uint16_t nVersion{ProTxVersion::LegacyBLS}; // message version
    uint256 proTxHash;
    uint16_t nMode{0}; // only 0 supported for now
    CBLSLazyPublicKey pubKeyOperator;
    CKeyID keyIDVoting;
    CScript scriptPayout;
    MasternodePayoutShares payouts;
    uint256 inputsHash; // replay protection
    std::vector<unsigned char> vchSig;

    SERIALIZE_METHODS(CProUpRegTx, obj)
    {
        READWRITE(
                obj.nVersion
        );
        if (obj.nVersion == 0 ||
            obj.nVersion > ProTxVersion::GetMax(/*is_basic_scheme_active=*/true, /*is_extended_addr=*/true)) {
            // unknown version, bail out early
            return;
        }
        READWRITE(
                obj.proTxHash,
                obj.nMode,
                CBLSLazyPublicKeyVersionWrapper(const_cast<CBLSLazyPublicKey&>(obj.pubKeyOperator), (obj.nVersion == ProTxVersion::LegacyBLS)),
                obj.keyIDVoting
        );
        if (obj.nVersion >= ProTxVersion::ExtAddr) {
            uint8_t payouts_count{0};
            SER_WRITE(obj, payouts_count = static_cast<uint8_t>(obj.payouts.size()));
            READWRITE(payouts_count);
            SER_READ(obj, obj.payouts.resize(payouts_count));
            for (auto& payout : obj.payouts) {
                READWRITE(payout);
            }
        } else {
            READWRITE(obj.scriptPayout);
        }
        READWRITE(
                obj.inputsHash
        );
        if (!(s.GetType() & SER_GETHASH)) {
            READWRITE(
                    obj.vchSig
            );
        }
    }

    std::string ToString() const;

    [[nodiscard]] static RPCResult GetJsonHelp(const std::string& key, bool optional);
    [[nodiscard]] UniValue ToJson() const;

    /**
     * Note: this check validates only some trivial consensus rules
     * Use `CheckProUpRegTx` or GetValidatedPayload<T> helper for full validation
     */
    bool IsTriviallyValid(TxValidationState& state) const;
};

class CProUpRevTx
{
public:
    static constexpr auto SPECIALTX_TYPE = TRANSACTION_PROVIDER_UPDATE_REVOKE;

    // these are just informational and do not have any effect on the revocation
    enum {
        REASON_NOT_SPECIFIED = 0,
        REASON_TERMINATION_OF_SERVICE = 1,
        REASON_COMPROMISED_KEYS = 2,
        REASON_CHANGE_OF_KEYS = 3,
        REASON_LAST = REASON_CHANGE_OF_KEYS
    };

    uint16_t nVersion{ProTxVersion::LegacyBLS}; // message version
    uint256 proTxHash;
    uint16_t nReason{REASON_NOT_SPECIFIED};
    uint256 inputsHash; // replay protection
    CBLSSignature sig;

    SERIALIZE_METHODS(CProUpRevTx, obj)
    {
        READWRITE(
                obj.nVersion
        );
        if (obj.nVersion == 0 ||
            obj.nVersion > ProTxVersion::GetMax(/*is_basic_scheme_active=*/true, /*is_extended_addr=*/true)) {
            // unknown version, bail out early
            return;
        }
        READWRITE(
                obj.proTxHash,
                obj.nReason,
                obj.inputsHash
        );
        if (!(s.GetType() & SER_GETHASH)) {
            READWRITE(
                    CBLSSignatureVersionWrapper(const_cast<CBLSSignature&>(obj.sig), (obj.nVersion == ProTxVersion::LegacyBLS))
            );
        }
    }

    std::string ToString() const;

    [[nodiscard]] static RPCResult GetJsonHelp(const std::string& key, bool optional);
    [[nodiscard]] UniValue ToJson() const;

    /**
     * Note: this check validates only some trivial consensus rules
     * Use `CheckProUpRevTx` or GetValidatedPayload<T> helper for full validation
     */
    bool IsTriviallyValid(TxValidationState& state) const;
};

template <typename ProTx>
static bool CheckInputsHash(const CTransaction& tx, const ProTx& proTx, TxValidationState& state)
{
    if (uint256 inputsHash = CalcTxInputsHash(tx); inputsHash != proTx.inputsHash) {
        return state.Invalid(TxValidationResult::TX_CONSENSUS, "bad-protx-inputs-hash");
    }
    return true;
}

#endif // BITCOIN_EVO_PROVIDERTX_H
