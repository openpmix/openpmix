#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/client3.c
#
# Ask for the same piece of information by more than one route - with the
# wildcard rank, with our own proc ID, and with no proc at all - and check
# that every route agrees.

import os
import sys

from examples import *


# this is the event notification function we pass down below when
# registering for general events - i.e., the default handler. We don't
# technically need to register one, but it is usually good practice to
# catch any events that occur
def notification_fn(evhdlr, status, source, info, results):
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
        eprint("Client: PMIx_Init failed:", client.error_string(rc))
        sys.exit(0)
    eprint("Client ns %s rank %d pid %d: Running"
           % (myproc['nspace'], myproc['rank'], pid))

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

    # get the number of procs in our job - univ size is the total number of
    # allocated slots, not the number of procs in the job
    rc, val = client.get(proc, PMIX_JOB_SIZE, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get job size failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(client, myproc)
    eprint("Client %s:%d num procs %d"
           % (myproc['nspace'], myproc['rank'], val['value']))

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
    # PMIx_Value_compare is deliberately unbound - these are ordinary
    # Python objects, so compare them directly
    if val == val2:
        eprint("Client ns %s rank %d: PMIx_Get local procs GOOD"
               % (myproc['nspace'], myproc['rank']))
    else:
        eprint("Client ns %s rank %d: PMIx_Get local procs mismatch"
               % (myproc['nspace'], myproc['rank']))

    # get our nodeID in various ways
    rc, val = client.get(None, PMIX_NODEID, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get nodeID with NULL proc "
               "failed: %s" % (myproc['nspace'], myproc['rank'],
                               client.error_string(rc)))
    # get the nodeID using our proc ID
    rc, val2 = client.get(myproc, PMIX_NODEID, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get nodeID with my ID failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
    if val is not None and val2 is not None and val == val2:
        eprint("Client ns %s rank %d: PMIx_Get nodeID GOOD"
               % (myproc['nspace'], myproc['rank']))
    else:
        eprint("Client ns %s rank %d: PMIx_Get nodeID mismatch"
               % (myproc['nspace'], myproc['rank']))

    # get our hostname in various ways
    rc, val = client.get(None, PMIX_HOSTNAME, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get hostname with NULL proc "
               "failed: %s" % (myproc['nspace'], myproc['rank'],
                               client.error_string(rc)))
        return done(client, myproc)
    # get the hostname using our proc ID
    rc, val2 = client.get(myproc, PMIX_HOSTNAME, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get hostname with my ID failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(client, myproc)
    if val == val2:
        eprint("Client ns %s rank %d: PMIx_Get hostname GOOD"
               % (myproc['nspace'], myproc['rank']))
    else:
        eprint("Client ns %s rank %d: PMIx_Get hostname mismatch"
               % (myproc['nspace'], myproc['rank']))

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
