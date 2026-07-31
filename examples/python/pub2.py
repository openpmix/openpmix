#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/pub2.c
#
# Two ranks ping-pong through the publish/lookup service: each iteration
# rank 0 publishes FOOBAR:<n> and looks up rank 1's BAZ:<n>, while rank 1
# does the reverse. The published data is marked PMIX_PERSIST_FIRST_READ
# so it is consumed by the matching lookup, and the lookups carry
# PMIX_WAIT so they block until the peer publishes.
#
# Takes an optional iteration count; requires exactly 2 processes.

import os
import sys

from examples import *

NITER = 3


def main():
    if 1 < len(sys.argv):
        iters = int(sys.argv[1])
    else:
        iters = NITER

    client = PMIxClient()

    # init us
    rc, myproc = client.init(None)
    if PMIX_SUCCESS != rc:
        eprint("Client: PMIx_Init failed: %d" % rc)
        sys.exit(0)

    # get our job size
    proc = {'nspace': myproc['nspace'], 'rank': PMIX_RANK_WILDCARD}
    rc, val = client.get(proc, PMIX_JOB_SIZE, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get job size failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(client, myproc)
    nprocs = val['value']
    # require at least 2 procs
    if 2 != nprocs:
        if 0 == myproc['rank']:
            eprint("%s requires 2 processes"
                   % os.path.basename(sys.argv[0]))
        sys.exit(1)

    # the persistence directive rides along with every publish
    persist = {'key': PMIX_PERSISTENCE, 'value': PMIX_PERSIST_FIRST_READ,
               'val_type': PMIX_PERSIST}
    lkinfo = [{'key': PMIX_WAIT, 'value': True, 'val_type': PMIX_BOOL},
              {'key': PMIX_TIMEOUT, 'value': 10, 'val_type': PMIX_INT}]

    for n in range(iters):
        if 0 == myproc['rank']:
            # publish something
            key = "FOOBAR:%s.%u:%d" % (myproc['nspace'], myproc['rank'], n)
            info = [{'key': key, 'value': n, 'val_type': PMIX_INT}, persist]
            rc = client.publish(info)
            if PMIX_SUCCESS != rc:
                eprint("Client ns %s rank %d: PMIx_Publish failed: %d"
                       % (myproc['nspace'], myproc['rank'], rc))
                return done(client, myproc)

            # lookup other rank's value
            key = "BAZ:%s.%u:%d" % (myproc['nspace'], 1, n)
            rc, pdata = client.lookup([{'key': key}], lkinfo)
            if PMIX_SUCCESS != rc:
                eprint("Client ns %s rank %d: PMIx_Lookup failed: %d"
                       % (myproc['nspace'], myproc['rank'], rc))
                return done(client, myproc)
            if not check(client, myproc, pdata[0], 1, n):
                return done(client, myproc)
            eprint("PUBLISH-LOOKUP SUCCEEDED: %d" % n)
        else:
            # lookup other rank's value
            key = "FOOBAR:%s.%u:%d" % (myproc['nspace'], 0, n)
            rc, pdata = client.lookup([{'key': key}], lkinfo)
            if PMIX_SUCCESS != rc:
                eprint("Client ns %s rank %d: PMIx_Lookup failed: %d"
                       % (myproc['nspace'], myproc['rank'], rc))
                return done(client, myproc)
            if not check(client, myproc, pdata[0], 0, n):
                return done(client, myproc)

            # publish something
            key = "BAZ:%s.%u:%d" % (myproc['nspace'], myproc['rank'], n)
            info = [{'key': key, 'value': n, 'val_type': PMIX_INT}, persist]
            rc = client.publish(info)
            if PMIX_SUCCESS != rc:
                eprint("Client ns %s rank %d: PMIx_Publish failed: %d"
                       % (myproc['nspace'], myproc['rank'], rc))
                return done(client, myproc)

    return done(client, myproc)


def check(client, myproc, pdata, rank, n):
    """Check a lookup return for value and source."""
    if myproc['nspace'] != pdata['proc']['nspace']:
        eprint("Client ns %s rank %d: PMIx_Lookup returned wrong nspace: %s"
               % (myproc['nspace'], myproc['rank'], pdata['proc']['nspace']))
        return False
    if rank != pdata['proc']['rank']:
        eprint("Client ns %s rank %d: PMIx_Lookup returned wrong rank: %d"
               % (myproc['nspace'], myproc['rank'], pdata['proc']['rank']))
        return False
    if PMIX_INT != pdata['val_type']:
        eprint("Client ns %s rank %d: PMIx_Lookup returned wrong type: %d"
               % (myproc['nspace'], myproc['rank'], pdata['val_type']))
        return False
    if n != pdata['value']:
        eprint("Client ns %s rank %d: PMIx_Lookup returned wrong value: %d"
               % (myproc['nspace'], myproc['rank'], pdata['value']))
        return False
    return True


def done(client, myproc):
    # finalize us
    rc = client.finalize(None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d:PMIx_Finalize failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
    else:
        eprint("Client ns %s rank %d:PMIx_Finalize successfully completed"
               % (myproc['nspace'], myproc['rank']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
