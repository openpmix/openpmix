#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/group_daemon_fail.c
#
# Lose a whole *daemon* rather than a single client. The last rank stays
# alive but never contributes to the construct, so the collective is still
# pending when the harness kills that rank's daemon. Every surviving
# member must see the construct abort rather than hang.
#
# This one needs an external harness to kill the victim's daemon while the
# construct is outstanding; run it by hand rather than under `make check`.
#
# Requires a minimum of 4 processes.

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
    # - it stays alive but never contributes, so the construct is pending
    # when the harness kills its daemon
    victim = nprocs - 1

    if myproc['rank'] == victim:
        eprint("%d alive but NOT joining the construct - awaiting daemon "
               "kill" % myproc['rank'])
        time.sleep(60)
        # if we are still here, the harness never killed our daemon
        return done(client, myproc)

    eprint("%d executing Group_construct" % myproc['rank'])
    procs = [{'nspace': myproc['nspace'], 'rank': n} for n in range(nprocs)]

    rc, results = client.group_construct("faultgroup", procs, None)

    if PMIX_GROUP_CONSTRUCT_ABORT == rc:
        eprint("%d construct ABORT after daemon loss: PASS" % myproc['rank'])
    else:
        eprint("%d construct returned %s, expected "
               "PMIX_GROUP_CONSTRUCT_ABORT: FAILED"
               % (myproc['rank'], client.error_string(rc)))

    return done(client, myproc)


def done(client, myproc):
    client.finalize(None)
    eprint("Client ns %s rank %d: FINALIZED"
           % (myproc['nspace'], myproc['rank']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
