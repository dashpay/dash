// Copyright (c) 2014-2019 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MASTERNODE_UTILS_H
#define MASTERNODE_UTILS_H

#include "evo/deterministicmns.h"

class CConnman;

class CMasternodeUtils
{
public:
    typedef std::pair<arith_uint256, CDeterministicMNCPtr> score_pair_t;
    typedef std::vector<score_pair_t> score_pair_vec_t;
    typedef std::pair<int, const CDeterministicMNCPtr> rank_pair_t;
    typedef std::vector<rank_pair_t> rank_pair_vec_t;

public:
    static void ProcessMasternodeConnections(CConnman& connman);
    static void DoMaintenance(CConnman &connman);
};

#endif//MASTERNODE_UTILS_H
