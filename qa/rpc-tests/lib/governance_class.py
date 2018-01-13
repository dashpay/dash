import os
import sys
sys.path.append(os.path.join(os.path.dirname(__file__), '..', 'lib'))
import models
from bitcoinrpc.authproxy import JSONRPCException
import misc
import re
from misc import printdbg
import time


# mixin for GovObj composed classes like proposal and superblock, etc.
class GovernanceClass(object):
    only_masternode_can_submit = False

    # lazy
    @property
    def go(self):
        return self.governance_object

    # pass thru to GovernanceObject#vote
    def vote(self, dashd, signal, outcome):
        return self.go.vote(dashd, signal, outcome)

    # pass thru to GovernanceObject#voted_on
    def voted_on(self, **kwargs):
        return self.go.voted_on(**kwargs)

    def vote_validity(self, dashd):
        if self.is_valid():
            printdbg("Voting valid! %s: %d" % (self.__class__.__name__, self.id))
            self.vote(dashd, models.VoteSignals.valid, models.VoteOutcomes.yes)
        else:
            printdbg("Voting INVALID! %s: %d" % (self.__class__.__name__, self.id))
            self.vote(dashd, models.VoteSignals.valid, models.VoteOutcomes.no)

    def get_submit_command(self):
        object_fee_tx = self.go.object_fee_tx

        import dashlib
        obj_data = dashlib.SHIM_serialise_for_dashd(self.serialise())

        cmd = ['gobject', 'submit', '0', '1', str(int(time.time())), obj_data, object_fee_tx]

        return cmd

    def list(self):
        dikt = {
            "DataHex": self.serialise(),
            "Hash": self.object_hash,
            "CollateralHash": self.go.object_fee_tx,
            "AbsoluteYesCount": self.go.absolute_yes_count,
            "YesCount": self.go.yes_count,
            "NoCount": self.go.no_count,
            "AbstainCount": self.go.abstain_count,
        }

        # return a dict similar to dashd "gobject list" output
        return {self.object_hash: dikt}

    def get_submit_command(self):
        import dashlib
        obj_data = dashlib.SHIM_serialise_for_dashd(self.serialise())

        # new objects won't have parent_hash, revision, etc...
        cmd = ['gobject', 'submit', '0', '1', str(int(time.time())), obj_data]

        # some objects don't have a collateral tx to submit
        if not self.only_masternode_can_submit:
            cmd.append(go.object_fee_tx)

        return cmd

    def submit(self, dashd):
        # don't attempt to submit a superblock unless a masternode
        # note: will probably re-factor this, this has code smell
        if (self.only_masternode_can_submit and not dashd.is_masternode()):
            print("Not a masternode. Only masternodes may submit these objects")
            return

        try:
            object_hash = dashd.rpc_command(*self.get_submit_command())
            printdbg("Submitted: [%s]" % object_hash)
            return object_hash
        except JSONRPCException as e:
            print("Unable to submit: %s" % e.message)

    def serialise(self):
        import inflection
        import binascii
        import simplejson

        # 'proposal', 'superblock', etc.
        name = self._meta.name
        obj_type = inflection.singularize(name)

        return binascii.hexlify(simplejson.dumps((obj_type, self.get_dict()), sort_keys=True).encode('utf-8')).decode('utf-8')

    def dashd_serialise(self):
        import dashlib
        return dashlib.SHIM_serialise_for_dashd(self.serialise())

    @classmethod
    def serialisable_fields(self):
        # Python is so not very elegant...
        pk_column = self._meta.primary_key.db_column
        fk_columns = [fk.db_column for fk in self._meta.rel.values()]
        do_not_use = [pk_column]
        do_not_use.extend(fk_columns)
        do_not_use.append('object_hash')
        fields_to_serialise = list(self._meta.columns.keys())

        for field in do_not_use:
            if field in fields_to_serialise:
                fields_to_serialise.remove(field)

        return fields_to_serialise

    def get_dict(self):
        dikt = {}

        for field_name in self.serialisable_fields():
            dikt[field_name] = getattr(self, field_name)

        return dikt
