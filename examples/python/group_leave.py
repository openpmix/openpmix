#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/group_leave.c
#
# A member departs a group voluntarily. Every surviving member has
# registered a handler for PMIX_GROUP_LEFT and must receive that
# notification - if it does not, this test fails (rather than hangs) via a
# bounded wait.
#
# The whole job is the membership (rather than a subset) so that, when
# launched across nodes, the departing rank's node always also hosts a
# surviving member, exercising both the local and remote
# notification-delivery paths.
#
# Requires a minimum of 2 processes.

import sys
import threading

from examples import *

client = PMIxClient()
myproc = {}
left_seen = threading.Event()
departed = {}


# handler for the PMIX_GROUP_LEFT event - records that we were notified
# and who departed
def left_handler(evhdlr, status, source, info, results):
    global departed

    departed = find_key(info, PMIX_EVENT_AFFECTED_PROC) or {}
    eprint("%s:%d NOTIFIED that %s:%d left the group"
           % (myproc['nspace'], myproc['rank'], departed.get('nspace'),
              departed.get('rank', PMIX_RANK_UNDEF)))
    left_seen.set()

    # we must always continue the event chain
    return PMIX_EVENT_ACTION_COMPLETE, None


def main():
    global myproc

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
    if nprocs < 2:
        if 0 == myproc['rank']:
            eprint("This example requires a minimum of 2 processes")
        sys.exit(1)
    eprint("Client %s:%d job size %d"
           % (myproc['nspace'], myproc['rank'], nprocs))

    # register our handler for the group-left event
    rc, _ = client.register_event_handler([PMIX_GROUP_LEFT], None,
                                          left_handler)
    if PMIX_SUCCESS != rc:
        eprint("%d: failed to register handler: %s"
               % (myproc['rank'], client.error_string(rc)))
        return done(rc)

    # sync everyone up first
    rc = client.fence([proc], None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Fence failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(rc)

    # the entire job is the group membership; the last rank is the one
    # that will voluntarily leave
    victim = nprocs - 1

    eprint("%d executing Group_construct" % myproc['rank'])
    procs = [{'nspace': myproc['nspace'], 'rank': n} for n in range(nprocs)]
    rc, results = client.group_construct("lgroup", procs, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Group_construct failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(rc)

    # sync so everyone has completed construction before anyone leaves
    rc = client.fence([proc], None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: post-construct PMIx_Fence failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(rc)

    if myproc['rank'] == victim:
        # voluntarily depart the group - this must generate the
        # PMIX_GROUP_LEFT event to the remaining members and return
        # success once locally generated
        eprint("%d executing Group_leave" % myproc['rank'])
        rc = client.group_leave("lgroup", None)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Group_leave FAILED: %s"
                   % (myproc['nspace'], myproc['rank'],
                      client.error_string(rc)))
            return done(rc)
        eprint("%d Group_leave complete: SUCCESS" % myproc['rank'])
    else:
        # a surviving member - wait (bounded) to be notified of the
        # departure
        if not left_seen.wait(timeout=10):
            eprint("Client ns %s rank %d: FAILED - never received "
                   "PMIX_GROUP_LEFT" % (myproc['nspace'], myproc['rank']))
            return done(PMIX_ERR_TIMEOUT)
        if departed.get('rank') != victim:
            eprint("Client ns %s rank %d: FAILED - wrong departed rank %d "
                   "(expected %d)" % (myproc['nspace'], myproc['rank'],
                                      departed.get('rank', PMIX_RANK_UNDEF),
                                      victim))
            return done(PMIX_ERROR)
        eprint("%d PMIX_GROUP_LEFT correctly received: PASS"
               % myproc['rank'])

    return done(rc)


def done(rc):
    eprint("Client ns %s rank %d: Finalizing"
           % (myproc['nspace'], myproc['rank']))
    client.finalize(None)
    eprint("%s:%d COMPLETE (rc %s)"
           % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
    return 0 if PMIX_SUCCESS == rc else 1


if __name__ == '__main__':
    sys.exit(main())
