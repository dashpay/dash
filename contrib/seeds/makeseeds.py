#!/usr/bin/env python3
# Copyright (c) 2013-2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Generate seeds.txt from "protx list valid 1"
# then create onion_seeds.txt and add some active onion services to it; check tor.md for some
#

import argparse
import collections
import ipaddress
import re
import sys
import json
from typing import List, Dict, Union

from asmap import ASMap, net_to_prefix

NSEEDS=512

MAX_SEEDS_PER_ASN=4

MAX_SEEDS_PER_ASN = {
    'ipv4': 4,
    'ipv6': 10,
}

MIN_BLOCKS = 2_300_000

PATTERN_IPV4 = re.compile(r"^((\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})):(\d+)$")
PATTERN_IPV6 = re.compile(r"^\[([0-9a-z:]+)\]:(\d+)$")
PATTERN_ONION = re.compile(r"^([a-z2-7]{56}\.onion):(\d+)$")
PATTERN_I2P = re.compile(r"^([a-z2-7]{52}\.b32.i2p):(\d+)$")


def parseip(ip_in: str) -> Union[dict, None]:
    """ Parses a line from `seeds_main.txt` into a dictionary of details for that line.
    or `None`, if the line could not be parsed.
    """
    m = PATTERN_IPV4.match(ip_in)
    ip = None
    if m is None:
        m = PATTERN_IPV6.match(ip_in)
        if m is None:
            m = PATTERN_ONION.match(ip_in)
            if m is None:
                m = PATTERN_I2P.match(ip_in)
                if m is None:
                    return None
                else:
                    net = 'i2p'
                    ipstr = sortkey = m.group(1)
                    port = int(m.group(2))
            else:
                net = 'onion'
                ipstr = sortkey = m.group(1)
                port = int(m.group(2))
        else:
            net = 'ipv6'
            if m.group(1) in ['::']: # Not interested in localhost
                return None
            ipstr = m.group(1)
            sortkey = ipstr # XXX parse IPv6 into number, could use name_to_ipv6 from generate-seeds
            port = int(m.group(2))
    else:
        # Do IPv4 sanity check
        ip = 0
        for i in range(0,4):
            if int(m.group(i+2)) < 0 or int(m.group(i+2)) > 255:
                return None
            ip = ip + (int(m.group(i+2)) << (8*(3-i)))
        if ip == 0:
            return None
        net = 'ipv4'
        sortkey = ip
        ipstr = m.group(1)
        port = int(m.group(6))

    return {
        "net": net,
        "ip": ipstr,
        "port": port,
        "ipnum": ip,
        "sortkey": sortkey
    }

def filtermulticollateralhash(mns : List[Dict]) -> List[Dict]:
    '''Filter out MNs sharing the same collateral hash'''
    hist = collections.defaultdict(list)
    for mn in mns:
        hist[mn['collateralHash']].append(mn)
    return [mn for mn in mns if len(hist[mn['collateralHash']]) == 1]

def filtermulticollateraladdress(mns : List[Dict]) -> List[Dict]:
    '''Filter out MNs sharing the same collateral address'''
    hist = collections.defaultdict(list)
    for mn in mns:
        hist[mn['collateralAddress']].append(mn)
    return [mn for mn in mns if len(hist[mn['collateralAddress']]) == 1]

def filtermultipayoutaddress(mns : List[Dict]) -> List[Dict]:
    '''Filter out MNs sharing the same payout address'''
    hist = collections.defaultdict(list)
    for mn in mns:
        hist[mn['state']['payoutAddress']].append(mn)
    return [mn for mn in mns if len(hist[mn['state']['payoutAddress']]) == 1]

# Based on Greg Maxwell's seed_filter.py
def filterbyasn(asmap: ASMap, ips: List[Dict], max_per_asn: Dict, max_per_net: int) -> List[Dict]:
    """ Prunes `ips` by
    (a) trimming ips to have at most `max_per_net` ips from each net (e.g. ipv4, ipv6); and
    (b) trimming ips to have at most `max_per_asn` ips from each asn in each net.
    """
    # Sift out ips by type
    ips_ipv46 = [ip for ip in ips if ip['net'] in ['ipv4', 'ipv6']]
    ips_onion = [ip for ip in ips if ip['net'] == 'onion']
    ips_i2p = [ip for ip in ips if ip['net'] == 'i2p']

    # Filter IPv46 by ASN, and limit to max_per_net per network
    result = []
    net_count: Dict[str, int] = collections.defaultdict(int)
    asn_count: Dict[int, int] = collections.defaultdict(int)

    for i, ip in enumerate(ips_ipv46):
        if net_count[ip['net']] == max_per_net:
            # do not add this ip as we already too many
            # ips from this network
            continue
        asn = asmap.lookup(net_to_prefix(ipaddress.ip_network(ip['ip'])))
        if not asn or asn_count[ip['net'], asn] == max_per_asn[ip['net']]:
            # do not add this ip as we already have too many
            # ips from this ASN on this network
            continue
        asn_count[ip['net'], asn] += 1
        net_count[ip['net']] += 1
        ip['asn'] = asn
        result.append(ip)

    # Add back Onions (up to max_per_net)
    result.extend(ips_onion[0:max_per_net])
    result.extend(ips_i2p[0:max_per_net])
    return result

def ip_stats(ips: List[Dict]) -> str:
    """ Format and return pretty string from `ips`. """
    hist: Dict[str, int] = collections.defaultdict(int)
    for ip in ips:
        if ip is not None:
            hist[ip['net']] += 1

    return f"{hist['ipv4']:6d} {hist['ipv6']:6d} {hist['onion']:6d} {hist['i2p']:6d}"

def parse_args():
    argparser = argparse.ArgumentParser(description='Generate a list of dash node seed ip addresses.')
    argparser.add_argument("-a","--asmap", help='the location of the asmap asn database file (required)', required=True)
    argparser.add_argument("protx_list", nargs='?', help='json as outputted by "protx list valid 1" (default: stdin)')
    argparser.add_argument("onion_seeds", nargs='?', help='file with onion seeds, one per line')
    return argparser.parse_args()

def main():
    args = parse_args()

    print(f'Loading asmap database "{args.asmap}"…', end='', file=sys.stderr, flush=True)
    with open(args.asmap, 'rb') as f:
        asmap = ASMap.from_binary(f.read())
    print('Done.', file=sys.stderr)

    # This expects a json as outputted by "protx list valid 1"
    if args.protx_list is not None:
        with open(args.protx_list, 'r', encoding="utf8") as f:
            mns = json.load(f)
    else:
        mns = json.load(sys.stdin)

    onions = []
    if args.onion_seeds is not None:
        with open(args.onion_seeds, 'r', encoding="utf8") as f:
            onions = f.read().split('\n')

    print(f'Total mns: {len(mns)}', file=sys.stderr)
    # Skip PoSe banned MNs
    mns = [mn for mn in mns if mn['state']['PoSeBanHeight'] == -1]
    print(f'After skip entries from PoSe banned MNs: {len(mns)}', file=sys.stderr)
    # Skip MNs with < 10000 confirmations
    mns = [mn for mn in mns if mn['confirmations'] >= 10000]
    print(f'After skip MNs with less than 10000 confirmations: {len(mns)}', file=sys.stderr)

    # Filter out MNs which are definitely from the same person/operator
    mns = filtermulticollateralhash(mns)
    mns = filtermulticollateraladdress(mns)
    mns = filtermultipayoutaddress(mns)
    print(f'After removing duplicates: {len(mns)}', file=sys.stderr)

    # Extract IPs
    ips = [parseip(mn['state']['addresses']['core_p2p'][0]) for mn in mns]
    for onion in onions:
        parsed = parseip(onion)
        if parsed is not None:
            ips.append(parsed)

    print('\x1b[7m  IPv4   IPv6  Onion  I2P Pass                                               \x1b[0m', file=sys.stderr)
    print(f'{ip_stats(ips):s} Initial', file=sys.stderr)
    # Skip entries with invalid address.
    ips = [ip for ip in ips if ip is not None]
    print(f'{ip_stats(ips):s} Skip entries with invalid address', file=sys.stderr)

    # Look up ASNs and limit results, both per ASN and globally.
    ips = filterbyasn(asmap, ips, MAX_SEEDS_PER_ASN, NSEEDS)
    print(f'{ip_stats(ips):s} Look up ASNs and limit results per ASN and per net', file=sys.stderr)
    # Sort the results by IP address (for deterministic output).
    ips.sort(key=lambda x: (x['net'], x['sortkey']), reverse=True)

    for ip in ips:
        if ip['net'] == 'ipv6':
            print(f"[{ip['ip']}]:{ip['port']}", end="")
        else:
            print(f"{ip['ip']}:{ip['port']}", end="")
        if 'asn' in ip:
            print(f" # AS{ip['asn']}", end="")
        print()

if __name__ == '__main__':
    main()
