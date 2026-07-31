#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/dmodex.c
#
# Exercise the direct-modex path: commit values but never fence, so every
# get of a peer's key has to be satisfied by the server going out and
# fetching it on demand. A timeout qualifier bounds each such get.

import sys

from examples import *

TLIMIT = 10


def main():
    client = PMIxClient()

    # init us
    rc, myproc = client.init(None)
    if PMIX_SUCCESS != rc:
        eprint("Client: PMIx_Init failed:", client.error_string(rc))
        sys.exit(0)

    # get our job size
    proc = {'nspace': myproc['nspace'], 'rank': PMIX_RANK_WILDCARD}
    rc, val = client.get(proc, PMIX_JOB_SIZE, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get job size failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(client, myproc)
    nprocs = val['value']

    if 0 == myproc['rank']:
        eprint("Client ns %s rank %d: Running. World size %d"
               % (myproc['nspace'], myproc['rank'], nprocs))

    # put a few values
    tmp = "%s-%d-internal" % (myproc['nspace'], myproc['rank'])
    rc = client.store_internal(myproc, tmp,
                               {'value': 1234, 'val_type': PMIX_UINT32})
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Store_internal failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(client, myproc)

    tmp = "%s-%d-local" % (myproc['nspace'], myproc['rank'])
    rc = client.put(PMIX_LOCAL, tmp, {'value': 1234, 'val_type': PMIX_UINT64})
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Put local failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(client, myproc)

    tmp = "%s-%d-remote" % (myproc['nspace'], myproc['rank'])
    rc = client.put(PMIX_GLOBAL, tmp,
                    {'value': "1234", 'val_type': PMIX_STRING})
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Put remote failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(client, myproc)

    # a byte object - the Python shape is {'bytes': ..., 'size': ...}
    bo = bytes(range(128))
    rc = client.put(PMIX_GLOBAL, "ghex",
                    {'value': {'bytes': bo, 'size': len(bo)},
                     'val_type': PMIX_BYTE_OBJECT})
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Put ghex failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(client, myproc)

    # commit the data to the server
    rc = client.commit()
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Commit failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(client, myproc)

    # get a list of our local peers
    rc, val = client.get(proc, PMIX_LOCAL_PEERS, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get local peers failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(client, myproc)
    # split the returned string to get the rank of each local peer
    peers = val['value'].split(',')
    nlocal = len(peers)
    if nprocs == nlocal:
        all_local = True
        locals_ = []
    else:
        all_local = False
        locals_ = [int(p) for p in peers]

    timeout = [{'key': PMIX_TIMEOUT, 'value': TLIMIT, 'val_type': PMIX_INT}]

    # get the committed data
    for n in range(nprocs):
        if n == myproc['rank']:
            # local peers doesn't include us, so check for
            # ourselves separately
            local = True
        elif all_local:
            local = True
        else:
            local = n in locals_
        proc['rank'] = n
        if local:
            tmp = "%s-%d-local" % (myproc['nspace'], n)
            rc, val = client.get(proc, tmp, timeout)
            if PMIX_SUCCESS != rc:
                eprint("Client ns %s rank %d: PMIx_Get %s failed: %s"
                       % (myproc['nspace'], myproc['rank'], tmp,
                          client.error_string(rc)))
                return done(client, myproc)
            if PMIX_UINT64 != val['val_type']:
                eprint("%s:%d: PMIx_Get Key %s failed - returned wrong "
                       "type: %s" % (myproc['nspace'], myproc['rank'], tmp,
                                     client.data_type_string(val['val_type'])))
                return done(client, myproc)
            if 1234 != val['value']:
                eprint("%s:%d: PMIx_Get Key %s failed - returned wrong "
                       "value: %d" % (myproc['nspace'], myproc['rank'], tmp,
                                      val['value']))
                return done(client, myproc)
        else:
            tmp = "%s-%d-remote" % (myproc['nspace'], n)
            rc, val = client.get(proc, tmp, timeout)
            if PMIX_SUCCESS != rc:
                eprint("Client ns %s rank %d: PMIx_Get %s failed: %s"
                       % (myproc['nspace'], myproc['rank'], tmp,
                          client.error_string(rc)))
                return done(client, myproc)
            if PMIX_STRING != val['val_type']:
                eprint("%s:%d: PMIx_Get Key %s failed - returned wrong "
                       "type: %s" % (myproc['nspace'], myproc['rank'], tmp,
                                     client.data_type_string(val['val_type'])))
                return done(client, myproc)
            if "1234" != val['value']:
                eprint("%s:%d: PMIx_Get Key %s failed - returned wrong "
                       "value: %s" % (myproc['nspace'], myproc['rank'], tmp,
                                      val['value']))
                return done(client, myproc)
        # if this isn't us, then get the ghex key
        if n != myproc['rank']:
            rc, val = client.get(proc, "ghex", timeout)
            if PMIX_SUCCESS != rc:
                eprint("Client ns %s rank %d: PMIx_Get ghex failed: %s"
                       % (myproc['nspace'], myproc['rank'],
                          client.error_string(rc)))
                return done(client, myproc)
            if PMIX_BYTE_OBJECT != val['val_type']:
                eprint("%s:%d: PMIx_Get ghex failed - returned wrong type: %s"
                       % (myproc['nspace'], myproc['rank'],
                          client.data_type_string(val['val_type'])))
                return done(client, myproc)
            if 128 != val['value']['size']:
                eprint("%s:%d: PMIx_Get ghex failed - returned wrong size: %d"
                       % (myproc['nspace'], myproc['rank'],
                          val['value']['size']))
                return done(client, myproc)

    return done(client, myproc)


def done(client, myproc):
    # call fence so everyone waits before leaving
    rc = client.fence(None, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Fence failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        sys.exit(1)

    rc = client.finalize(None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d:PMIx_Finalize failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
    elif 0 == myproc['rank']:
        eprint("Client ns %s rank %d:PMIx_Finalize successfully completed"
               % (myproc['nspace'], myproc['rank']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
