#!/usr/bin/env python3
# Copyright (c) 2018-2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import os
import sys
import argparse
import json

def perform_pre_checks():
    mock_result_path = os.path.join(os.getcwd(), "mock_result")
    if os.path.isfile(mock_result_path):
        with open(mock_result_path, "r", encoding="utf8") as f:
            mock_result = f.read()
        if mock_result[0]:
            sys.stdout.write(mock_result[2:])
            sys.exit(int(mock_result[0]))

def enumerate(args):
    sys.stdout.write(json.dumps([{"fingerprint": "00000001", "type": "trezor", "model": "trezor_t"}]))

def getdescriptors(args):
    xpub = "tpubD6NzVbkrYhZ4WaWSyoBvQwbpLkojyoTZPRsgXELWz3Popb3qkjcJyJUGLnL4qHHoQvao8ESaAstxYSnhyswJ76uZPStJRJCTKvosUCJZL5B"

    legacy = os.path.isfile(os.path.join(os.getcwd(), "mock_legacy_markers"))
    marker = "'" if legacy else "h"
    receive_checksum = "vt6w3l3j" if legacy else "aqllu46s"
    internal_checksum = "all0v2p2" if legacy else "v567pq2g"
    origin = f"[00000001/44{marker}/1{marker}/{args.account}']"
    sys.stdout.write(json.dumps({
        "receive": [f"pkh({origin}{xpub}/0/*)#{receive_checksum}"],
        "internal": [f"pkh({origin}{xpub}/1/*)#{internal_checksum}"],
    }))


def displayaddress(args):
    # Several descriptor formats are acceptable, so allowing for potential
    # changes to InferDescriptor:
    if args.fingerprint != "00000001":
        return sys.stdout.write(json.dumps({"error": "Unexpected fingerprint", "fingerprint": args.fingerprint}))

    expected_desc = {
        "pkh([00000001/44h/1h/0h/0/0]02c97dc3f4420402e01a113984311bf4a1b8de376cac0bdcfaf1b3ac81f13433c7)#q3pqd8wh": "yg8pCsowDbY7PbJMthTi4xkkmhNfcNNdHV",
        "pkh([00000001/44h/1h/0h/0/1]03a20a46308be0b8ded6dff0a22b10b4245c587ccf23f3b4a303885be3a524f172)#x450egwy": "wrong_address",
    }
    if args.desc not in expected_desc:
        return sys.stdout.write(json.dumps({"error": "Unexpected descriptor", "desc": args.desc}))

    return sys.stdout.write(json.dumps({"address": expected_desc[args.desc]}))

def signtx(args):
    if args.fingerprint != "00000001":
        return sys.stdout.write(json.dumps({"error": "Unexpected fingerprint", "fingerprint": args.fingerprint}))

    with open(os.path.join(os.getcwd(), "mock_psbt"), "r", encoding="utf8") as f:
        mock_psbt = f.read()

    if args.fingerprint == "00000001" :
        sys.stdout.write(json.dumps({
            "psbt": mock_psbt,
            "complete": True
        }))
    else:
        sys.stdout.write(json.dumps({"psbt": args.psbt}))

parser = argparse.ArgumentParser(prog='./signer.py', description='External signer mock')
parser.add_argument('--fingerprint')
parser.add_argument('--chain', default='main')
parser.add_argument('--stdin', action='store_true')

subparsers = parser.add_subparsers(description='Commands', dest='command')
subparsers.required = True

parser_enumerate = subparsers.add_parser('enumerate', help='list available signers')
parser_enumerate.set_defaults(func=enumerate)

parser_getdescriptors = subparsers.add_parser('getdescriptors')
parser_getdescriptors.set_defaults(func=getdescriptors)
parser_getdescriptors.add_argument('--account', metavar='account')

parser_displayaddress = subparsers.add_parser('displayaddress', help='display address on signer')
parser_displayaddress.add_argument('--desc', metavar='desc')
parser_displayaddress.set_defaults(func=displayaddress)

parser_signtx = subparsers.add_parser('signtx')
parser_signtx.add_argument('psbt', metavar='psbt')

parser_signtx.set_defaults(func=signtx)

if not sys.stdin.isatty():
    buffer = sys.stdin.read()
    if buffer and buffer.rstrip() != "":
       sys.argv.extend(buffer.rstrip().split(" "))

args = parser.parse_args()

perform_pre_checks()

args.func(args)
