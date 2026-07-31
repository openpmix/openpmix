#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/alloc.c
#
# Three ways of learning that an allocation request finished, one per
# rank:
#
#   rank 0     makes the request and waits on its own callback, then
#              notifies everyone else that it completed
#   rank 1     registers an event handler for PMIX_NOTIFY_ALLOC_COMPLETE
#              and waits to be told
#   rank 2+    polls with a query for PMIX_QUERY_ALLOC_STATUS

import sys
import time

from examples import *

MYALLOCATION = "MYALLOCATION"

# the C version passes a pointer to its myrel_t as PMIX_EVENT_RETURN_OBJECT
# and recovers it from the handler's info array. Python has no such
# pointer, so the handler uses this module-global instead
myrel = MyRel()
client = PMIxClient()


# this is a callback function for the query and allocation-request APIs.
# The request will call back with a status indicating whether it could be
# fully satisfied, partially satisfied, or completely failed; the results
# carry the key that was provided in the request, so the returned value
# can be correlated to the requested key.
#
# Unlike the C version there is no release_fn to call and nothing to
# deep-copy: the binding converted the results to Python and released the
# library's copy before this runs.
def infocbfunc(status, results, mq):
    eprint("Allocation request returned %s" % client.error_string(status))
    for item in results:
        eprint("Transferring %s" % item['key'])
    mq.info = results
    # the status returned here indicates whether the requested
    # information was found or not - preserve it, then release the block
    mq.wakeup(status)


# this is an event notification function that we explicitly request be
# called when the allocation completes. We could catch it in the general
# event notification function and test the status, but it often is simpler
# to declare a use-specific notification callback point
def release_fn(evhdlr, status, source, info, results):
    pstatus = find_key(info, PMIX_ALLOC_STATUS)
    if pstatus is None:
        pstatus = PMIX_ERROR
    myrel.status = pstatus
    # release the lock
    myrel.wakeup()
    # tell the event handler state machine that we are the last step
    return PMIX_EVENT_ACTION_COMPLETE, None


def main():
    # init us
    rc, myproc = client.init(None)
    if PMIX_SUCCESS != rc:
        eprint("Client: PMIx_Init failed: %d" % rc)
        sys.exit(0)
    eprint("Client ns %s rank %d: Running"
           % (myproc['nspace'], myproc['rank']))

    # get our universe size
    proc = {'nspace': myproc['nspace'], 'rank': PMIX_RANK_WILDCARD}
    rc, val = client.get(proc, PMIX_UNIV_SIZE, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get universe size failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(client, myproc)
    eprint("Client %s:%d universe size %d"
           % (myproc['nspace'], myproc['rank'], val['value']))

    if 0 == myproc['rank']:
        # try to get an allocation
        mydata = MyQuery()
        info = [{'key': PMIX_ALLOC_NUM_NODES, 'value': 12,
                 'val_type': PMIX_UINT64},
                {'key': PMIX_ALLOC_ID, 'value': MYALLOCATION,
                 'val_type': PMIX_STRING}]
        rc = client.allocation_request_nb(PMIX_ALLOC_NEW, info,
                                          infocbfunc, mydata)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Allocation_request_nb "
                   "failed: %d" % (myproc['nspace'], myproc['rank'], rc))
            return done(client, myproc)
        mydata.wait()
        rc = mydata.status
        eprint("Client ns %s rank %d: Allocation returned status: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))

        # need to notify rank=1 so it gets released
        info = [{'key': PMIX_ALLOC_STATUS, 'value': rc,
                 'val_type': PMIX_STATUS}]
        client.notify_event(PMIX_NOTIFY_ALLOC_COMPLETE, myproc,
                            PMIX_RANGE_GLOBAL, info)

    elif 1 == myproc['rank']:
        # demonstrate a notification based approach - register a handler
        # specifically for when the allocation operation completes
        info = [{'key': PMIX_ALLOC_ID, 'value': MYALLOCATION,
                 'val_type': PMIX_STRING}]
        rc, _ = client.register_event_handler([PMIX_NOTIFY_ALLOC_COMPLETE],
                                              info, release_fn)
        if PMIX_SUCCESS != rc:
            eprint("EVENT HANDLER REGISTRATION FAILED WITH STATUS %d" % rc)

        # now wait to hear that the request is complete
        myrel.wait()
        eprint("[%s:%d] Allocation returned status: %s"
               % (myproc['nspace'], myproc['rank'],
                  client.error_string(myrel.status)))

    else:
        # demonstrate a query-based approach - wait a little while and ask
        # to see if it was done
        time.sleep(0.00001)
        mydata = MyQuery()
        query = [{'keys': [PMIX_QUERY_ALLOC_STATUS],
                  'qualifiers': [{'key': PMIX_ALLOC_ID,
                                  'value': MYALLOCATION,
                                  'val_type': PMIX_STRING}]}]
        _, tmp = client.proc_string(myproc)
        eprint("Client %s querying allocation" % tmp)
        rc = client.query_nb(query, infocbfunc, mydata)
        if PMIX_SUCCESS != rc:
            eprint("PMIx_Query_info failed: %d" % rc)
            return done(client, myproc)
        mydata.wait()
        eprint("[%s:%d] Allocation query returned status: %s"
               % (myproc['nspace'], myproc['rank'],
                  client.error_string(mydata.status)))

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
