#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/group_construct_abort.c
#
# The counterpart of group_die with fault tolerance turned OFF. The last
# rank leaves before contributing to the construct and without calling
# finalize. Because the construct does NOT carry
# PMIX_GROUP_FT_COLLECTIVE, the loss of a required member must abort the
# construct on every surviving member, across all servers - rather than
# completing it on a reduced membership.
#
# Launch with a launcher that does not kill the job when the victim
# exits; under PRRTE that is `prterun --rtos recoverable ...`.
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
        # leave before contributing, without finalizing, so the server
        # sees a dropped connection. Exit 0 so the launcher does not abort
        # the job
        eprint("%d leaving BEFORE group construct (simulated failure)"
               % myproc['rank'])
        time.sleep(2)
        os._exit(0)

    eprint("%d executing Group_construct (no FT_COLLECTIVE)"
           % myproc['rank'])
    procs = [{'nspace': myproc['nspace'], 'rank': n} for n in range(nprocs)]

    # No PMIX_GROUP_FT_COLLECTIVE: the loss of a required member must
    # abort the construct on every surviving member, across all servers
    rc, results = client.group_construct("cabortgroup", procs, None)

    if PMIX_GROUP_CONSTRUCT_ABORT == rc:
        eprint("%d construct ABORT on member loss: PASS" % myproc['rank'])
    else:
        eprint("%d construct returned %s, expected "
               "PMIX_GROUP_CONSTRUCT_ABORT: FAILED"
               % (myproc['rank'], client.error_string(rc)))

    client.finalize(None)
    eprint("Client ns %s rank %d: FINALIZED"
           % (myproc['nspace'], myproc['rank']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
