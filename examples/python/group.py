#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/group.c
#
# Ranks 0, 2 and 3 construct a group named "ourgroup" and then tear it
# down. Rank 0 additionally supplies PMIX_GROUP_FINAL_MEMBERSHIP_ORDER, so
# the membership comes back in the order it asked for rather than by rank.
#
# Pass --test-query to also query the server for the active namespaces.
#
# Requires a minimum of 4 processes.

import os
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

    testquery = False
    for arg in sys.argv[1:]:
        if "--test-query" == arg:
            testquery = True
        else:
            eprint("Usage: %s [--test-query]"
                   % os.path.basename(sys.argv[0]))
            sys.exit(1)

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
        eprint("Client ns %s rank %d: PMIx_Get job size failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(0)
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
        return done(refid)

    tmp = "%s-%d-local" % (myproc['nspace'], myproc['rank'])
    rc = client.put(PMIX_LOCAL, tmp, {'value': 1234, 'val_type': PMIX_UINT64})
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Put local failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(refid)

    tmp = "%s-%d-remote" % (myproc['nspace'], myproc['rank'])
    rc = client.put(PMIX_REMOTE, tmp,
                    {'value': "1234", 'val_type': PMIX_STRING})
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Put remote failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(refid)

    # push the data to our PMIx server
    rc = client.commit()
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Commit failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(refid)

    # call fence to sync
    rc = client.fence([proc], None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Fence failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(refid)

    # rank=0,2,3 construct a new group
    if myproc['rank'] in (0, 2, 3):
        eprint("%d executing Group_construct" % myproc['rank'])
        procs = [{'nspace': myproc['nspace'], 'rank': r} for r in (0, 2, 3)]
        info = [{'key': PMIX_GROUP_ASSIGN_CONTEXT_ID, 'value': True,
                 'val_type': PMIX_BOOL}]

        if 0 == myproc['rank']:
            # ask for the membership to come back in this order
            parray = [{'nspace': myproc['nspace'], 'rank': r}
                      for r in (2, 0, 3)]
            info.append({'key': PMIX_GROUP_FINAL_MEMBERSHIP_ORDER,
                         'value': {'type': PMIX_PROC, 'array': parray},
                         'val_type': PMIX_DATA_ARRAY})

        rc, results = client.group_construct("ourgroup", procs, info)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Group_construct failed: %s"
                   % (myproc['nspace'], myproc['rank'],
                      client.error_string(rc)))
            return done(refid)
        # we should have a single results object
        if results:
            for item in results:
                if PMIX_GROUP_CONTEXT_ID == item['key']:
                    eprint("%d Group construct complete with status %s "
                           "KEY %s CID %lu"
                           % (myproc['rank'],
                              client.error_string(PMIX_SUCCESS),
                              item['key'], item['value']))
                elif PMIX_GROUP_MEMBERSHIP == item['key']:
                    parray = item['value']['array']
                    if 0 == myproc['rank']:
                        eprint("NUM MEMBERS: %u MEMBERSHIP:" % len(parray))
                        for m in parray:
                            eprint("\t%s:%u" % (m['nspace'], m['rank']))
        else:
            eprint("%d Group construct complete, but results returned"
                   % myproc['rank'])

        eprint("%d executing Group_destruct" % myproc['rank'])
        rc = client.group_destruct("ourgroup", None)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Group_destruct failed: %s"
                   % (myproc['nspace'], myproc['rank'],
                      client.error_string(rc)))
            return done(refid)

    if testquery and 0 == myproc['rank']:
        # first ask for a list of active namespaces
        rc, results = client.query([{'keys': [PMIX_QUERY_NAMESPACES],
                                     'qualifiers': None}])
        if PMIX_SUCCESS != rc:
            eprint("Error: PMIx_Query_info for namespaces failed: %d (%s)"
                   % (rc, client.error_string(rc)))
            return done(refid)
        eprint("\n\n--> Query returned (ninfo %d)" % len(results))
        for item in results:
            _, tmp = client.info_string(item)
            eprint(tmp)
        eprint("<--- END\n\n")

        # we can then parse the results to find a namespace of interest,
        # and query about that namespace in particular. Or we can simply
        # query for info on ALL namespaces
        rc, results = client.query([{'keys': [PMIX_QUERY_NAMESPACE_INFO],
                                     'qualifiers': None}])
        if PMIX_SUCCESS != rc:
            eprint("Error: PMIx_Query_info for namespace info failed: %d (%s)"
                   % (rc, client.error_string(rc)))
            return done(refid)
        eprint("--> Query returned (ninfo %d)" % len(results))
        for item in results:
            _, tmp = client.info_string(item)
            eprint(tmp)
        eprint("<--- END\n\n")

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
    return rc


if __name__ == '__main__':
    sys.exit(main())
