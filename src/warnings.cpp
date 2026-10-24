// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <warnings.h>

#include <sync.h>
#include <util/string.h>
#include <util/system.h>
#include <util/translation.h>
#include <hash.h>

#include <atomic>
#include <map>
#include <vector>

static GlobalMutex g_warnings_mutex;
static bilingual_str g_misc_warnings GUARDED_BY(g_warnings_mutex);
static bool fLargeWorkInvalidChainFound GUARDED_BY(g_warnings_mutex) = false;
static std::map<std::string, bilingual_str> g_evo_state_warnings GUARDED_BY(g_warnings_mutex);

void SetMiscWarning(const bilingual_str& warning)
{
    LOCK(g_warnings_mutex);
    g_misc_warnings = warning;
}

//! Whether any snapshot-state warning is set, so clearing one is free while none is.
static std::atomic<bool> g_any_evo_state_warning{false};

bool SetEvoStateWarning(const std::string& source, const std::optional<bilingual_str>& warning)
{
    if (!warning && !g_any_evo_state_warning.load()) return false;
    LOCK(g_warnings_mutex);
    const bool changed{warning ? (g_evo_state_warnings.insert_or_assign(source, *warning), true)
                               : g_evo_state_warnings.erase(source) > 0};
    g_any_evo_state_warning = !g_evo_state_warnings.empty();
    return changed;
}

void SetfLargeWorkInvalidChainFound(bool flag)
{
    LOCK(g_warnings_mutex);
    fLargeWorkInvalidChainFound = flag;
}

bilingual_str GetWarnings(bool verbose)
{
    bilingual_str warnings_concise;
    std::vector<bilingual_str> warnings_verbose;

    LOCK(g_warnings_mutex);

    // Pre-release build warning
    if (!CLIENT_VERSION_IS_RELEASE) {
        warnings_concise = _("This is a pre-release test build - use at your own risk - do not use for mining or merchant applications");
        warnings_verbose.emplace_back(warnings_concise);
    }

    // Misc warnings like out of disk space and clock is wrong
    if (!g_misc_warnings.empty()) {
        warnings_concise = g_misc_warnings;
        warnings_verbose.emplace_back(warnings_concise);
    }

    for (const auto& [_, warning] : g_evo_state_warnings) {
        warnings_concise = warning;
        warnings_verbose.emplace_back(warning);
    }

    if (fLargeWorkInvalidChainFound) {
        warnings_concise = _("Warning: We do not appear to fully agree with our peers! You may need to upgrade, or other nodes may need to upgrade.");
        warnings_verbose.emplace_back(warnings_concise);
    }

    if (verbose) {
        return Join(warnings_verbose, Untranslated("<hr />"));
    }

    return warnings_concise;
}
