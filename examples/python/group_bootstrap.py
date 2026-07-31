#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/group_bootstrap.c
#
# Bootstrap a group without every member knowing the full membership up
# front. Ranks 0 and 3 each construct with only themselves as the named
# member, declaring PMIX_GROUP_BOOTSTRAP=2 (two procs are bootstrapping)
# and naming one extra rank apiece via PMIX_GROUP_ADD_MEMBERS. Ranks 4
# and 5 join by constructing with no membership at all.
#
# Requires a minimum of 6 processes.

import socket
import sys

from examples import *

client = PMIxClient()
myproc = {}


def notification_fn(evhdlr, status, source, info, results):
    eprint("Client %s:%d NOTIFIED with status %d"
           % (myproc['nspace'], myproc['rank'], status))
    return PMIX_EVENT_ACTION_COMPLETE, None


def main():
    global myproc

    hostname = socket.gethostname()

    # init us
    rc, myproc = client.init(None)
    if PMIX_SUCCESS != rc:
        eprint("Client: PMIx_Init failed:", client.error_string(rc))
        sys.exit(1)
    eprint("Client ns %s rank %d: Running on host %s"
           % (myproc['nspace'], myproc['rank'], hostname))

    proc = {'nspace': myproc['nspace'], 'rank': PMIX_RANK_WILDCARD}

    # get our job size
    rc, val = client.get(proc, PMIX_JOB_SIZE, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get job size failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(rc, 0)
    nprocs = val['value']
    if nprocs < 6:
        if 0 == myproc['rank']:
            eprint("This example with add-members requires a minimum of 6 "
                   "processes")
        sys.exit(1)
    eprint("Client %s:%d job size %u"
           % (myproc['nspace'], myproc['rank'], nprocs))

    # register our default errhandler
    rc, refid = client.register_event_handler(None, None, notification_fn)
    if PMIX_SUCCESS != rc:
        return done(rc, refid)

    # call fence to sync
    rc = client.fence([proc], None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Fence failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(rc, refid)

    results = []
    parray = None

    # rank=0 and 3 bootstrap a new group
    if myproc['rank'] in (0, 3):
        eprint("%d executing Group_construct" % myproc['rank'])
        # two procs are performing the bootstrap
        added = 4 if 3 == myproc['rank'] else 5
        eprint("[%u]: Adding members" % myproc['rank'])
        info = [{'key': PMIX_GROUP_ASSIGN_CONTEXT_ID, 'value': True,
                 'val_type': PMIX_BOOL},
                {'key': PMIX_GROUP_BOOTSTRAP, 'value': 2,
                 'val_type': PMIX_SIZE},
                {'key': PMIX_GROUP_ADD_MEMBERS,
                 'value': {'type': PMIX_PROC,
                           'array': [{'nspace': myproc['nspace'],
                                      'rank': added}]},
                 'val_type': PMIX_DATA_ARRAY}]
        rc, results = client.group_construct("ourgroup", [myproc], info)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Group_construct failed: %s"
                   % (myproc['nspace'], myproc['rank'],
                      client.error_string(rc)))
            return done(rc, refid)
    elif myproc['rank'] in (4, 5):
        eprint("%d executing Group_construct" % myproc['rank'])
        rc, results = client.group_construct("ourgroup", [], None)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Group_construct failed: %s"
                   % (myproc['nspace'], myproc['rank'],
                      client.error_string(rc)))
            return done(rc, refid)
    eprint("%d GROUP CONSTRUCT COMPLETE" % myproc['rank'])

    if myproc['rank'] in (0, 3, 4, 5):
        # we should have a single results object
        if results:
            for item in results:
                if key_is(item['key'], PMIX_GROUP_CONTEXT_ID):
                    eprint("%d Group construct complete with status %s "
                           "KEY %s CID %lu"
                           % (myproc['rank'],
                              client.error_string(PMIX_SUCCESS),
                              as_key(item['key']), item['value']))
                elif key_is(item['key'], PMIX_GROUP_MEMBERSHIP):
                    parray = item['value']['array']
                    eprint("[%u] NUM MEMBERS: %u MEMBERSHIP:"
                           % (myproc['rank'], len(parray)))
                    for m in parray:
                        eprint("\t%s:%u" % (m['nspace'], m['rank']))
        else:
            eprint("%d Group construct complete, but no results returned"
                   % myproc['rank'])
        if parray is None:
            eprint("%d NULL proc array" % myproc['rank'])
            return done(rc, refid)

        eprint("%d Executing group fence" % myproc['rank'])
        grp = {'nspace': "ourgroup", 'rank': PMIX_RANK_WILDCARD}
        rc = client.fence([grp], None)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Fence failed: %d"
                   % (myproc['nspace'], myproc['rank'], rc))
            return done(rc, refid)

        eprint("%d executing Group_destruct" % myproc['rank'])
        rc = client.group_destruct("ourgroup", None)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Group_destruct failed: %s"
                   % (myproc['nspace'], myproc['rank'],
                      client.error_string(rc)))
            return done(rc, refid)

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
