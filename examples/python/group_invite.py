#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/group_invite.c
#
# Form a group by invitation:
#   - The leader (rank 0) invites the whole job, itself included.
#   - Every other rank has registered a PMIX_GROUP_INVITED handler and
#     responds by accepting via group_join_nb (the non-blocking form is
#     mandatory: the handler runs on the library progress thread, so the
#     blocking form would deadlock).
#   - Every rank has registered a PMIX_GROUP_CONSTRUCT_COMPLETE handler
#     and must receive that event once the group forms - the leader
#     because it is a member, the invitees because they joined. A rank
#     that never receives it fails (via a bounded wait) rather than
#     hanging.
#   - Once formed, every member fences across the group (identified by
#     {grpid, PMIX_RANK_WILDCARD}) to prove the group is usable, then the
#     leader destructs it.
#
# The invite model relies solely on the event-notification subsystem, so
# launching the job across nodes exercises the cross-server notification
# path for both the invitations and the completion broadcast. No member is
# lost in this test, so no --rtos recoverable is required. See
# docs/how-things-work/sets_groups/group_construct.rst.
#
# Requires a minimum of 2 processes.

import sys
import threading

from examples import *

GROUP_ID = "invgroup"

client = PMIxClient()
myproc = {}
complete_seen = threading.Event()


def join_cbfunc(status, results, cbdata):
    # the C version passes a NULL callback to Group_join_nb; the binding
    # requires a callable, so this is the equivalent no-op
    pass


# PMIX_GROUP_INVITED handler: an invitee accepts the invitation. The
# leader sees its own invitation reflected back (source == self) and
# ignores it.
def invite_handler(evhdlr, status, source, info, results):
    # if I am the leader, ignore my own invitation and let the chain
    # proceed
    if source == myproc:
        return PMIX_EVENT_ACTION_COMPLETE, None

    # find the group id carried on the invitation and accept it
    grp = find_key(info, PMIX_GROUP_ID)
    eprint("%s:%d INVITED to group %s by %s:%d - accepting"
           % (myproc['nspace'], myproc['rank'],
              "(unknown)" if grp is None else grp,
              source['nspace'], source['rank']))
    rc = client.group_join_nb(grp, source, PMIX_GROUP_ACCEPT, None,
                              join_cbfunc, None)
    if PMIX_SUCCESS != rc:
        eprint("%s:%d ERROR in PMIx_Group_join_nb: %s"
               % (myproc['nspace'], myproc['rank'],
                  client.error_string(rc)))

    # always continue the event chain
    return PMIX_EVENT_ACTION_COMPLETE, None


# PMIX_GROUP_CONSTRUCT_COMPLETE handler: the group finished forming
def complete_handler(evhdlr, status, source, info, results):
    eprint("%s:%d NOTIFIED that group construct is complete"
           % (myproc['nspace'], myproc['rank']))
    complete_seen.set()
    return PMIX_EVENT_ACTION_COMPLETE, None


def register_handler(code, fn):
    """Register one event handler for a single code.

    Registration blocks in Python, so unlike the C version there is no
    registration callback to wait on.
    """
    rc, _ = client.register_event_handler([code], None, fn)
    return rc


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

    # register handlers for the invitation and the completion events
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

    # sync so every rank has its handlers in place before anyone invites
    rc = client.fence([proc], None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Fence failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(rc)

    if 0 == myproc['rank']:
        # the leader invites the whole job (itself included) to the group
        eprint("%d executing Group_invite for the whole job"
               % myproc['rank'])
        procs = [{'nspace': myproc['nspace'], 'rank': n}
                 for n in range(nprocs)]
        rc, results = client.group_invite(GROUP_ID, procs, None)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Group_invite FAILED: %s"
                   % (myproc['nspace'], myproc['rank'],
                      client.error_string(rc)))
            return done(rc)
        eprint("%d Group invite complete with status PMIX_SUCCESS"
               % myproc['rank'])
    else:
        # an invitee: our invite_handler accepts asynchronously. Wait
        # (bounded) for the group to finish forming rather than hang
        eprint("%s:%d waiting to be invited and to join the group"
               % (myproc['nspace'], myproc['rank']))

    # every member (leader and invitees) must receive CONSTRUCT_COMPLETE
    if not complete_seen.wait(timeout=10):
        eprint("Client ns %s rank %d: FAILED - never received "
               "PMIX_GROUP_CONSTRUCT_COMPLETE"
               % (myproc['nspace'], myproc['rank']))
        return done(PMIX_ERR_TIMEOUT)
    eprint("%d PMIX_GROUP_CONSTRUCT_COMPLETE received: PASS"
           % myproc['rank'])

    # prove the group is usable: fence across it by its group id
    grp = {'nspace': GROUP_ID, 'rank': PMIX_RANK_WILDCARD}
    rc = client.fence([grp], None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Fence across group FAILED: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(rc)
    eprint("%d group fence complete" % myproc['rank'])

    # tear the group back down. Destruct is a collective over the group
    # membership, so every member (leader and invitees alike) must call it
    eprint("%d executing Group_destruct" % myproc['rank'])
    rc = client.group_destruct(GROUP_ID, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Group_destruct FAILED: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(rc)

    # final sync across the job
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
