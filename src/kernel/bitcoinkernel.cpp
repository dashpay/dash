// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <stdexcept>
#include <functional>
#include <string>

// Define G_TRANSLATION_FUN symbol in libbitcoinkernel library so users of the
// library aren't required to export this symbol
extern const std::function<std::string(const char*)> G_TRANSLATION_FUN = nullptr;

//----------------
// symbols g_stats_client, ValueFromAmount, GetPrettyExceptionStr are re-defined
// here because adding relevant sources files will pull too many extra dependencies recursively
#include <consensus/amount.h>
#include <stats/client.h>
#include <univalue.h>
std::unique_ptr<StatsdClient> g_stats_client{std::make_unique<StatsdClient>()};

UniValue ValueFromAmount(const CAmount amount)
{
    static_assert(COIN > 1);
    int64_t quotient = amount / COIN;
    int64_t remainder = amount % COIN;
    if (amount < 0) {
        quotient = -quotient;
        remainder = -remainder;
    }
    return UniValue(UniValue::VNUM,
            strprintf("%s%d.%08d", amount < 0 ? "-" : "", quotient, remainder));
}
//////////////////////

std::string GetPrettyExceptionStr(const std::exception_ptr& e)
{
    try {
        // rethrow and catch the exception as there is no other way to reliably cast to the real type (not possible with RTTI)
        std::rethrow_exception(e);
    } catch (const std::exception& e2) {
        return e2.what();
    } catch (...) {
        throw;
    }
}
