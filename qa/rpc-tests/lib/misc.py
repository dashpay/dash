import time
from datetime import datetime
import re
import sys
import os


def is_numeric(strin):
    import decimal

    # Decimal allows spaces in input, but we don't
    if strin.strip() != strin:
        return False
    try:
        value = decimal.Decimal(strin)
    except decimal.InvalidOperation as e:
        return False

    return True


def printdbg(str):
    ts = time.strftime('%Y-%m-%d %H:%M:%S', time.gmtime(now()))
    logstr = "{} {}".format(ts, str)
    if os.environ.get('SENTINEL_DEBUG', None):
        print(logstr)

    sys.stdout.flush()


def is_hash(s):
    m = re.match('^[a-f0-9]{64}$', s)
    return m is not None


def now():
    return int(time.time())


def epoch2str(epoch):
    return datetime.utcfromtimestamp(epoch).strftime("%Y-%m-%d %H:%M:%S")


class Bunch(object):
    def __init__(self, **kwargs):
        self.__dict__.update(kwargs)

    def get(self, name):
        return self.__dict__.get(name, None)
