#!/usr/bin/env python
# -*- coding: utf-8 -*-

import sys, os
sys.path.append(os.path.normpath(os.path.join(os.path.dirname(__file__), 'lib')))

from decimal import Decimal
from bitcoinrpc.authproxy import AuthServiceProxy, JSONRPCException
from sortedcontainers import SortedDict

import threading
import errno
import simplejson
import binascii
import time
import traceback


class LogListener(threading.Thread):
    lock = threading.Lock()
    logfile = open('./test.log', 'w')
    
    def __init__(self, name, default_timeout):
        threading.Thread.__init__(self)

        self.messages = SortedDict()
        self.default_timeout = default_timeout
        self.event = threading.Event()
        
        self.is_running = True
        self.name = name
        self.daemon = True
        self.start()

    def starts_with_count(self, message):
        count = 0
        for i in self.messages.irange(minimum = message):
            if not i.startswith( message ):
                break
            count += self.messages[i]
        return count

    def exact_count(self, message):
        return self.messages.get(message, 0)

    def expect_count(self, message, mincount, timeout=None):
        if timeout is None:
            timeout = self.default_timeout

        now = time.time()
        end = now + timeout
        curcount = self.exact_count(message)

        while (curcount < mincount and now < end):
            self.event.wait(end - now)
            now = time.time()
            curcount = self.exact_count(message)

        return curcount

    def expect(self, message, expected_count, timeout=None):
        count = self.expect_count(message, expected_count, timeout)
        if count != expected_count:
            print('TEST FAILED: message:"{0}", count:{1} (expected {2})'.format(message, count, expected_count))
            traceback.print_stack()
            sys.exit()
        else:
            print('{3}: message:"{0}", count:{1} (expected {2})'.format(message, count, expected_count, self.name))

    def expect_minimum(self, message, mincount, timeout=None):
        count = self.expect_count(message, mincount, timeout)
        if count < mincount:
            print('TEST FAILED: message:"{0}" count:{1} (expected at least {2})'.format(message, count, mincount))
            traceback.print_stack()
            sys.exit()
        else:
            print('{3}: message:"{0}" count:{1} (expected at least {2})'.format(message, count, mincount, self.name))

    def expect_maximum(self, message, maxcount, timeout=None):
        count = self.expect_count(message, maxcount+1, timeout)
        if count > maxcount:
            print('TEST FAILED: message:"{0}" count:{1} (expected no more than {2})'.format(message, count, maxcount))
            traceback.print_stack()
            sys.exit()
        else:
            print('{3}: message:"{0}" count:{1} (expected no more than {2})'.format(message, count, maxcount, self.name))

    def run(self):
        try:
            os.mkfifo(self.name)
        except OSError as oe:
            if oe.errno != errno.EEXIST:
                raise

        while self.is_running:
            with open(self.name) as fifo:
                with LogListener.lock:
                    print("connected to {0}".format(self.name))

                while self.is_running:
                    line = fifo.readline()
                    
                    if line == '':
                        break
                        
                    key = line[:-1];
                    with LogListener.lock:
                        LogListener.logfile.write('{0}: {1}: "{2}"\n'.format(time.strftime('%X %x %Z'), self.name, key))
                        LogListener.logfile.flush()
                    
                    self.messages[key] = self.messages.get(key, 0) + 1
                    
                    self.event.set()
                    self.event.clear()
                    
    def close(self):
        self.is_running = False
        fifo = open(self.name, 'w')
        print >> fifo, "\n"
        fifo.close()
        self.join()


class client():
    def __init__(self, rpcuser, rpcpassword, rpcbindip, rpcport):
        serverURL = 'http://' + rpcuser + ':' + rpcpassword + '@' + rpcbindip + ':' + str(rpcport)
        self.access = AuthServiceProxy(serverURL)
        
    def checksynced(self):
        try:
            r = self.access.mnsync('status')
            return (r['IsSynced'])

        except JSONRPCException as e:
            print(e.args)
            sys.exit()

        except Exception as e:
            print(e.args)
            sys.exit()

    def get_governance(self):
        try:
            r = self.access.getgovernanceinfo()
            ginfo = {
                "proposalfee": r.get('proposalfee'),
                "superblockcycle": r.get('superblockcycle'),
                "nextsuperblock": r.get('nextsuperblock')
            }
            return ginfo

        except JSONRPCException as e:
            print(e.args)
            sys.exit()

        except Exception as e:
            print(e.args)
            sys.exit()

    def get_getblockcount(self):
        try:
            r = self.access.getblockcount()
            return r

        except JSONRPCException as e:
            print(e.args)
            sys.exit()

        except Exception as e:
            print(e.args)
            sys.exit()

    def get_prepare(self, preparetime, proposalhex):
        try:
            r = self.access.gobject('prepare', str(0), str(1), str(preparetime), proposalhex)
            return r

        except JSONRPCException as e:
            print(e.args)
            sys.exit()

        except Exception as e:
            print(e.args)
            sys.exit()

    def get_submit(self, preparetime, proposalhex, feetxid):
        try:
            r = self.access.gobject('submit', str(0), str(1), str(preparetime), proposalhex, feetxid)
            return r

        except JSONRPCException as e:
            print(e.args)
            sys.exit()

        except Exception as e:
            print(e.args)
            sys.exit()
            
    def get_submit_sb(self, preparetime, triggerhex):
        try:
            r = self.access.gobject('submit', str(0), str(1), str(preparetime), triggerhex)
            return r

        except JSONRPCException as e:
            print(e.args)
            sys.exit()

        except Exception as e:
            print(e.args)
            sys.exit()

    def get_vote(self, proposalhash):
        try:
            r = self.access.gobject('vote-many', proposalhash, 'funding', 'yes')    
            return r

        except JSONRPCException as e:
            print(e.args)
            sys.exit()

        except Exception as e:
            print(e.args)
            sys.exit()


    def get_rawtxid(self, txid):
        try:
            r = self.access.getrawtransaction(txid, 1)
            confirmations = r.get('confirmations')
            if confirmations:
                print('confirmations : ', confirmations)
                return confirmations
            else:
                print('confirmations : 0')
                return 0


        except JSONRPCException as e:
            print(e.args)
            sys.exit()

        except Exception as e:
            print(e.args)
            sys.exit()

    def get_getnewaddress(self):
        try:
            r = self.access.getnewaddress()
            return r

        except JSONRPCException as e:
            print(e.args)
            sys.exit()

        except Exception as e:
            print(e.args)
            sys.exit()


# https://github.com/dashpay/sentinel/blob/master/lib/dashlib.py#L226-L236
def deserialise(hexdata):
    json = binascii.unhexlify(hexdata)
    obj = simplejson.loads(json, use_decimal=True)
    return obj

def serialise(dikt):
    json = simplejson.dumps(dikt, sort_keys=True, use_decimal=True)
    hexdata = binascii.hexlify(json.encode('utf-8')).decode('utf-8')
    return hexdata
