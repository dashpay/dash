// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <governance/validators.h>

#include <governance/common.h>
#include <key_io.h>
#include <timedata.h>
#include <tinyformat.h>
#include <util/std23.h>
#include <util/strencodings.h>

#include <univalue.h>

#include <algorithm>

namespace {

constexpr size_t MAX_DATA_SIZE = 512;
constexpr size_t MAX_NAME_SIZE = 40;

bool GetDataValue(const UniValue& objJSON, const std::string& strKey, std::string& strValueRet, std::string& strErrorMessages)
{
    try {
        strValueRet = objJSON[strKey].get_str();
        return true;
    } catch (std::exception& e) {
        strErrorMessages += std::string(e.what()) + std::string(";");
    } catch (...) {
        strErrorMessages += "Unknown exception;";
    }
    return false;
}

bool GetDataValue(const UniValue& objJSON, const std::string& strKey, int64_t& nValueRet, std::string& strErrorMessages)
{
    try {
        const UniValue& uValue = objJSON[strKey];
        if (uValue.getType() == UniValue::VNUM) {
            nValueRet = uValue.getInt<int64_t>();
            return true;
        }
    } catch (std::exception& e) {
        strErrorMessages += std::string(e.what()) + std::string(";");
    } catch (...) {
        strErrorMessages += "Unknown exception;";
    }
    return false;
}

bool GetDataValue(const UniValue& objJSON, const std::string& strKey, double& dValueRet, std::string& strErrorMessages)
{
    try {
        const UniValue& uValue = objJSON[strKey];
        if (uValue.getType() == UniValue::VNUM) {
            dValueRet = uValue.get_real();
            return true;
        }
    } catch (std::exception& e) {
        strErrorMessages += std::string(e.what()) + std::string(";");
    } catch (...) {
        strErrorMessages += "Unknown exception;";
    }
    return false;
}

bool ValidateType(const UniValue& objJSON, std::string& strErrorMessages)
{
    int64_t nType;
    if (!GetDataValue(objJSON, "type", nType, strErrorMessages)) {
        strErrorMessages += "type field not found;";
        return false;
    }

    if (nType != std23::to_underlying(GovernanceObject::PROPOSAL)) {
        strErrorMessages += strprintf("type is not %d;", std23::to_underlying(GovernanceObject::PROPOSAL));
        return false;
    }

    return true;
}

bool ValidateName(const UniValue& objJSON, std::string& strErrorMessages)
{
    std::string strName;
    if (!GetDataValue(objJSON, "name", strName, strErrorMessages)) {
        strErrorMessages += "name field not found;";
        return false;
    }

    if (strName.size() > MAX_NAME_SIZE) {
        strErrorMessages += strprintf("name exceeds %lu characters;", MAX_NAME_SIZE);
        return false;
    }

    if (strName.empty()) {
        strErrorMessages += "name cannot be empty;";
        return false;
    }

    static constexpr std::string_view strAllowedChars{"-_abcdefghijklmnopqrstuvwxyz0123456789"};

    strName = ToLower(strName);

    if (strName.find_first_not_of(strAllowedChars) != std::string::npos) {
        strErrorMessages += "name contains invalid characters;";
        return false;
    }

    return true;
}

bool ValidateStartEndEpoch(const UniValue& objJSON, bool fCheckExpiration, std::string& strErrorMessages)
{
    int64_t nStartEpoch = 0;
    int64_t nEndEpoch = 0;

    if (!GetDataValue(objJSON, "start_epoch", nStartEpoch, strErrorMessages)) {
        strErrorMessages += "start_epoch field not found;";
        return false;
    }

    if (!GetDataValue(objJSON, "end_epoch", nEndEpoch, strErrorMessages)) {
        strErrorMessages += "end_epoch field not found;";
        return false;
    }

    if (nEndEpoch <= nStartEpoch) {
        strErrorMessages += "end_epoch <= start_epoch;";
        return false;
    }

    if (fCheckExpiration && nEndEpoch <= GetAdjustedTime()) {
        strErrorMessages += "expired;";
        return false;
    }

    return true;
}

bool ValidatePaymentAmount(const UniValue& objJSON, std::string& strErrorMessages)
{
    double dValue = 0.0;

    if (!GetDataValue(objJSON, "payment_amount", dValue, strErrorMessages)) {
        strErrorMessages += "payment_amount field not found;";
        return false;
    }

    if (dValue <= 0.0) {
        strErrorMessages += "payment_amount is negative;";
        return false;
    }

    // TODO: Should check for an amount which exceeds the budget but this is
    // currently difficult because start and end epochs are defined in terms of
    // clock time instead of block height.

    return true;
}

bool ValidatePaymentAddress(const UniValue& objJSON, bool fAllowScript, std::string& strErrorMessages)
{
    std::string strPaymentAddress;

    if (!GetDataValue(objJSON, "payment_address", strPaymentAddress, strErrorMessages)) {
        strErrorMessages += "payment_address field not found;";
        return false;
    }

    if (std::find_if(strPaymentAddress.begin(), strPaymentAddress.end(), IsSpace) != strPaymentAddress.end()) {
        strErrorMessages += "payment_address can't have whitespaces;";
        return false;
    }

    CTxDestination dest = DecodeDestination(strPaymentAddress);
    if (!IsValidDestination(dest)) {
        strErrorMessages += "payment_address is invalid;";
        return false;
    }

    const ScriptHash *scriptID = std::get_if<ScriptHash>(&dest);
    if (!fAllowScript && scriptID) {
        strErrorMessages += "script addresses are not supported;";
        return false;
    }

    return true;
}

/*
  The purpose of this function is to replicate the behavior of the
  Python urlparse function used by sentinel (urlparse.py).  This function
  should return false whenever urlparse raises an exception and true
  otherwise.
 */
bool CheckURL(const std::string& strURLIn)
{
    std::string strRest(strURLIn);
    std::string::size_type nPos = strRest.find(':');

    if (nPos != std::string::npos) {
        if (nPos < strRest.size()) {
            strRest = strRest.substr(nPos + 1);
        } else {
            strRest = "";
        }
    }

    // Process netloc
    if ((strRest.size() > 2) && (strRest.substr(0, 2) == "//")) {
        static constexpr std::string_view strNetlocDelimiters{"/?#"};

        strRest = strRest.substr(2);

        std::string::size_type nPos2 = strRest.find_first_of(strNetlocDelimiters);

        std::string strNetloc = strRest.substr(0, nPos2);

        if ((strNetloc.find('[') != std::string::npos) && (strNetloc.find(']') == std::string::npos)) {
            return false;
        }

        if ((strNetloc.find(']') != std::string::npos) && (strNetloc.find('[') == std::string::npos)) {
            return false;
        }
    }

    return true;
}

bool ValidateURL(const UniValue& objJSON, std::string& strErrorMessages)
{
    std::string strURL;
    if (!GetDataValue(objJSON, "url", strURL, strErrorMessages)) {
        strErrorMessages += "url field not found;";
        return false;
    }

    if (std::find_if(strURL.begin(), strURL.end(), IsSpace) != strURL.end()) {
        strErrorMessages += "url can't have whitespaces;";
        return false;
    }

    if (strURL.size() < 4U) {
        strErrorMessages += "url too short;";
        return false;
    }

    if (!CheckURL(strURL)) {
        strErrorMessages += "url invalid;";
        return false;
    }

    return true;
}

bool ParseProposalJSON(const std::string& strHexData, UniValue& objJSONOut, std::string& strErrorMessages)
{
    if (strHexData.empty()) return false;

    std::vector<unsigned char> v = ParseHex(strHexData);
    if (v.size() > MAX_DATA_SIZE) {
        strErrorMessages = strprintf("data exceeds %lu characters;", MAX_DATA_SIZE);
        return false;
    }

    const std::string strJSONData(v.begin(), v.end());
    if (strJSONData.empty()) return false;

    try {
        UniValue obj(UniValue::VOBJ);
        obj.read(strJSONData);
        if (!obj.isObject()) {
            throw std::runtime_error("Proposal must be a JSON object");
        }
        objJSONOut = obj;
        return true;
    } catch (std::exception& e) {
        strErrorMessages += std::string(e.what()) + std::string(";");
    } catch (...) {
        strErrorMessages += "Unknown exception;";
    }
    return false;
}

} // namespace

bool ValidateProposal(const std::string& strDataHex, std::string& strErrorOut, bool fCheckExpiration, bool fAllowScript)
{
    UniValue objJSON(UniValue::VOBJ);
    if (!ParseProposalJSON(strDataHex, objJSON, strErrorOut)) {
        strErrorOut += "JSON parsing error;";
        return false;
    }
    if (!ValidateType(objJSON, strErrorOut)) {
        strErrorOut += "Invalid type;";
        return false;
    }
    if (!ValidateName(objJSON, strErrorOut)) {
        strErrorOut += "Invalid name;";
        return false;
    }
    if (!ValidateStartEndEpoch(objJSON, fCheckExpiration, strErrorOut)) {
        strErrorOut += "Invalid start:end range;";
        return false;
    }
    if (!ValidatePaymentAmount(objJSON, strErrorOut)) {
        strErrorOut += "Invalid payment amount;";
        return false;
    }
    if (!ValidatePaymentAddress(objJSON, fAllowScript, strErrorOut)) {
        strErrorOut += "Invalid payment address;";
        return false;
    }
    if (!ValidateURL(objJSON, strErrorOut)) {
        strErrorOut += "Invalid URL;";
        return false;
    }
    return true;
}
