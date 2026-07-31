#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/client.c
#
# The canonical client: register handlers, honor a stop-in-app debugger
# directive, publish a few values with different scopes, fence, and then
# read back what every peer contributed - checking that a locally-scoped
# value comes back to local peers and a remotely-scoped one to the rest.

import os
import sys
import time

from examples import *

# the C version passes a pointer to its lock as PMIX_EVENT_RETURN_OBJECT
# and digs it back out of the handler's info array. There is no such
# pointer in Python, so the release handler closes over this instead
myrel = MyRel()


# this is the event notification function we pass down below when
# registering for general events - i.e., the default handler. We don't
# technically need to register one, but it is usually good practice to
# catch any events that occur
def notification_fn(evhdlr, status, source, info, results):
    return PMIX_EVENT_ACTION_COMPLETE, None


# this is an event notification function that we explicitly request be
# called when the PMIX_ERR_DEBUGGER_RELEASE notification is issued. We
# could catch it in the general event notification function and test the
# status to see if it was "debugger release", but it often is simpler to
# declare a use-specific notification callback point. In this case, we are
# asking to know when we are told the debugger released us
def release_fn(evhdlr, status, source, info, results):
    # the status will be PMIX_ERR_DEBUGGER_RELEASE since that is the code
    # we registered to receive, so just return success
    myrel.status = PMIX_SUCCESS
    # release the lock
    myrel.wakeup()
    # tell the event handler state machine that we are the last step
    return PMIX_EVENT_ACTION_COMPLETE, None


def main():
    pid = os.getpid()
    eprint("Client %d: Running" % pid)

    client = PMIxClient()

    # init us - note that the call to "init" includes the return of
    # any job-related info provided by the RM. This includes any
    # debugger flag instructing us to stop-in-init. If such a directive
    # is included, then the process will be stopped in this call until
    # the "debugger release" notification arrives
    rc, myproc = client.init(None)
    if PMIX_SUCCESS != rc:
        if PMIX_ERR_UNREACH == rc:
            eprint("Client: Cannot operate as singleton")
        else:
            eprint("Client: PMIx_Init failed:", client.error_string(rc))
        sys.exit(1)
    eprint("Client ns %s rank %d pid %d: Running"
           % (myproc['nspace'], myproc['rank'], pid))

    # register our default event handler - again, this isn't strictly
    # required, but is generally good practice. Registration blocks in
    # Python, so there is no registration callback to wait on
    rc, _ = client.register_event_handler(None, None, notification_fn)
    if PMIX_SUCCESS != rc:
        eprint("[%s:%d] Default handler registration failed"
               % (myproc['nspace'], myproc['rank']))
        return done(client, myproc)

    # job-related info is found in our nspace, assigned to the
    # wildcard rank as it doesn't relate to a specific rank. Setup
    # a name to retrieve such values
    proc = {'nspace': myproc['nspace'], 'rank': PMIX_RANK_WILDCARD}

    # check to see if we have been instructed to wait for a debugger
    # to attach to us. We won't get both a stop-in-init AND a
    # wait-for-notify directive, so we should never stop twice. This
    # directive is provided so that something like an MPI implementation
    # can do some initial setup in MPI_Init prior to pausing for the
    # debugger
    rc, val = client.get(proc, PMIX_DEBUG_STOP_IN_APP, None)
    if PMIX_SUCCESS == rc:
        # register for debugger release
        rc, _ = client.register_event_handler([PMIX_ERR_DEBUGGER_RELEASE],
                                              None, release_fn)
        if PMIX_SUCCESS != rc:
            eprint("[%s:%d] Debug handler registration failed"
                   % (myproc['nspace'], myproc['rank']))
            return done(client, myproc)
        # wait for debugger release
        myrel.wait()

    # check for local topology info
    rc = client.load_topology()
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Load_topology failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(client, myproc)
    eprint("Client %s:%d topology loaded" % (myproc['nspace'], myproc['rank']))

    # get our universe size
    rc, val = client.get(proc, PMIX_UNIV_SIZE, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get universe size failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(client, myproc)
    eprint("Client %s:%d universe size %d"
           % (myproc['nspace'], myproc['rank'], val['value']))

    # get the number of procs in our job - univ size is the total number of
    # allocated slots, not the number of procs in the job
    rc, val = client.get(proc, PMIX_JOB_SIZE, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get job size failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(client, myproc)
    nprocs = val['value']
    eprint("Client %s:%d num procs %d"
           % (myproc['nspace'], myproc['rank'], nprocs))

    # put a few values
    tmp = "%s-%d-internal" % (myproc['nspace'], myproc['rank'])
    rc = client.store_internal(myproc, tmp,
                               {'value': 1234, 'val_type': PMIX_UINT32})
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Store_internal failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(client, myproc)

    tmp = "%s-%d-local" % (myproc['nspace'], myproc['rank'])
    rc = client.put(PMIX_LOCAL, tmp, {'value': 1234, 'val_type': PMIX_UINT64})
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Put local failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(client, myproc)

    tmp = "%s-%d-remote" % (myproc['nspace'], myproc['rank'])
    rc = client.put(PMIX_REMOTE, tmp,
                    {'value': "1234", 'val_type': PMIX_STRING})
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Put remote failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(client, myproc)

    # push the data to our PMIx server
    rc = client.commit()
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Commit failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(client, myproc)
    if 0 == myproc['rank']:
        eprint("Rank 0 sleeping")
        time.sleep(5)

    # call fence to synchronize with our peers - instruct
    # the fence operation to collect and return all "put"
    # data from our peers
    info = [{'key': PMIX_COLLECT_DATA, 'value': True, 'val_type': PMIX_BOOL}]
    rc = client.fence([proc], info)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Fence failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(client, myproc)

    # get a list of our local procs - some may not be in our job
    rc, val = client.get(proc, PMIX_LOCAL_PROCS, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get local procs with WILDCARD rank "
               "failed: %s" % (myproc['nspace'], myproc['rank'],
                               client.error_string(rc)))
        return done(client, myproc)
    # get the list using our proc ID
    rc, val2 = client.get(myproc, PMIX_LOCAL_PROCS, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get local procs with my ID "
               "failed: %s" % (myproc['nspace'], myproc['rank'],
                               client.error_string(rc)))
        return done(client, myproc)
    # PMIx_Value_compare is one of the struct helpers the bindings
    # deliberately leave unbound - the values are ordinary Python objects
    # here, so compare them directly
    if val == val2:
        eprint("Client ns %s rank %d: PMIx_Get local procs GOOD"
               % (myproc['nspace'], myproc['rank']))
    else:
        eprint("Client ns %s rank %d: PMIx_Get local procs mismatch"
               % (myproc['nspace'], myproc['rank']))

    # get a list of our local peers
    rc, val = client.get(proc, PMIX_LOCAL_PEERS, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get local peers failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(client, myproc)
    # split the returned string to get the rank of each local peer
    peers = val['value'].split(',')
    nlocal = len(peers)
    if nprocs == nlocal:
        all_local = True
        locals_ = []
    else:
        all_local = False
        locals_ = [int(p) for p in peers]

    # check the returned data
    for n in range(nprocs):
        local = all_local or n in locals_
        proc['rank'] = n
        if local:
            tmp = "%s-%d-local" % (myproc['nspace'], n)
            rc, val = client.get(proc, tmp, None)
            if PMIX_SUCCESS != rc:
                eprint("Client ns %s rank %d: PMIx_Get %s failed: %s"
                       % (myproc['nspace'], myproc['rank'], tmp,
                          client.error_string(rc)))
                return done(client, myproc)
            if PMIX_UINT64 != val['val_type']:
                eprint("Client ns %s rank %d: PMIx_Get %s returned wrong "
                       "type: %d" % (myproc['nspace'], myproc['rank'], tmp,
                                     val['val_type']))
                return done(client, myproc)
            if 1234 != val['value']:
                eprint("Client ns %s rank %d: PMIx_Get %s returned wrong "
                       "value: %d" % (myproc['nspace'], myproc['rank'], tmp,
                                      val['value']))
                return done(client, myproc)
        else:
            tmp = "%s-%d-remote" % (myproc['nspace'], n)
            rc, val = client.get(proc, tmp, None)
            if PMIX_SUCCESS != rc:
                eprint("Client ns %s rank %d: PMIx_Get %s failed: %s"
                       % (myproc['nspace'], myproc['rank'], tmp,
                          client.error_string(rc)))
                return done(client, myproc)
            if PMIX_STRING != val['val_type']:
                eprint("Client ns %s rank %d: PMIx_Get %s returned wrong "
                       "type: %d" % (myproc['nspace'], myproc['rank'], tmp,
                                     val['val_type']))
                return done(client, myproc)
            if "1234" != val['value']:
                eprint("Client ns %s rank %d: PMIx_Get %s returned wrong "
                       "value: %s" % (myproc['nspace'], myproc['rank'], tmp,
                                      val['value']))
                return done(client, myproc)
        eprint("Client ns %s rank %d: PMIx_Get %s returned correct"
               % (myproc['nspace'], myproc['rank'], tmp))

    return done(client, myproc)


def done(client, myproc):
    # finalize us
    eprint("Client ns %s rank %d: Finalizing"
           % (myproc['nspace'], myproc['rank']))
    rc = client.finalize(None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d:PMIx_Finalize failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
    else:
        eprint("Client ns %s rank %d:PMIx_Finalize successfully completed"
               % (myproc['nspace'], myproc['rank']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
