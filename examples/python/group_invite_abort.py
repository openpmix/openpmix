#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/group_invite_abort.c
#
# The all-or-nothing counterpart of group_invite_decline: one invitee
# declines, but the invite does NOT carry PMIX_GROUP_OPTIONAL. A single
# decline must therefore abort the whole construct:
#   - the leader's invite must return PMIX_GROUP_CONSTRUCT_ABORT;
#   - every invited participant must receive the
#     PMIX_GROUP_CONSTRUCT_ABORT event; and
#   - nobody may be told the (nonexistent) group completed.
#
# Requires a minimum of 3 processes.

import sys
import threading

from examples import *

GROUP_ID = "idabortgroup"

client = PMIxClient()
myproc = {}
decliner = 0
complete_seen = threading.Event()
abort_seen = threading.Event()


def join_cbfunc(status, results, cbdata):
    # the C version passes a NULL callback to Group_join_nb; the binding
    # requires a callable, so this is the equivalent no-op
    pass


# PMIX_GROUP_INVITED handler: accept, unless we are the designated
# decliner
def invite_handler(evhdlr, status, source, info, results):
    if source == myproc:
        # our own invitation - ignore
        return PMIX_EVENT_ACTION_COMPLETE, None

    grp = find_key(info, PMIX_GROUP_ID)
    if myproc['rank'] == decliner:
        opt = PMIX_GROUP_DECLINE
        eprint("%s:%d INVITED - declining"
               % (myproc['nspace'], myproc['rank']))
    else:
        opt = PMIX_GROUP_ACCEPT
        eprint("%s:%d INVITED - accepting"
               % (myproc['nspace'], myproc['rank']))
    rc = client.group_join_nb(grp, source, opt, None, join_cbfunc, None)
    if PMIX_SUCCESS != rc:
        eprint("%s:%d ERROR in PMIx_Group_join_nb: %s"
               % (myproc['nspace'], myproc['rank'],
                  client.error_string(rc)))
    return PMIX_EVENT_ACTION_COMPLETE, None


# PMIX_GROUP_CONSTRUCT_COMPLETE handler - must NOT fire in this exerciser
def complete_handler(evhdlr, status, source, info, results):
    eprint("%s:%d ERROR - unexpected PMIX_GROUP_CONSTRUCT_COMPLETE"
           % (myproc['nspace'], myproc['rank']))
    complete_seen.set()
    return PMIX_EVENT_ACTION_COMPLETE, None


# PMIX_GROUP_CONSTRUCT_ABORT handler - the construct was aborted
def abort_handler(evhdlr, status, source, info, results):
    eprint("%s:%d NOTIFIED that group construct was ABORTED"
           % (myproc['nspace'], myproc['rank']))
    abort_seen.set()
    return PMIX_EVENT_ACTION_COMPLETE, None


def register_handler(code, fn):
    rc, _ = client.register_event_handler([code], None, fn)
    return rc


def main():
    global myproc, decliner

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
    if nprocs < 3:
        if 0 == myproc['rank']:
            eprint("This example requires a minimum of 3 processes")
        sys.exit(1)
    decliner = nprocs - 1
    eprint("Client %s:%d job size %d (decliner is rank %d)"
           % (myproc['nspace'], myproc['rank'], nprocs, decliner))

    rc = register_handler(PMIX_GROUP_INVITED, invite_handler)
    if PMIX_SUCCESS != rc:
        eprint("%d: failed to register invited handler: %s"
               % (myproc['rank'], client.error_string(rc)))
        return done(rc)
    rc = register_handler(PMIX_GROUP_CONSTRUCT_COMPLETE, complete_handler)
    if PMIX_SUCCESS != rc:
        eprint("%d: failed to register complete handler: %s"
               % (myproc['rank'], client.error_string(rc)))
        return done(rc)
    rc = register_handler(PMIX_GROUP_CONSTRUCT_ABORT, abort_handler)
    if PMIX_SUCCESS != rc:
        eprint("%d: failed to register abort handler: %s"
               % (myproc['rank'], client.error_string(rc)))
        return done(rc)

    # sync so every rank has its handlers in place before the leader
    # invites
    rc = client.fence([proc], None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Fence failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(rc)

    if 0 == myproc['rank']:
        # invite the whole job with no PMIX_GROUP_OPTIONAL, so a single
        # decline must abort the construct outright
        eprint("%d executing Group_invite (all-or-nothing) for the whole job"
               % myproc['rank'])
        procs = [{'nspace': myproc['nspace'], 'rank': n}
                 for n in range(nprocs)]
        rc, results = client.group_invite(GROUP_ID, procs, None)
        eprint("%d Group invite returned: %s"
               % (myproc['rank'], client.error_string(rc)))
        if PMIX_GROUP_CONSTRUCT_ABORT != rc:
            eprint("Client ns %s rank %d: FAILED - invite returned %s, "
                   "expected PMIX_GROUP_CONSTRUCT_ABORT"
                   % (myproc['nspace'], myproc['rank'],
                      client.error_string(rc)))
            return done(PMIX_ERROR)
        eprint("%d PMIx_Group_invite returned CONSTRUCT_ABORT: PASS"
               % myproc['rank'])

    # every invited participant must receive the abort event and none may
    # be told the (nonexistent) group completed
    if not abort_seen.wait(timeout=10):
        eprint("Client ns %s rank %d: FAILED - never received "
               "PMIX_GROUP_CONSTRUCT_ABORT"
               % (myproc['nspace'], myproc['rank']))
        return done(PMIX_ERR_TIMEOUT)
    if complete_seen.is_set():
        eprint("Client ns %s rank %d: FAILED - received CONSTRUCT_COMPLETE "
               "for an aborted construct"
               % (myproc['nspace'], myproc['rank']))
        return done(PMIX_ERROR)
    eprint("%d PMIX_GROUP_CONSTRUCT_ABORT received: PASS" % myproc['rank'])
    rc = PMIX_SUCCESS

    # final job-level sync so every rank tears down together
    rc = client.fence([proc], None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: final PMIx_Fence failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))

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
