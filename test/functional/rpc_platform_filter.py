#!/usr/bin/env python3
# Copyright (c) 2020 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that commands submitted by the platform user are filtered."""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import str_to_b64str, assert_equal

import os
import http.client
import urllib.parse


class HTTPBasicsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def setup_chain(self):
        super().setup_chain()
        # Append rpcauth to dash.conf before initialization
        rpcauthplatform = "rpcauth=platform-user:dd88fd676186f48553775d6fb5a2d344$bc1f7898698ead19c6ec7ff47055622dd7101478f1ff6444103d3dc03cd77c13"
        # rpcuser : platform-user
        # rpcpassword : password123
        rpcauthoperator = "rpcauth=operator:e9b45dd0b61a7be72155535435365a3a$8fb7470bc6f74d8ceaf9a23f49b06127723bd563b3ed5d9cea776ef01803d191"
        # rpcuser : operator
        # rpcpassword : otherpassword

        masternodeblskey="masternodeblsprivkey=58af6e39bb4d86b22bda1a02b134c2f5b71caffa1377540b02f7f1ad122f59e0"

        with open(os.path.join(self.options.tmpdir+"/node0", "dash.conf"), 'a', encoding='utf8') as f:
            f.write(masternodeblskey+"\n")
            f.write(rpcauthplatform+"\n")
            f.write(rpcauthoperator+"\n")

    def run_test(self):

        url = urllib.parse.urlparse(self.nodes[0].url)

        self.log.info('Try using a incorrect password for platform-user...')
        rpcuserauthpairwrong = "platform-user:rpcpasswordwrong"

        headers = {"Authorization": "Basic " + str_to_b64str(rpcuserauthpairwrong)}

        conn = http.client.HTTPConnection(url.hostname, url.port)
        conn.connect()
        conn.request('POST', '/', '{"method": "getbestblockhash"}', headers)
        resp = conn.getresponse()
        assert_equal(resp.status, 401)
        conn.close()

        self.log.info('Try using a correct password for platform-user and running a whitelisted command...')
        rpcuserauthpairplatform = "platform-user:password123"
        rpcuserauthpairoperator = "operator:otherpassword"

        headers = {"Authorization": "Basic " + str_to_b64str(rpcuserauthpairplatform)}

        conn = http.client.HTTPConnection(url.hostname, url.port)
        conn.connect()
        conn.request('POST', '/', '{"method": "getbestblockhash"}', headers)
        resp = conn.getresponse()
        assert_equal(resp.status, 200)
        conn.close()

        self.log.info('Try running a not whitelisted command...')

        headers = {"Authorization": "Basic " + str_to_b64str(rpcuserauthpairplatform)}

        conn = http.client.HTTPConnection(url.hostname, url.port)
        conn.connect()
        conn.request('POST', '/', '{"method": "stop"}', headers)
        resp = conn.getresponse()
        assert_equal(resp.status, 403)
        conn.close()

        self.log.info('Try running a not whitelisted command as the operator...')

        headers = {"Authorization": "Basic " + str_to_b64str(rpcuserauthpairoperator)}

        conn = http.client.HTTPConnection(url.hostname, url.port)
        conn.connect()
        conn.request('POST', '/', '{"method": "stop"}', headers)
        resp = conn.getresponse()
        assert_equal(resp.status, 200)
        conn.close()


if __name__ == '__main__':
    HTTPBasicsTest().main()
