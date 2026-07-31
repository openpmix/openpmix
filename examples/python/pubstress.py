#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/pubstress.c
#
# A heavier workout of the publish/lookup service:
#
#   ranks 0,1   ping-pong PMIX_PERSIST_FIRST_READ data, as pub2 does, but
#               with deliberate delays so the two get out of step
#   rank 2      publishes a value each iteration that everyone else reads
#               (no first-read persistence, so many readers can see it)
#   rank 2      then exercises unpublish: by key, a full purge, and a
#               persistence-qualified unpublish that must remove only the
#               app-scoped key and leave the session-scoped one
#
# Takes an optional iteration count; requires at least 4 processes.

import os
import sys
import time

from examples import *

NITER = 10


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
    # require at least 4 procs
    if nprocs < 4:
        if 0 == myproc['rank']:
            eprint("%s requires at least 4 processes"
                   % os.path.basename(sys.argv[0]))
        sys.exit(1)

    persist = {'key': PMIX_PERSISTENCE, 'value': PMIX_PERSIST_FIRST_READ,
               'val_type': PMIX_PERSIST}
    lkinfo = [{'key': PMIX_WAIT, 'value': True, 'val_type': PMIX_BOOL},
              {'key': PMIX_TIMEOUT, 'value': 10, 'val_type': PMIX_INT}]

    if myproc['rank'] < 2:
        for n in range(iters):
            if 0 == myproc['rank']:
                if n % 2:   # delay us a bit so other ranks get ahead
                    time.sleep(1)
                # publish something
                key = "FOOBAR:%s.%u:%d" % (myproc['nspace'],
                                           myproc['rank'], n)
                rc = client.publish([{'key': key, 'value': n,
                                      'val_type': PMIX_INT}, persist])
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
                if not check(client, myproc, pdata[0], 1, PMIX_INT, n):
                    return done(client, myproc)
                eprint("PUBLISH-LOOKUP SUCCEEDED: %d" % n)
            elif 1 == myproc['rank']:
                if 0 == n:
                    time.sleep(1)
                # lookup other rank's value
                key = "FOOBAR:%s.%u:%d" % (myproc['nspace'], 0, n)
                rc, pdata = client.lookup([{'key': key}], lkinfo)
                if PMIX_SUCCESS != rc:
                    eprint("Client ns %s rank %d: PMIx_Lookup failed: %d"
                           % (myproc['nspace'], myproc['rank'], rc))
                    return done(client, myproc)
                if not check(client, myproc, pdata[0], 0, PMIX_INT, n):
                    return done(client, myproc)
                # publish something
                key = "BAZ:%s.%u:%d" % (myproc['nspace'],
                                        myproc['rank'], n)
                rc = client.publish([{'key': key, 'value': n,
                                      'val_type': PMIX_INT}, persist])
                if PMIX_SUCCESS != rc:
                    eprint("Client ns %s rank %d: PMIx_Publish failed: %d"
                           % (myproc['nspace'], myproc['rank'], rc))
                    return done(client, myproc)
        return done(client, myproc)

    # we cannot participate in the "firstread" cases as only
    # one other proc can read the value
    for n in range(iters):
        if 2 == myproc['rank']:
            if n % 2:   # delay us a bit so other ranks get ahead
                time.sleep(1)
            # publish something
            key = "BIGTEST:%s.%u:%d" % (myproc['nspace'], myproc['rank'], n)
            bcount = 1234 + n
            eprint("Client ns %s rank %d: Published: %lu"
                   % (myproc['nspace'], myproc['rank'], bcount))
            rc = client.publish([{'key': key, 'value': bcount,
                                  'val_type': PMIX_SIZE}])
            if PMIX_SUCCESS != rc:
                eprint("Client ns %s rank %d: PMIx_Publish failed: %d"
                       % (myproc['nspace'], myproc['rank'], rc))
                return done(client, myproc)
        else:
            # everyone else retrieves it
            key = "BIGTEST:%s.%u:%d" % (myproc['nspace'], 2, n)
            rc, pdata = client.lookup([{'key': key}], lkinfo)
            if PMIX_SUCCESS != rc:
                eprint("Client ns %s rank %d: PMIx_Lookup failed: %d"
                       % (myproc['nspace'], myproc['rank'], rc))
                return done(client, myproc)
            if not check(client, myproc, pdata[0], 2, PMIX_SIZE, 1234 + n):
                return done(client, myproc)
            eprint("Client ns %s rank %d: PUBLISH-LOOKUP BIGTEST "
                   "SUCCEEDED: %d" % (myproc['nspace'], myproc['rank'], n))

    rc = client.fence([proc], None)

    if 2 == myproc['rank']:
        keys = ["BIGTEST:%s.%u:%d" % (myproc['nspace'], myproc['rank'], 0),
                "BIGTEST:%s.%u:%d" % (myproc['nspace'], myproc['rank'], 1)]
        rc = client.unpublish(keys, None)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Unpublish failed: %d"
                   % (myproc['nspace'], myproc['rank'], rc))
            return done(client, myproc)
        # purge the rest
        rc = client.unpublish(None, None)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Unpublish purge failed: %d"
                   % (myproc['nspace'], myproc['rank'], rc))
            return done(client, myproc)
        # verify the purge - should not be found
        key = "BIGTEST:%s.%u:%d" % (myproc['nspace'], 2, 2)
        rc, pdata = client.lookup([{'key': key}], None)
        if PMIX_SUCCESS == rc:
            eprint("Client ns %s rank %d: Purge failed"
                   % (myproc['nspace'], myproc['rank']))
            return done(client, myproc)
        eprint("Client ns %s rank %d: Purge succeeded: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))

        # now publish something on different persistences
        for idx, (persistence, value) in enumerate(
                ((PMIX_PERSIST_PROC, 1), (PMIX_PERSIST_APP, 2),
                 (PMIX_PERSIST_SESSION, 3))):
            key = "RANGETEST:%s.%u:%d" % (myproc['nspace'],
                                          myproc['rank'], idx)
            rc = client.publish([{'key': key, 'value': value,
                                  'val_type': PMIX_SIZE},
                                 {'key': PMIX_PERSISTENCE,
                                  'value': persistence,
                                  'val_type': PMIX_PERSIST}])
            if PMIX_SUCCESS != rc:
                eprint("Client ns %s rank %d: PMIx_Publish failed: %d"
                       % (myproc['nspace'], myproc['rank'], rc))
                return done(client, myproc)

        # unpublish only those on the app persistence
        keys = ["RANGETEST:%s.%u:1" % (myproc['nspace'], myproc['rank'])]
        info = [{'key': PMIX_PERSISTENCE, 'value': PMIX_PERSIST_APP,
                 'val_type': PMIX_PERSIST}]
        rc = client.unpublish(keys, info)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Unpublish of app persistence "
                   "failed: %s" % (myproc['nspace'], myproc['rank'],
                                   client.error_string(rc)))
            return done(client, myproc)
        eprint("Client ns %s rank %d: PMIx_Unpublish of app persistence "
               "succeeded" % (myproc['nspace'], myproc['rank']))

        # verify the app persistence keys are gone
        key = "RANGETEST:%s.%u:1" % (myproc['nspace'], myproc['rank'])
        rc, pdata = client.lookup([{'key': key}], None)
        if PMIX_SUCCESS == rc:
            eprint("Client ns %s rank %d: PMIx_Lookup succeeded instead of "
                   "failed" % (myproc['nspace'], myproc['rank']))
            return done(client, myproc)
        eprint("Client ns %s rank %d: PMIx_Lookup of app persistence value "
               "correctly failed: %s" % (myproc['nspace'], myproc['rank'],
                                         client.error_string(rc)))

        # verify the session persistence key is still there
        key = "RANGETEST:%s.%u:2" % (myproc['nspace'], myproc['rank'])
        rc, pdata = client.lookup([{'key': key}], None)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Lookup failed: %s"
                   % (myproc['nspace'], myproc['rank'],
                      client.error_string(rc)))
            return done(client, myproc)
        eprint("Client ns %s rank %d: PMIx_Lookup of session persistence "
               "value succeeded" % (myproc['nspace'], myproc['rank']))

    return done(client, myproc)


def check(client, myproc, pdata, rank, val_type, value):
    """Check a lookup return for value and source."""
    if myproc['nspace'] != pdata['proc']['nspace']:
        eprint("Client ns %s rank %d: PMIx_Lookup returned wrong nspace: %s"
               % (myproc['nspace'], myproc['rank'], pdata['proc']['nspace']))
        return False
    if rank != pdata['proc']['rank']:
        eprint("Client ns %s rank %d: PMIx_Lookup returned wrong rank: %d"
               % (myproc['nspace'], myproc['rank'], pdata['proc']['rank']))
        return False
    if val_type != pdata['val_type']:
        eprint("Client ns %s rank %d: PMIx_Lookup returned wrong type: %s"
               % (myproc['nspace'], myproc['rank'],
                  client.data_type_string(pdata['val_type'])))
        return False
    if value != pdata['value']:
        eprint("Client ns %s rank %d: PMIx_Lookup returned wrong value: %lu"
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
