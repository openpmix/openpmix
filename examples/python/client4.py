#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/client4.c
#
# Check the rank-qualified job info: rank, nodeID and appnum must come
# back correctly for ourselves and for a peer, and must be refused when
# asked for with the wildcard rank (they are per-rank values, so there is
# no job-level answer).
#
# Expects two procs, one per node, each in its own app.

import sys

from examples import *


def main():
    client = PMIxClient()

    rc, myproc = client.init(None)
    if PMIX_SUCCESS != rc:
        if PMIX_ERR_UNREACH == rc:
            eprint("Cannot operate as singleton")
        else:
            eprint("PMIx_Init failed:", client.error_string(rc))
        sys.exit(1)
    # our identity is fixed, so cache its string form and reuse it. Unlike
    # the C version there is nothing to free - proc_string() hands back an
    # ordinary Python string
    _, me = client.proc_string(myproc)
    eprint("%s: Running" % me)

    # get our rank
    proc = {'nspace': myproc['nspace'], 'rank': PMIX_RANK_INVALID}
    rc, val = client.get(proc, PMIX_RANK, None)
    if PMIX_SUCCESS != rc:
        eprint("%s: Get rank failed - %s" % (me, client.error_string(rc)))
        return done(client)
    if val['value'] != myproc['rank']:
        eprint("%s: Get rank returned wrong rank - %u instead of %u"
               % (me, val['value'], myproc['rank']))
        return done(client)
    eprint("%s: Rank return correct" % me)

    # get our node ID
    rc, val = client.get(myproc, PMIX_NODEID, None)
    if PMIX_SUCCESS != rc:
        eprint("%s: Get my nodeID failed - %s" % (me, client.error_string(rc)))
        return done(client)
    # rank=0 is on node 0, rank=1 on node 1
    if val['value'] != myproc['rank']:
        eprint("%s: Get my nodeID returned wrong value - %u instead of %u"
               % (me, val['value'], myproc['rank']))
        return done(client)
    eprint("%s: NodeID return correct" % me)

    # get our node ID with rank=WILDCARD - should fail!
    proc = {'nspace': myproc['nspace'], 'rank': PMIX_RANK_WILDCARD}
    rc, val = client.get(proc, PMIX_NODEID, None)
    if PMIX_SUCCESS == rc:
        eprint("%s: Get my nodeID with WILDCARD rank incorrectly succeeded"
               % me)
        return done(client)
    eprint("%s: NodeID with WILDCARD rank correctly failed - %s"
           % (me, client.error_string(rc)))

    # get our peer's nodeID
    proc['rank'] = 1 if 0 == myproc['rank'] else 0
    rc, val = client.get(proc, PMIX_NODEID, None)
    if PMIX_SUCCESS != rc:
        eprint("%s: Get peer's nodeID failed - %s"
               % (me, client.error_string(rc)))
        return done(client)
    # rank=0 is on node 0, rank=1 on node 1
    if val['value'] != proc['rank']:
        eprint("%s: Get peer's nodeID returned wrong value - %u instead of %u"
               % (me, val['value'], proc['rank']))
        return done(client)
    eprint("%s: Peer's NodeID return correct" % me)

    # get our appnum
    rc, val = client.get(myproc, PMIX_APPNUM, None)
    if PMIX_SUCCESS != rc:
        eprint("%s: Get my appnum failed - %s" % (me, client.error_string(rc)))
        return done(client)
    # rank=0 is in appnum 0, rank=1 is in appnum 1
    if val['value'] != myproc['rank']:
        eprint("%s: Get my appnum returned wrong value - %u instead of %u"
               % (me, val['value'], myproc['rank']))
        return done(client)
    eprint("%s: Appnum return correct" % me)

    # get appnum with WILDCARD - should fail!
    proc = {'nspace': myproc['nspace'], 'rank': PMIX_RANK_WILDCARD}
    rc, val = client.get(proc, PMIX_APPNUM, None)
    if PMIX_SUCCESS == rc:
        eprint("%s: Get appnum with WILDCARD incorrectly succeeded - "
               "returned %u" % (me, val['value']))
        return done(client)
    eprint("%s: Appnum with WILDCARD rank correctly failed - %s"
           % (me, client.error_string(rc)))

    # get peer's appnum
    proc['rank'] = 1 if 0 == myproc['rank'] else 0
    rc, val = client.get(proc, PMIX_APPNUM, None)
    if PMIX_SUCCESS != rc:
        eprint("%s: Get peer's appnum failed - %s"
               % (me, client.error_string(rc)))
        return done(client)
    # rank=0 is in appnum 0, rank=1 is in appnum 1
    if val['value'] != proc['rank']:
        eprint("%s: Get peer's appnum returned wrong value - %u instead of %u"
               % (me, val['value'], proc['rank']))
        return done(client)
    eprint("%s: Peer's Appnum return correct" % me)

    return done(client)


def done(client):
    # finalize us
    client.finalize(None)
    return 0


if __name__ == '__main__':
    sys.exit(main())
