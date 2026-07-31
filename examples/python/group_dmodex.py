#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/group_dmodex.c
#
# This test simulates the way Open MPI uses PMIx_Group_construct to
# implement the MPI4 functions:
# - MPI_Comm_create_from_group
# - MPI_Intercomm_create_from_groups
#
# The group is constructed to obtain a context ID, each member puts a
# value keyed by that ID, the group is destructed, and then every member
# reads back everyone else's value - so the reads are satisfied by direct
# modex after the group is gone.
#
# Requires a minimum of 4 processes.

import sys

from examples import *

client = PMIxClient()
myproc = {}
GET_TIMEOUT = 600   # default 600 secs to get remote data


def notification_fn(evhdlr, status, source, info, results):
    eprint("Client %s:%d NOTIFIED with status %d"
           % (myproc['nspace'], myproc['rank'], status))
    return PMIX_EVENT_ACTION_COMPLETE, None


def main():
    global myproc

    # init us
    rc, myproc = client.init(None)
    if PMIX_SUCCESS != rc:
        eprint("Client: PMIx_Init failed:", client.error_string(rc))
        sys.exit(1)
    eprint("Client ns %s rank %d: Running"
           % (myproc['nspace'], myproc['rank']))

    proc = {'nspace': myproc['nspace'], 'rank': PMIX_RANK_WILDCARD}

    # get our job size
    rc, val = client.get(proc, PMIX_JOB_SIZE, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get universe size failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(rc, 0)
    nprocs = val['value']
    if nprocs < 4:
        if 0 == myproc['rank']:
            eprint("This example equires a minimum of 4 processes")
        sys.exit(1)
    eprint("Client %s:%d job size %d"
           % (myproc['nspace'], myproc['rank'], nprocs))

    # register our default errhandler
    rc, refid = client.register_event_handler(None, None, notification_fn)
    eprint("Client %s:%d ERRHANDLER REGISTRATION CALLBACK CALLED WITH "
           "STATUS %d, ref=%d"
           % (myproc['nspace'], myproc['rank'], rc, refid))
    if PMIX_SUCCESS != rc:
        return done(rc, refid)

    # call fence to sync
    rc = client.fence([proc], None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Fence failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(rc, refid)

    procs = [{'nspace': myproc['nspace'], 'rank': n} for n in range(nprocs)]
    info = [{'key': PMIX_GROUP_ASSIGN_CONTEXT_ID, 'value': True,
             'val_type': PMIX_BOOL}]
    rc, results = client.group_construct("ourgroup", procs, info)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Group_construct failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(rc, refid)
    # check the results
    cid = None
    for item in results:
        if key_is(item['key'], PMIX_GROUP_CONTEXT_ID):
            cid = item['value']
            eprint("%d Group construct complete with status %s CID %lu"
                   % (myproc['rank'], client.error_string(PMIX_SUCCESS), cid))
            break
    if cid is None:
        eprint("%d Group construct complete, but no CID returned"
               % myproc['rank'])
        return done(rc, refid)

    # put some data
    tmp = "%s-%lu-%d-remote" % (myproc['nspace'], cid, myproc['rank'])
    rc = client.put(PMIX_GLOBAL, tmp,
                    {'value': 1234 + myproc['rank'], 'val_type': PMIX_UINT64})
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Put failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(rc, refid)

    # commit the data to the server
    rc = client.commit()
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Commit failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(rc, refid)

    # destruct the group
    rc = client.group_destruct("ourgroup", None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Group_destruct failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(rc, refid)

    tinfo = [{'key': PMIX_TIMEOUT, 'value': GET_TIMEOUT,
              'val_type': PMIX_UINT32}]
    for n in range(nprocs):
        proc['rank'] = n
        tmp = "%s-%lu-%d-remote" % (myproc['nspace'], cid, n)
        rc, val = client.get(proc, tmp, tinfo)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Get %s failed: %d"
                   % (myproc['nspace'], n, tmp, rc))
            return done(rc, refid)
        if PMIX_UINT64 != val['val_type']:
            eprint("%s:%d: PMIx_Get Key %s returned wrong type: %d"
                   % (myproc['nspace'], myproc['rank'], tmp,
                      val['val_type']))
            return done(rc, refid)
        if (1234 + n) != val['value']:
            eprint("%s:%d: PMIx_Get Key %s returned wrong value: %lu"
                   % (myproc['nspace'], myproc['rank'], tmp, val['value']))
            return done(rc, refid)

    return done(rc, refid)


def done(rc, refid):
    # finalize us
    status = client.deregister_event_handler(refid)
    eprint("Client %s:%d OP CALLBACK CALLED WITH STATUS %d"
           % (myproc['nspace'], myproc['rank'], status))

    ret = client.finalize(None)
    if PMIX_SUCCESS != ret:
        eprint("Client ns %s rank %d:PMIx_Finalize failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(ret)))
        rc = ret
    eprint("%s:%d COMPLETE" % (myproc['nspace'], myproc['rank']))
    return rc


if __name__ == '__main__':
    sys.exit(main())
