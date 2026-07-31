#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/group_die.c
#
# Exercise the server's lost-connection accounting for a group construct.
#
# The last rank leaves before contributing to the construct (after a short
# delay, so the others have entered it and their servers have built the
# local trackers) and WITHOUT calling finalize, so the server sees a
# dropped connection. The server must account for the departed member and
# complete the construct on the survivors rather than hang. Before the
# server tracked group participation by identity this would hang, because
# the group's local phase never reached its expected count. See
# docs/how-things-work/collectives.
#
# The whole job is the membership (rather than a subset) so that, when
# launched across nodes, the victim's node always also hosts a surviving
# member. Launch with a launcher that does not kill the job when the
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

    # sync everyone up first - all ranks are alive for this fence
    rc = client.fence([proc], None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Fence failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        sys.exit(1)

    # the entire job is the group membership; the last rank is the victim
    victim = nprocs - 1

    if myproc['rank'] == victim:
        # the "victim": a member that leaves before contributing. Delay
        # briefly so the other members have entered the construct and
        # their servers have built the local trackers, then leave without
        # calling it - and without calling finalize, so the server sees a
        # dropped connection and must run its lost-connection accounting.
        # We exit with status 0 so the launcher does not abort the whole
        # job; the dropped socket, not the exit code, is what exercises
        # the server
        eprint("%d leaving BEFORE group construct (simulated failure)"
               % myproc['rank'])
        time.sleep(2)
        os._exit(0)

    eprint("%d executing Group_construct" % myproc['rank'])
    procs = [{'nspace': myproc['nspace'], 'rank': n} for n in range(nprocs)]
    info = [{'key': PMIX_GROUP_ASSIGN_CONTEXT_ID, 'value': True,
             'val_type': PMIX_BOOL}]

    rc, results = client.group_construct("diegroup", procs, info)

    # The construct must COMPLETE rather than hang. It may return success
    # or a partial-success/lost-connection status depending on how the
    # host treats the departed member; either way, reaching here is the
    # point of the test - the collective did not hang on the lost
    # participant
    eprint("%d Group construct complete on survivors: status %s"
           % (myproc['rank'], client.error_string(rc)))

    client.finalize(None)
    eprint("Client ns %s rank %d: FINALIZED"
           % (myproc['nspace'], myproc['rank']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
