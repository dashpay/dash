import sys
import os
sys.path.append(os.path.join(os.path.dirname(__file__), '..'))
sys.path.append(os.path.join(os.path.dirname(__file__), '..', 'lib'))
import base58
import hashlib
import re
from decimal import Decimal
import simplejson
import binascii
from misc import printdbg, epoch2str
import time


def is_valid_dash_address(address, network='mainnet'):
    # Only public key addresses are allowed
    # A valid address is a RIPEMD-160 hash which contains 20 bytes
    # Prior to base58 encoding 1 version byte is prepended and
    # 4 checksum bytes are appended so the total number of
    # base58 encoded bytes should be 25.  This means the number of characters
    # in the encoding should be about 34 ( 25 * log2( 256 ) / log2( 58 ) ).
    dash_version = 140 if network == 'testnet' else 76

    # Check length (This is important because the base58 library has problems
    # with long addresses (which are invalid anyway).
    if ((len(address) < 26) or (len(address) > 35)):
        return False

    address_version = None

    try:
        decoded = base58.b58decode_chk(address)
        address_version = ord(decoded[0:1])
    except:
        # rescue from exception, not a valid Dash address
        return False

    if (address_version != dash_version):
        return False

    return True


def hashit(data):
    return int(hashlib.sha256(data.encode('utf-8')).hexdigest(), 16)


# returns the masternode VIN of the elected winner
def elect_mn(**kwargs):
    current_block_hash = kwargs['block_hash']
    mn_list = kwargs['mnlist']

    # filter only enabled MNs
    enabled = [mn for mn in mn_list if mn.status == 'ENABLED']

    block_hash_hash = hashit(current_block_hash)

    candidates = []
    for mn in enabled:
        mn_vin_hash = hashit(mn.vin)
        diff = mn_vin_hash - block_hash_hash
        absdiff = abs(diff)
        candidates.append({'vin': mn.vin, 'diff': absdiff})

    candidates.sort(key=lambda k: k['diff'])

    try:
        winner = candidates[0]['vin']
    except:
        winner = None

    return winner


def parse_masternode_status_vin(status_vin_string):
    status_vin_string_regex = re.compile('CTxIn\(COutPoint\(([0-9a-zA-Z]+),\\s*(\d+)\),')

    m = status_vin_string_regex.match(status_vin_string)
    txid = m.group(1)
    index = m.group(2)

    vin = txid + '-' + index
    if (txid == '0000000000000000000000000000000000000000000000000000000000000000'):
        vin = None

    return vin


def create_superblock(proposals, event_block_height, budget_max, sb_epoch_time):
    from models import Superblock, GovernanceObject, Proposal

    # don't create an empty superblock
    if (len(proposals) == 0):
        printdbg("No proposals, cannot create an empty superblock.")
        return None

    budget_allocated = Decimal(0)
    fudge = 60 * 60 * 2  # fudge-factor to allow for slighly incorrect estimates

    payments = []
    for proposal in proposals:
        fmt_string = "name: %s, rank: %4d, hash: %s, amount: %s <= %s"

        # skip proposals that are too expensive...
        if (budget_allocated + proposal.payment_amount) > budget_max:
            printdbg(
                fmt_string % (
                    proposal.name,
                    proposal.rank,
                    proposal.object_hash,
                    proposal.payment_amount,
                    "skipped (blows the budget)",
                )
            )
            continue

        # skip proposals if the SB isn't within the Proposal time window...
        window_start = proposal.start_epoch - fudge
        window_end = proposal.end_epoch + fudge

        printdbg("\twindow_start: %s" % epoch2str(window_start))
        printdbg("\twindow_end: %s" % epoch2str(window_end))
        printdbg("\tsb_epoch_time: %s" % epoch2str(sb_epoch_time))

        if (sb_epoch_time < window_start or sb_epoch_time > window_end):
            printdbg(
                fmt_string % (
                    proposal.name,
                    proposal.rank,
                    proposal.object_hash,
                    proposal.payment_amount,
                    "skipped (SB time is outside of Proposal window)",
                )
            )
            continue

        printdbg(
            fmt_string % (
                proposal.name,
                proposal.rank,
                proposal.object_hash,
                proposal.payment_amount,
                "adding",
            )
        )

        # else add proposal and keep track of total budget allocation
        budget_allocated += proposal.payment_amount

        payment = {'address': proposal.payment_address,
                   'amount': "{0:.8f}".format(proposal.payment_amount),
                   'proposal': "{}".format(proposal.object_hash)}
        payments.append(payment)

    # don't create an empty superblock
    if not payments:
        printdbg("No proposals made the cut!")
        return None

    # 'payments' now contains all the proposals for inclusion in the
    # Superblock, but needs to be sorted by proposal hash descending
    payments.sort(key=lambda k: k['proposal'], reverse=True)

    sb = Superblock(
        event_block_height=event_block_height,
        payment_addresses='|'.join([pd['address'] for pd in payments]),
        payment_amounts='|'.join([pd['amount'] for pd in payments]),
        proposal_hashes='|'.join([pd['proposal'] for pd in payments]),
    )
    printdbg("generated superblock: %s" % sb.__dict__)

    return sb


# shims 'til we can fix the dashd side
def SHIM_serialise_for_dashd(sentinel_hex):
    from models import DASHD_GOVOBJ_TYPES
    # unpack
    obj = deserialise(sentinel_hex)

    # shim for dashd
    govtype = obj[0]

    # add 'type' attribute
    obj[1]['type'] = DASHD_GOVOBJ_TYPES[govtype]

    # superblock => "trigger" in dashd
    if govtype == 'superblock':
        obj[0] = 'trigger'

    # dashd expects an array (even though there is only a 1:1 relationship between govobj->class)
    obj = [obj]

    # re-pack
    dashd_hex = serialise(obj)
    return dashd_hex


# shims 'til we can fix the dashd side
def SHIM_deserialise_from_dashd(dashd_hex):
    from models import DASHD_GOVOBJ_TYPES

    # unpack
    obj = deserialise(dashd_hex)

    # shim from dashd
    # only one element in the array...
    obj = obj[0]

    # extract the govobj type
    govtype = obj[0]

    # superblock => "trigger" in dashd
    if govtype == 'trigger':
        obj[0] = govtype = 'superblock'

    # remove redundant 'type' attribute
    if 'type' in obj[1]:
        del obj[1]['type']

    # re-pack
    sentinel_hex = serialise(obj)
    return sentinel_hex


# convenience
def deserialise(hexdata):
    json = binascii.unhexlify(hexdata)
    obj = simplejson.loads(json, use_decimal=True)
    return obj


def serialise(dikt):
    json = simplejson.dumps(dikt, sort_keys=True, use_decimal=True)
    hexdata = binascii.hexlify(json.encode('utf-8')).decode('utf-8')
    return hexdata


def did_we_vote(output):
    from bitcoinrpc.authproxy import JSONRPCException

    # sentinel
    voted = False
    err_msg = ''

    try:
        detail = output.get('detail').get('dash.conf')
        result = detail.get('result')
        if 'errorMessage' in detail:
            err_msg = detail.get('errorMessage')
    except JSONRPCException as e:
        result = 'failed'
        err_msg = e.message

    # success, failed
    printdbg("result  = [%s]" % result)
    if err_msg:
        printdbg("err_msg = [%s]" % err_msg)

    voted = False
    if result == 'success':
        voted = True

    # in case we spin up a new instance or server, but have already voted
    # on the network and network has recorded those votes
    m_old = re.match(r'^time between votes is too soon', err_msg)
    m_new = re.search(r'Masternode voting too often', err_msg, re.M)

    if result == 'failed' and (m_old or m_new):
        printdbg("DEBUG: Voting too often, need to sync w/network")
        voted = False

    return voted


def parse_raw_votes(raw_votes):
    votes = []
    for v in list(raw_votes.values()):
        (outpoint, ntime, outcome, signal) = v.split(':')
        signal = signal.lower()
        outcome = outcome.lower()

        mn_collateral_outpoint = parse_masternode_status_vin(outpoint)
        v = {
            'mn_collateral_outpoint': mn_collateral_outpoint,
            'signal': signal,
            'outcome': outcome,
            'ntime': ntime,
        }
        votes.append(v)

    return votes
