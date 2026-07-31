#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/group_invite_decline.c
#
# One invitee explicitly DECLINES the invitation. The invite carries no
# timeout, so the decline itself - not a timer - must resolve the
# construct. With PMIX_GROUP_OPTIONAL set, the library must:
#   - fire PMIX_GROUP_INVITE_FAILED at the leader naming the decliner;
#   - complete the invite on the members that DID accept (reduced
#     membership) rather than hang or error; and
#   - deliver PMIX_GROUP_CONSTRUCT_COMPLETE only to those members.
#
# Requires a minimum of 3 processes.

import sys
import threading

from examples import *

GROUP_ID = "idclgroup"

client = PMIxClient()
myproc = {}
decliner = 0
complete_seen = threading.Event()
failed_seen = threading.Event()
failed_proc = {}


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


# PMIX_GROUP_CONSTRUCT_COMPLETE handler
def complete_handler(evhdlr, status, source, info, results):
    eprint("%s:%d NOTIFIED that group construct is complete"
           % (myproc['nspace'], myproc['rank']))
    complete_seen.set()
    return PMIX_EVENT_ACTION_COMPLETE, None


# PMIX_GROUP_INVITE_FAILED handler (leader) - records who declined
def failed_handler(evhdlr, status, source, info, results):
    global failed_proc

    failed_proc = find_key(info, PMIX_EVENT_AFFECTED_PROC) or {}
    eprint("%s:%d NOTIFIED that %s:%d declined the invitation"
           % (myproc['nspace'], myproc['rank'], failed_proc.get('nspace'),
              failed_proc.get('rank', PMIX_RANK_UNDEF)))
    failed_seen.set()
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
    # the last rank is the one that will decline the invitation
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
    if 0 == myproc['rank']:
        rc = register_handler(PMIX_GROUP_INVITE_FAILED, failed_handler)
        if PMIX_SUCCESS != rc:
            eprint("%d: failed to register invite-failed handler: %s"
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
        # invite the whole job - deliberately with NO timeout, so the
        # decline itself (not a timer) must resolve the construct. Mark
        # participation PMIX_GROUP_OPTIONAL so the decline yields a
        # reduced group rather than aborting the construct
        eprint("%d executing Group_invite (optional, no timeout) for the "
               "whole job" % myproc['rank'])
        procs = [{'nspace': myproc['nspace'], 'rank': n}
                 for n in range(nprocs)]
        tinfo = [{'key': PMIX_GROUP_OPTIONAL, 'value': True,
                  'val_type': PMIX_BOOL}]
        rc, results = client.group_invite(GROUP_ID, procs, tinfo)
        eprint("%d Group invite returned: %s"
               % (myproc['rank'], client.error_string(rc)))
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Group_invite FAILED: %s"
                   % (myproc['nspace'], myproc['rank'],
                      client.error_string(rc)))
            return done(rc)
        # we must have been told the non-responder failed to answer
        if not failed_seen.is_set():
            eprint("Client ns %s rank %d: FAILED - never received "
                   "PMIX_GROUP_INVITE_FAILED"
                   % (myproc['nspace'], myproc['rank']))
            return done(PMIX_ERROR)
        if failed_proc.get('rank') != decliner:
            eprint("Client ns %s rank %d: FAILED - wrong declined rank %d "
                   "(expected %d)"
                   % (myproc['nspace'], myproc['rank'],
                      failed_proc.get('rank', PMIX_RANK_UNDEF), decliner))
            return done(PMIX_ERROR)
        eprint("%d PMIX_GROUP_INVITE_FAILED for decliner: PASS"
               % myproc['rank'])
    elif myproc['rank'] == decliner:
        eprint("%d declined the group (not a member)" % myproc['rank'])

    # the leader and the accepting members must be notified of completion;
    # the decliner must NOT be (it is not part of the group)
    if myproc['rank'] != decliner:
        if not complete_seen.wait(timeout=10):
            eprint("Client ns %s rank %d: FAILED - never received "
                   "PMIX_GROUP_CONSTRUCT_COMPLETE"
                   % (myproc['nspace'], myproc['rank']))
            return done(PMIX_ERR_TIMEOUT)
        eprint("%d PMIX_GROUP_CONSTRUCT_COMPLETE received: PASS"
               % myproc['rank'])

    # final job-level sync so every rank (including the decliner) tears
    # down together
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
