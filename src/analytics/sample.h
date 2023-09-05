// Copyright (c) 2014-2023 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ANALYTICS_SAMPLE_H
#define BITCOIN_ANALYTICS_SAMPLE_H

class ArgsManager;
class CTxMemPool;
namespace Statsd {
class StatsdClient;
} /* namespace Statsd */

void SampleStats(const Statsd::StatsdClient& client, const ArgsManager* args, const CTxMemPool* mempool);

#endif // BITCOIN_ANALYTICS_SAMPLE_H
