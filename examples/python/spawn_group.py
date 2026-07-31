#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/spawn_group.c
#
# Bootstrap a group spanning a parent job and a job it spawns. Rank 0 of
# each job declares PMIX_GROUP_BOOTSTRAP=2 and adds the rest of its own
# job via PMIX_GROUP_ADD_MEMBERS; every other rank simply constructs with
# no membership and waits to be assembled in.
#
# The child job is the same script, marked by CHILD in its environment.
#
# Takes an optional child size; requires a minimum of 2 processes.

import os
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

    maxprocs = 2
    if 1 < len(sys.argv):
        maxprocs = int(sys.argv[1])

    # init us
    rc, myproc = client.init(None)
    if PMIX_SUCCESS != rc:
        eprint("Client: PMIx_Init failed:", client.error_string(rc))
        sys.exit(0)
    eprint("Client ns %s rank %d: Running on host %s"
           % (myproc['nspace'], myproc['rank'], hostname))

    proc = {'nspace': myproc['nspace'], 'rank': PMIX_RANK_WILDCARD}

    # get our job size
    rc, val = client.get(proc, PMIX_JOB_SIZE, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get job size failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(0)
    nprocs = val['value']
    if nprocs < 2:
        if 0 == myproc['rank']:
            eprint("This example requires a minimum of 2 processes")
        return done(0)
    eprint("Client %s:%d job size %u"
           % (myproc['nspace'], myproc['rank'], nprocs))

    # register our default errhandler
    rc, refid = client.register_event_handler(None, None, notification_fn)
    if PMIX_SUCCESS != rc:
        return done(refid)

    results = []

    # the rest of our own job, named as one wildcard entry
    wildcard = [{'nspace': myproc['nspace'], 'rank': PMIX_RANK_WILDCARD}]

    # see if we are the parent job
    if os.getenv("CHILD") is None:
        # put some modex-like data
        tmp = "%s-%u-remote" % (myproc['nspace'], myproc['rank'])
        rc = client.put(PMIX_GLOBAL, tmp,
                        {'value': 1234 + myproc['rank'],
                         'val_type': PMIX_UINT64})
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Put failed: %d"
                   % (myproc['nspace'], myproc['rank'], rc))
            return done(refid)

        # commit it
        rc = client.commit()
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Commit failed: %s"
                   % (myproc['nspace'], myproc['rank'],
                      client.error_string(rc)))
            return done(refid)

        # do our own exchange
        rc = client.fence([proc], None)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Fence failed: %s"
                   % (myproc['nspace'], myproc['rank'],
                      client.error_string(rc)))
            return done(refid)

        # parent rank=0 will call spawn
        if 0 == myproc['rank']:
            me = os.path.abspath(sys.argv[0])
            app = {'cmd': me, 'argv': [os.path.basename(me)],
                   'maxprocs': maxprocs, 'env': ["CHILD=3"], 'info': []}

            eprint("Client ns %s rank %d: calling PMIx_Spawn"
                   % (myproc['nspace'], myproc['rank']))
            info = [{'key': PMIX_MAPBY, 'value': "node",
                     'val_type': PMIX_STRING}]
            rc, nsp2 = client.spawn(info, [app])
            if PMIX_SUCCESS != rc:
                eprint("Client ns %s rank %d: PMIx_Spawn failed: %d"
                       % (myproc['nspace'], myproc['rank'], rc))
                return done(refid)

            # now bootstrap the group: ask for a new context ID, declare
            # that two procs are bootstrapping, and add the rest of the
            # parent job
            info = [{'key': PMIX_GROUP_ASSIGN_CONTEXT_ID, 'value': True,
                     'val_type': PMIX_BOOL},
                    {'key': PMIX_GROUP_BOOTSTRAP, 'value': 2,
                     'val_type': PMIX_SIZE},
                    {'key': PMIX_GROUP_ADD_MEMBERS,
                     'value': {'type': PMIX_PROC, 'array': wildcard},
                     'val_type': PMIX_DATA_ARRAY}]

            # construct the group
            rc, results = client.group_construct("ourgroup", [myproc], info)
            if PMIX_SUCCESS != rc:
                eprint("Client ns %s rank %d: PMIx_Group_construct failed: %s"
                       % (myproc['nspace'], myproc['rank'],
                          client.error_string(rc)))
                return done(refid)
        else:
            # we will participate in the group assembly with the spawned
            # children
            rc, results = client.group_construct("ourgroup", [], None)
            if PMIX_SUCCESS != rc:
                eprint("Client ns %s rank %d: PMIx_Group_construct failed: %s"
                       % (myproc['nspace'], myproc['rank'],
                          client.error_string(rc)))
                return done(refid)
            # process results to get final membership
    else:
        # we are the child job - put some data
        tmp = "%s-%u-remote" % (myproc['nspace'], myproc['rank'])
        rc = client.put(PMIX_GLOBAL, tmp,
                        {'value': 1234 + myproc['rank'],
                         'val_type': PMIX_UINT64})
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Put failed: %d"
                   % (myproc['nspace'], myproc['rank'], rc))
            return done(refid)

        # commit it
        rc = client.commit()
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Commit failed: %s"
                   % (myproc['nspace'], myproc['rank'],
                      client.error_string(rc)))
            return done(refid)

        # do our own exchange
        rc = client.fence([proc], None)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Fence failed: %s"
                   % (myproc['nspace'], myproc['rank'],
                      client.error_string(rc)))
            return done(refid)

        # rank=0 will setup the group assembly with the parent
        if 0 == myproc['rank']:
            # two procs are performing the bootstrap; add the rest of the
            # child job
            info = [{'key': PMIX_GROUP_BOOTSTRAP, 'value': 2,
                     'val_type': PMIX_SIZE},
                    {'key': PMIX_GROUP_ADD_MEMBERS,
                     'value': {'type': PMIX_PROC, 'array': wildcard},
                     'val_type': PMIX_DATA_ARRAY}]

            # construct the group
            rc, results = client.group_construct("ourgroup", [myproc], info)
            if PMIX_SUCCESS != rc:
                eprint("Client ns %s rank %d: PMIx_Group_construct failed: %s"
                       % (myproc['nspace'], myproc['rank'],
                          client.error_string(rc)))
                return done(refid)
        else:
            # wait for group assembly to complete
            rc, results = client.group_construct("ourgroup", [], None)
            if PMIX_SUCCESS != rc:
                eprint("Client ns %s rank %d: PMIx_Group_construct failed: %s"
                       % (myproc['nspace'], myproc['rank'],
                          client.error_string(rc)))
                return done(refid)

    # we should have a single results object
    members = None
    if results:
        for item in results:
            if PMIX_GROUP_CONTEXT_ID == item['key']:
                eprint("%s:%d Group construct complete with status %s "
                       "KEY %s CID %lu"
                       % (myproc['nspace'], myproc['rank'],
                          client.error_string(PMIX_SUCCESS),
                          item['key'], item['value']))
            elif PMIX_GROUP_MEMBERSHIP == item['key']:
                members = item['value']['array']
                eprint("[%s:%u] NUM MEMBERS: %u MEMBERSHIP:"
                       % (myproc['nspace'], myproc['rank'], len(members)))
                for m in members:
                    eprint("\t%s:%u" % (m['nspace'], m['rank']))
        if members is None:
            eprint("%s:%d Membership not returned"
                   % (myproc['nspace'], myproc['rank']))
            return done(refid)
    else:
        eprint("%s:%d No returned results"
               % (myproc['nspace'], myproc['rank']))
        return done(refid)

    # check for modex data to have been exchanged
    tinfo = [{'key': PMIX_IMMEDIATE, 'value': True, 'val_type': PMIX_BOOL}]
    for member in members:
        # ignore my own nspace
        if member['nspace'] == myproc['nspace']:
            continue
        if PMIX_RANK_WILDCARD == member['rank']:
            # get the size of the other nspace
            other = {'nspace': member['nspace'], 'rank': PMIX_RANK_WILDCARD}
            rc, val = client.get(other, PMIX_JOB_SIZE, tinfo)
            if PMIX_SUCCESS != rc:
                eprint("Client ns %s rank %d: PMIx_Get job size failed for "
                       "nspace %s: %s"
                       % (myproc['nspace'], myproc['rank'],
                          member['nspace'], client.error_string(rc)))
                return done(refid)
            nprocs = val['value']
            # cycle across the procs and get their modex data
            for m in range(nprocs):
                other['rank'] = m
                tmp = "%s-%u-remote" % (other['nspace'], m)
                rc, val = client.get(other, tmp, tinfo)
                if PMIX_SUCCESS != rc:
                    eprint("Client ns %s rank %d: PMIx_Get %s failed: %s"
                           % (myproc['nspace'], myproc['rank'], tmp,
                              client.error_string(rc)))
                else:
                    eprint("Client ns %s rank %d: Get modex for proc %s:%u "
                           "returned %lu"
                           % (myproc['nspace'], myproc['rank'],
                              other['nspace'], m, val['value']))

    return done(refid)


def done(refid):
    # finalize us
    client.deregister_event_handler(refid)

    eprint("Client ns %s rank %d: Finalizing"
           % (myproc['nspace'], myproc['rank']))
    rc = client.finalize(None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d:PMIx_Finalize failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
    else:
        eprint("Client ns %s rank %d:PMIx_Finalize successfully completed"
               % (myproc['nspace'], myproc['rank']))
    eprint("%s:%d COMPLETE" % (myproc['nspace'], myproc['rank']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
