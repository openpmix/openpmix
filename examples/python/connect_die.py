#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/connect_die.c
#
# Exercise the server's lost-connection accounting for a collective.
#
# Every rank posts remote-scope data and commits it, which is what makes
# the client send endpoint info to the server on connect - appended to the
# tracker's info array after the collective-status slot. That is exactly
# the layout that used to make the lost-connection accounting write the
# status over the appended endpoint info.
#
# After the fence, every rank EXCEPT the last calls PMIx_Connect; the last
# rank leaves WITHOUT calling it (after a short delay, so the others have
# already entered the connect and their servers have built the local
# trackers) and WITHOUT calling finalize, so the server sees a dropped
# connection. The server must account for the departed participant and
# complete the connect on the survivors rather than hang.
#
# The whole job is the connect set (rather than a subset) so that, when
# launched across nodes, the victim's node always also hosts a surviving
# participant. Launch with a launcher that does not kill the job when the
# victim exits; under PRRTE that is `prterun --rtos recoverable ...`.
#
# Requires a minimum of 4 processes.

import os
import sys
import time

from examples import *


def main():
    client = PMIxClient()

    rc, myproc = client.init(None)
    if PMIX_SUCCESS != rc:
        eprint("Client: PMIx_Init failed:", client.error_string(rc))
        sys.exit(1)

    # get our job size
    proc = {'nspace': myproc['nspace'], 'rank': PMIX_RANK_WILDCARD}
    rc, val = client.get(proc, PMIX_JOB_SIZE, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get job size failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        sys.exit(1)
    nprocs = val['value']
    if nprocs < 4:
        if 0 == myproc['rank']:
            eprint("This example requires a minimum of 4 processes")
        sys.exit(1)
    eprint("Client %s:%d job size %d"
           % (myproc['nspace'], myproc['rank'], nprocs))

    # post some remote-scope data and commit it
    key = "connect-die-remote-%u" % myproc['rank']
    rc = client.put(PMIX_REMOTE, key,
                    {'value': 1000 + myproc['rank'], 'val_type': PMIX_UINT64})
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Put failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        sys.exit(1)
    rc = client.commit()
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Commit failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        sys.exit(1)

    # sync everyone up first - all ranks are alive for this fence
    rc = client.fence([proc], None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Fence failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        sys.exit(1)

    # the entire job is the connect set; the last rank is the victim
    victim = nprocs - 1

    if myproc['rank'] == victim:
        # the "victim": a participant that leaves before contributing.
        # Delay briefly so the other participants have entered the connect
        # and their servers have built the local trackers, then leave
        # without calling it - and without calling finalize, so the server
        # sees a dropped connection and must run its lost-connection
        # accounting. We exit with status 0 so the launcher does not abort
        # the whole job; the dropped socket, not the exit code, is what
        # exercises the server
        eprint("%d leaving BEFORE PMIx_Connect (simulated failure)"
               % myproc['rank'])
        time.sleep(2)
        os._exit(0)

    eprint("%d executing PMIx_Connect" % myproc['rank'])
    procs = [{'nspace': myproc['nspace'], 'rank': n} for n in range(nprocs)]

    rc = client.connect(procs, None)

    # The connect must COMPLETE rather than hang. It may return success or
    # a partial-success/lost-connection status depending on how the host
    # treats the departed participant; either way, reaching here is the
    # point of the test - the collective did not hang on the lost
    # participant, and the server did not corrupt the tracker's info array
    # while accounting for the loss
    eprint("%d Connect complete on survivors: status %s"
           % (myproc['rank'], client.error_string(rc)))

    client.finalize(None)
    eprint("Client ns %s rank %d: FINALIZED"
           % (myproc['nspace'], myproc['rank']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
