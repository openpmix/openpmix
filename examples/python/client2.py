#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/client2.c
#
# Exercise the data-array value type: every rank puts an array of 32
# uint64 values, fences, and then verifies the array it gets back from
# each of its peers.
#
# In Python a data array is the value dict
#     {'value': {'type': <type>, 'array': [...]}, 'val_type': PMIX_DATA_ARRAY}
# so there is nothing to malloc or free.

import sys

from examples import *


# this is the event notification function we pass down below when
# registering for general events - i.e., the default handler. We don't
# technically need to register one, but it is usually good practice to
# catch any events that occur
def notification_fn(evhdlr, status, source, info, results):
    return PMIX_EVENT_ACTION_COMPLETE, None


def main():
    client = PMIxClient()

    # init us - note that the call to "init" includes the return of
    # any job-related info provided by the RM. This includes any
    # debugger flag instructing us to stop-in-init. If such a directive
    # is included, then the process will be stopped in this call until
    # the "debugger release" notification arrives
    rc, myproc = client.init(None)
    if PMIX_SUCCESS != rc:
        eprint("Client: PMIx_Init failed:", rc)
        sys.exit(0)
    eprint("Client ns %s rank %d: Running"
           % (myproc['nspace'], myproc['rank']))

    # register our default event handler - again, this isn't strictly
    # required, but is generally good practice
    rc, _ = client.register_event_handler(None, None, notification_fn)
    if PMIX_SUCCESS != rc:
        eprint("[%s:%d] Default handler registration failed"
               % (myproc['nspace'], myproc['rank']))
        return done(client, myproc)

    # job-related info is found in our nspace, assigned to the
    # wildcard rank as it doesn't relate to a specific rank. Setup
    # a name to retrieve such values
    proc = {'nspace': myproc['nspace'], 'rank': PMIX_RANK_WILDCARD}

    # get our job size
    rc, val = client.get(proc, PMIX_JOB_SIZE, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get job size failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(client, myproc)
    nprocs = val['value']
    eprint("Client %s:%d job size %d"
           % (myproc['nspace'], myproc['rank'], nprocs))

    # put a data array of uint64 values
    u64 = [2 * n for n in range(32)]
    rc = client.put(PMIX_GLOBAL, "test-key",
                    {'value': {'type': PMIX_UINT64, 'array': u64},
                     'val_type': PMIX_DATA_ARRAY})
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Put failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(client, myproc)

    # push the data to our PMIx server
    rc = client.commit()
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Commit failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(client, myproc)

    # call fence to synchronize with our peers - instruct
    # the fence operation to collect and return all "put"
    # data from our peers
    info = [{'key': PMIX_COLLECT_DATA, 'value': True, 'val_type': PMIX_BOOL}]
    rc = client.fence([proc], info)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Fence failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(client, myproc)

    # check the returned data
    for n in range(nprocs):
        proc['rank'] = n
        rc, val = client.get(proc, "test-key", None)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Get on rank %d failed: %d"
                   % (myproc['nspace'], myproc['rank'], n,
                      rc))
            return done(client, myproc)
        if PMIX_DATA_ARRAY != val['val_type']:
            eprint("Client ns %s rank %d: PMIx_Get on rank %d returned wrong "
                   "type: %d" % (myproc['nspace'], myproc['rank'], n,
                                 val['val_type']))
            return done(client, myproc)
        darray = val['value']
        if darray is None:
            eprint("Client ns %s rank %d: PMIx_Get %d returned NULL array"
                   % (myproc['nspace'], myproc['rank'], n))
            return done(client, myproc)
        if PMIX_UINT64 != darray['type']:
            eprint("Client ns %s rank %d: PMIx_Get %d returned wrong array "
                   "value type %d" % (myproc['nspace'], myproc['rank'], n,
                                      darray['type']))
            return done(client, myproc)
        if 32 != len(darray['array']):
            eprint("Client ns %s rank %d: PMIx_Get %d returned wrong array "
                   "value size %d" % (myproc['nspace'], myproc['rank'], n,
                                      len(darray['array'])))
            return done(client, myproc)
        for k in range(32):
            if (2 * k) != darray['array'][k]:
                eprint("Client ns %s rank %d: PMIx_Get %d returned wrong "
                       "value: %d" % (myproc['nspace'], myproc['rank'], n,
                                      darray['array'][k]))
                return done(client, myproc)

    return done(client, myproc)


def done(client, myproc):
    # finalize us
    eprint("Client ns %s rank %d: Finalizing"
           % (myproc['nspace'], myproc['rank']))
    rc = client.finalize(None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d:PMIx_Finalize failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
    else:
        eprint("Client ns %s rank %d:PMIx_Finalize successfully completed"
               % (myproc['nspace'], myproc['rank']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
