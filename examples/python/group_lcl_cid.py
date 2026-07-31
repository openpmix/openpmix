#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/group_lcl_cid.c
#
# Each member contributes a local context ID to the group construct, via a
# PMIX_GROUP_INFO array nested inside the construct's directives. After
# the group is destructed, every member reads back every other member's
# local CID, qualifying the get with the group's assigned context ID.
#
# The C version builds those nested directives with the PMIx_Info_list_*
# builder. That family is deliberately not bound: in Python the nested
# array is just a list of dicts inside a value dict.
#
# Requires a minimum of 4 processes.

import os
import socket
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

    hostname = socket.gethostname()

    # init us
    rc, myproc = client.init(None)
    if PMIX_SUCCESS != rc:
        eprint("Client: PMIx_Init failed:", client.error_string(rc))
        sys.exit(1)
    eprint("Client ns %s rank %d host %s pid %lu: Running"
           % (myproc['nspace'], myproc['rank'], hostname, os.getpid()))

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

    # register our default errhandler
    rc, refid = client.register_event_handler(None, None, notification_fn)
    if PMIX_SUCCESS != rc:
        return done(rc, refid)

    # put some "modex" data
    rc = client.put(PMIX_GLOBAL, "modex-btl",
                    {'value': "btl-tcp-%u" % myproc['rank'],
                     'val_type': PMIX_STRING})
    if PMIX_SUCCESS != rc:
        return done(rc, refid)

    # commit it
    rc = client.commit()
    if PMIX_SUCCESS != rc:
        return done(rc, refid)

    # call fence to sync
    rc = client.fence([proc], None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Fence failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(rc, refid)

    procs = [{'nspace': myproc['nspace'], 'rank': n} for n in range(nprocs)]

    # our contribution to the group: our local context ID, carried as a
    # nested info array under PMIX_GROUP_INFO
    lcid = 1234 + myproc['rank']
    grpinfo = [{'key': PMIX_GROUP_ASSIGN_CONTEXT_ID, 'value': True,
                'val_type': PMIX_BOOL},
               {'key': PMIX_GROUP_INFO,
                'value': {'type': PMIX_INFO,
                          'array': [{'key': PMIX_GROUP_LOCAL_CID,
                                     'value': lcid,
                                     'val_type': PMIX_SIZE}]},
                'val_type': PMIX_DATA_ARRAY}]

    rc, results = client.group_construct("ourgroup", procs, grpinfo)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Group_construct failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(rc, refid)
    # check the results for our global CID
    cid = 0
    idassigned = False
    if results:
        for item in results:
            if key_is(item['key'], PMIX_GROUP_CONTEXT_ID):
                cid = item['value']
                idassigned = True
                break
        eprint("Rank %d Group construct complete with status %s KEY %s "
               "CID assigned: %s value: %lu"
               % (myproc['rank'], client.error_string(rc),
                  as_key(results[0]['key']), "T" if idassigned else "F", cid))
    else:
        eprint("Rank %d Group construct complete, but no CID returned"
               % myproc['rank'])

    # destruct the group
    rc = client.group_destruct("ourgroup", None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Group_destruct failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(rc, refid)

    # the context ID is a qualifier on the get, not a value we want back
    tinfo = [{'key': PMIX_GROUP_CONTEXT_ID, 'value': cid,
              'val_type': PMIX_SIZE, 'flags': PMIX_INFO_QUALIFIER},
             {'key': PMIX_TIMEOUT, 'value': GET_TIMEOUT,
              'val_type': PMIX_UINT32}]

    for m in range(2):
        if 0 == myproc['rank']:
            eprint("ITERATION %zu" % m)
        proc = {'nspace': myproc['nspace'], 'rank': PMIX_RANK_WILDCARD}
        client.fence([proc], None)
        for n in range(nprocs):
            proc['rank'] = n
            rc, val = client.get(proc, PMIX_GROUP_LOCAL_CID, tinfo)
            if PMIX_SUCCESS != rc:
                eprint("Client ns %s rank %d: PMIx_Get of LOCAL CID for "
                       "rank %d failed: %s"
                       % (myproc['nspace'], myproc['rank'], n,
                          client.error_string(rc)))
                continue
            if PMIX_SIZE != val['val_type']:
                eprint("%s:%d: PMIx_Get LOCAL CID for rank %d returned "
                       "wrong type: %s"
                       % (myproc['nspace'], myproc['rank'], n,
                          client.data_type_string(val['val_type'])))
                os.abort()
            if (1234 + n) != val['value']:
                _, tmp = client.value_string(val)
                eprint("%s:%d: PMIx_Get LOCAL CID for rank %d returned "
                       "wrong value: %s"
                       % (myproc['nspace'], myproc['rank'], n, tmp))
                os.abort()
            _, tmp = client.value_string(val)
            eprint("%s:%d: PMIx_Get LOCAL CID for rank %u SUCCESS value: %s"
                   % (myproc['nspace'], myproc['rank'], n, tmp))
        proc = {'nspace': myproc['nspace'], 'rank': PMIX_RANK_WILDCARD}
        client.fence([proc], None)

    return done(rc, refid)


def done(rc, refid):
    # finalize us
    client.deregister_event_handler(refid)

    ret = client.finalize(None)
    if PMIX_SUCCESS != ret:
        eprint("Client ns %s rank %d:PMIx_Finalize failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(ret)))
        rc = ret
    eprint("%s:%d COMPLETE" % (myproc['nspace'], myproc['rank']))
    return rc


if __name__ == '__main__':
    sys.exit(main())
