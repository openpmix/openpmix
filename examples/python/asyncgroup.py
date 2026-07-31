#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/asyncgroup.c
#
# Build a group by invitation rather than by collective construct: rank 0
# invites ranks 2 and 3, which are sitting in an event handler for
# PMIX_GROUP_INVITED and accept with group_join_nb. Once the group is
# complete everyone fences across it and then tears it down.
#
# Requires a minimum of 4 processes.

import os
import socket
import sys
import threading

from examples import *

client = PMIxClient()
myproc = {}
invitedlock = MyLock()


def notification_fn(evhdlr, status, source, info, results):
    eprint("Client %s:%d NOTIFIED with status %s"
           % (myproc['nspace'], myproc['rank'], client.error_string(status)))
    return PMIX_EVENT_ACTION_COMPLETE, None


def grpcomplete(evhdlr, status, source, info, results):
    invitedlock.wakeup()
    return PMIX_EVENT_ACTION_COMPLETE, None


def invitefn(evhdlr, status, source, info, results):
    # if I am the leader, I can ignore this event
    if source == myproc:
        # mark the event chain as complete
        return PMIX_EVENT_ACTION_COMPLETE, None

    # search for grp id
    grp = find_key(info, PMIX_GROUP_ID)
    invitedlock.status = status
    rc = client.group_join_nb(grp, source, PMIX_GROUP_ACCEPT, None,
                              join_cbfunc, None)
    if PMIX_SUCCESS != rc:
        eprint("%s:%d Error in Group_join_nb: %s"
               % (myproc['nspace'], myproc['rank'],
                  client.error_string(rc)))
    # mark the event chain as complete
    return PMIX_EVENT_ACTION_COMPLETE, None


def join_cbfunc(status, results, cbdata):
    # the C version passes a NULL callback to Group_join_nb; the binding
    # requires a callable, so this is the equivalent no-op
    pass


def main():
    global myproc

    hostname = socket.gethostname()

    # init us
    rc, myproc = client.init(None)
    if PMIX_SUCCESS != rc:
        eprint("Client: PMIx_Init failed:", client.error_string(rc))
        sys.exit(1)
    eprint("[%d] Client ns %s rank %d: Running on %s"
           % (os.getpid(), myproc['nspace'], myproc['rank'], hostname))

    proc = {'nspace': myproc['nspace'], 'rank': PMIX_RANK_WILDCARD}

    # get our job size
    rc, val = client.get(proc, PMIX_JOB_SIZE, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get job size failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(rc, 0)
    nprocs = val['value']
    if nprocs < 4:
        if 0 == myproc['rank']:
            eprint("This example requires a minimum of 4 processes")
        sys.exit(1)
    eprint("Client %s:%d job size %d"
           % (myproc['nspace'], myproc['rank'], nprocs))

    # register our default errhandler
    rc, refid = client.register_event_handler(None, None, notification_fn)
    if PMIX_SUCCESS != rc:
        return done(rc, refid)

    # we need to register handlers for invitations
    rc, _ = client.register_event_handler([PMIX_GROUP_INVITED], None,
                                          invitefn)
    if PMIX_SUCCESS != rc:
        return done(rc, refid)

    if myproc['rank'] in (2, 3):
        # need to register for group complete event
        rc, _ = client.register_event_handler(
            [PMIX_GROUP_CONSTRUCT_COMPLETE], None, grpcomplete)
        if PMIX_SUCCESS != rc:
            return done(rc, refid)

    # call fence to sync
    rc = client.fence([proc], None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Fence failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(rc, refid)

    # rank=0 constructs a new group
    if 0 == myproc['rank']:
        eprint("%d executing Group_invite" % myproc['rank'])
        procs = [{'nspace': myproc['nspace'], 'rank': r} for r in (0, 2, 3)]
        rc, results = client.group_invite("ourgroup", procs, None)
        eprint("Client ns %s rank %d: Group invite complete with status %s!"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        if PMIX_SUCCESS != rc:
            sys.exit(1)
        eprint("%s:%d Execute fence across group"
               % (myproc['nspace'], myproc['rank']))
        grp = {'nspace': "ourgroup", 'rank': PMIX_RANK_WILDCARD}
        rc = client.fence([grp], None)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Fence across group failed: %d"
                   % (myproc['nspace'], myproc['rank'], rc))
            sys.exit(1)
        eprint("%d Executing Group_destruct" % myproc['rank'])
        rc = client.group_destruct("ourgroup", None)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Group_destruct failed: %s"
                   % (myproc['nspace'], myproc['rank'],
                      client.error_string(rc)))
            sys.exit(1)
    elif myproc['rank'] in (2, 3):
        # wait to be invited
        eprint("%s:%d waiting to join group"
               % (myproc['nspace'], myproc['rank']))
        invitedlock.wait()
        eprint("%s:%d Group complete - executing fence across group"
               % (myproc['nspace'], myproc['rank']))
        grp = {'nspace': "ourgroup", 'rank': PMIX_RANK_WILDCARD}
        rc = client.fence([grp], None)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Fence across group "
                   "failed: %s(%d)" % (myproc['nspace'], myproc['rank'],
                                       client.error_string(rc), rc))
            return done(rc, refid)
        eprint("%d Executing Group_destruct" % myproc['rank'])
        rc = client.group_destruct("ourgroup", None)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Group_destruct failed: %s"
                   % (myproc['nspace'], myproc['rank'],
                      client.error_string(rc)))
            return done(rc, refid)

    # call fence to sync
    rc = client.fence([proc], None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Fence failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))

    return done(rc, refid)


def done(rc, refid):
    # finalize us
    client.deregister_event_handler(refid)

    eprint("Client ns %s rank %d: Finalizing"
           % (myproc['nspace'], myproc['rank']))
    ret = client.finalize(None)
    if PMIX_SUCCESS != ret:
        eprint("Client ns %s rank %d:PMIx_Finalize failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(ret)))
        rc = ret
    else:
        eprint("Client ns %s rank %d:PMIx_Finalize successfully completed"
               % (myproc['nspace'], myproc['rank']))
    eprint("%s:%d COMPLETE" % (myproc['nspace'], myproc['rank']))
    return rc


if __name__ == '__main__':
    sys.exit(main())
