#!/usr/bin/env python2
#
# Distributed under the MIT/X11 software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#

from test_framework.mininode import *
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import *
import time

#TODO make the dash-specific code as valid python library in order to use it in sentinel as well as rpc-tests

import testtools
from models import Superblock
from dashd import DashDaemon

def now():
    return int(time.time())


class MNCheckRateTest(BitcoinTestFramework):
    def add_options(self, parser):
        parser.add_option("--testbinary", dest="testbinary",
                          default=os.getenv("DASHD", "dashd"),
                          help="bitcoind binary to test")

    def setup_chain(self):
        
        initialize_chain_mn(self.options.tmpdir)

    def setup_network(self):

        self.nodes = []

        #start masternode with -govtest parameter
        self.nodes.append(start_node(0, self.options.tmpdir, ["-externalip=127.0.0.1","-debug","-govtest","-testlog=/tmp/node0"]))
        #start regular nodes with test log enabled
        self.nodes.append(start_node(4, self.options.tmpdir, ["-externalip=127.0.0.1","-debug","-testlog=/tmp/node4"]))
        self.nodes.append(start_node(5, self.options.tmpdir, ["-externalip=127.0.0.1","-debug","-testlog=/tmp/node5"]))
        
        #start other nodes
        for i in range(1,4):
            self.nodes.append(start_node(i, self.options.tmpdir, ["-externalip=127.0.0.1","-debug"]))

        #connect nodes
        for i in range(6):
            for j in range(i+1,6):
                connect_nodes(self.nodes[i], j)

        get_rpc_proxy(rpc_url(0), 0).generate(1)
        sync_masternodes(self.nodes)

    def run_test(self):
        #CTestNetParams
        nPowTargetSpacing = 2.5 * 60

        #node1 should be a masternode started with "-govtest" parameter
        node1 = DashDaemon(host = '127.0.0.1', user='rt', password = 'rt', port = rpc_port(0))
        #node2 and node3 are regular nodes
        node2 = DashDaemon(host = '127.0.0.1', user='rt', password = 'rt', port = rpc_port(4))
        node3 = DashDaemon(host = '127.0.0.1', user='rt', password = 'rt', port = rpc_port(5))
        
        #~ node1 = get_rpc_proxy(rpc_url(0), 0)
        #~ node2 = get_rpc_proxy(rpc_url(12), 12)
        #~ node3 = get_rpc_proxy(rpc_url(13), 13)

        log1 = testtools.LogListener('/tmp/node0', 10)
        log2 = testtools.LogListener('/tmp/node4', 10)
        log3 = testtools.LogListener('/tmp/node5', 10)

        nSuperblockCycleSeconds = node1.superblockcycle() * nPowTargetSpacing

        while(not node1.rpc_command(*['masternode', 'list']) or not node1.is_synced() or not node2.is_synced() or not node3.is_synced()):
            print('not yet synced, sleep 5 sec')
            time.sleep(5)

        # create properly signed trigger that should be accepted as preliminarily valid

        event_block_height = node1.next_superblock_height()
        payment_amounts = 1

        sbobj = Superblock(
            event_block_height=event_block_height,
            payment_addresses='yYe8KwyaUu5YswSYmB3q3ryx8XTUu9y7Ui',
            payment_amounts='{0}'.format(payment_amounts),
            proposal_hashes='e8a0057914a2e1964ae8a945c4723491caae2077a90a00a2aabee22b40081a87',
        )

        ratecheckbuffer = list()

        sb_time = int(now())
        cmd = ['gobject', 'submit', '0', '1', str(sb_time), sbobj.dashd_serialise()]
        object_hash = node1.rpc_command(*cmd)

        print '\nperform basic checks\n'
        ratecheckbuffer.append(sb_time)

        log1.expect_minimum('push_inventory:govobj {0}'.format(object_hash), 1)

        log2.expect('govobj_accepted:{0}'.format(object_hash), 1)
        log3.expect('govobj_accepted:{0}'.format(object_hash), 1)

        log2.expect_minimum('push_inventory:govobj {0}'.format(object_hash), 1)
        log3.expect_minimum('push_inventory:govobj {0}'.format(object_hash), 1)

        print '\nwaiting 10 seconds'
        time.sleep(10)

        print 'try send duplicate object'

        pushed_count1 = log1.exact_count('push_inventory:govobj {0}'.format(object_hash))
        pushed_count2 = log2.exact_count('push_inventory:govobj {0}'.format(object_hash))
        pushed_count3 = log3.exact_count('push_inventory:govobj {0}'.format(object_hash))

        dup_hash = node1.rpc_command(*cmd)
        assert object_hash == dup_hash

        print 'verify that duplicate has not relayed\n'
        log1.expect_maximum('push_inventory:govobj {0}'.format(object_hash), pushed_count1, 10)
        log2.expect_maximum('push_inventory:govobj {0}'.format(object_hash), pushed_count2, 0)
        log3.expect_maximum('push_inventory:govobj {0}'.format(object_hash), pushed_count3, 0)

        print '\ncheck rate limit (4 objects in 4 seconds)\n'
        sb_time = int(now())
        while len(ratecheckbuffer) < 4:
            sb_time += 1
            cmd[4] = str(sb_time)
            object_hash = node1.rpc_command(*cmd)
            log2.expect('govobj_accepted:{0}'.format(object_hash), 1)
            log3.expect('govobj_accepted:{0}'.format(object_hash), 1)
            ratecheckbuffer.append(sb_time)


        print '\ncheck rate limit (5 objects in 5 seconds)\n'
        sb_time += 1
        cmd[4] = str(sb_time)
        object_hash = node1.rpc_command(*cmd)

        log2.expect_minimum('rate_too_high:{0}'.format(object_hash), 1)
        log3.expect_minimum('rate_too_high:{0}'.format(object_hash), 1)
        log2.expect_maximum('govobj_accepted:{0}'.format(object_hash), 0, 10)
        log3.expect_maximum('govobj_accepted:{0}'.format(object_hash), 0, 0)


        print '\ncheck rate limit (5 objects in (nSuperblockCycleSeconds*5/2.2)-1 seconds)'
        sb_time = int(ratecheckbuffer[0] + (nSuperblockCycleSeconds*5/2.2)-1)

        delay = sb_time - now()
        print 'wait {0} minutes (mocktime)\n'.format(delay/60.)
        assert delay > 0
        #time.sleep(sb_time - now() - 1)
        node2.rpc_command('setmocktime', sb_time)
        cmd[4] = str(sb_time)
        object_hash = node1.rpc_command(*cmd)

        log2.expect_minimum('rate_too_high:{0}'.format(object_hash), 1)
        log3.expect_minimum('rate_too_high:{0}'.format(object_hash), 1)
        log2.expect_maximum('govobj_accepted:{0}'.format(object_hash), 0, 10)
        log3.expect_maximum('govobj_accepted:{0}'.format(object_hash), 0, 0)


        print '\ncheck rate limit (5 objects in (nSuperblockCycleSeconds*5/2.2)+1 seconds)\n'
        sb_time = int(ratecheckbuffer[0] + (nSuperblockCycleSeconds*5/2.2)+1)
        cmd[4] = str(sb_time)
        object_hash = node1.rpc_command(*cmd)

        log2.expect_maximum('rate_too_high:{0}'.format(object_hash), 0, 10)
        log3.expect_maximum('rate_too_high:{0}'.format(object_hash), 0, 0)
        log2.expect_minimum('govobj_accepted:{0}'.format(object_hash), 1)
        log3.expect_minimum('govobj_accepted:{0}'.format(object_hash), 1)
        log2.expect_minimum('push_inventory:govobj {0}'.format(object_hash), 1)
        log3.expect_minimum('push_inventory:govobj {0}'.format(object_hash), 1)


        print '\ncheck for errors\n'

        assert log2.starts_with_count("inv_dropped") == 0
        assert log2.starts_with_count("govobj_invalid_time") == 0
        assert log2.starts_with_count("govobj_unrequested_received") == 0
        assert log2.starts_with_count("govobj_seen_received") == 0
        assert log2.starts_with_count("govobj_invalid_received") == 0

        assert log3.starts_with_count("inv_dropped") == 0
        assert log3.starts_with_count("govobj_invalid_time") == 0
        assert log3.starts_with_count("govobj_unrequested_received") == 0
        assert log3.starts_with_count("govobj_seen_received") == 0
        assert log3.starts_with_count("govobj_invalid_received") == 0

        log1.close()
        log2.close()
        log3.close()


if __name__ == '__main__':
    MNCheckRateTest().main()
