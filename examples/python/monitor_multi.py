#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/monitor_multi.c
#
# Monitor more than one process at a time: rank 0 asks for resource usage
# on ranks 0 and 1 by naming them in PMIX_MONITOR_TARGET_PROCS, waits for
# two updates, then cancels.
#
# Requires at least two processes.

import socket
import sys
import threading

from examples import *

client = PMIxClient()
myproc = {}
# the C version spins on a volatile counter waiting for updates
updates = threading.Semaphore(0)


# this is the event notification function we pass down below when
# registering for general events - i.e., the default handler. We don't
# technically need to register one, but it is usually good practice to
# catch any events that occur
def notification_fn(evhdlr, status, source, info, results):
    return PMIX_EVENT_ACTION_COMPLETE, None


def update(evhdlr, status, source, info, results):
    _, tmp = client.proc_string(myproc)
    eprint("[%s]UPDATE:" % tmp)
    for item in info:
        _, txt = client.info_string(item)
        eprint(txt, end='')
    eprint("\n")

    updates.release()
    return PMIX_EVENT_ACTION_COMPLETE, None


def main():
    global myproc

    hostname = socket.gethostname()

    # init us - note that the call to "init" includes the return of
    # any job-related info provided by the RM.
    rc, myproc = client.init(None)
    if PMIX_SUCCESS != rc:
        eprint("Client: PMIx_Init failed: %d" % rc)
        sys.exit(1)
    eprint("Client ns %s rank %d: Running"
           % (myproc['nspace'], myproc['rank']))

    # register our default event handler - again, this isn't strictly
    # required, but is generally good practice
    rc, _ = client.register_event_handler(None, None, notification_fn)
    if PMIX_SUCCESS != rc:
        eprint("[%s:%d] Default handler registration failed"
               % (myproc['nspace'], myproc['rank']))
        sys.exit(2)

    # register the monitor update event handler
    rc, _ = client.register_event_handler([PMIX_MONITOR_RESUSAGE_UPDATE],
                                          None, update)
    if PMIX_SUCCESS != rc:
        eprint("[%s:%d] Update handler registration failed"
               % (myproc['nspace'], myproc['rank']))
        sys.exit(3)

    # job-related info is found in our nspace, assigned to the
    # wildcard rank as it doesn't relate to a specific rank. Setup
    # a name to retrieve such values
    proc = {'nspace': myproc['nspace'], 'rank': PMIX_RANK_WILDCARD}

    # get our job size
    rc, val = client.get(proc, PMIX_JOB_SIZE, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get job size failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        sys.exit(4)
    nprocs = val['value']
    if 2 > nprocs:
        if 0 == myproc['rank']:
            eprint("This example requires at least two processes - got %d"
                   % nprocs)
        sys.exit(5)
    eprint("Client %s:%d node %s"
           % (myproc['nspace'], myproc['rank'], hostname))

    # call fence to synchronize with our peers - no need to
    # collect any info as we didn't "put" anything
    info = [{'key': PMIX_COLLECT_DATA, 'value': False, 'val_type': PMIX_BOOL}]
    rc = client.fence([proc], info)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Fence failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        sys.exit(6)

    if 0 == myproc['rank']:
        # ask to monitor process resource usage, naming the two procs we
        # want watched
        monitor = [{'key': PMIX_MONITOR_PROC_RESOURCE_USAGE,
                    'value': {'type': PMIX_PROC, 'array': []},
                    'val_type': PMIX_DATA_ARRAY}]
        targets = [{'nspace': myproc['nspace'], 'rank': 0},
                   {'nspace': myproc['nspace'], 'rank': 1}]
        directives = [{'key': PMIX_MONITOR_ID, 'value': "mymonitor",
                       'val_type': PMIX_STRING},
                      {'key': PMIX_MONITOR_RESOURCE_RATE, 'value': 3,
                       'val_type': PMIX_UINT32},
                      {'key': PMIX_MONITOR_TARGET_PROCS,
                       'value': {'type': PMIX_PROC, 'array': targets},
                       'val_type': PMIX_DATA_ARRAY}]
        rc, results = client.monitor(monitor, PMIX_MONITOR_RESUSAGE_UPDATE,
                                     directives)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Process_monitor failed: %s"
                   % (myproc['nspace'], myproc['rank'],
                      client.error_string(rc)))
            return done()

        if 0 == len(results):
            eprint("No procs found")
        else:
            eprint("INITIAL PROC RESULTS:")
            for item in results:
                _, txt = client.info_string(item)
                eprint(txt, end='')
            eprint("\n")
            for _ in range(2):
                updates.acquire()

        # cancel the monitor
        monitor = [{'key': PMIX_MONITOR_CANCEL, 'value': "mymonitor",
                    'val_type': PMIX_STRING}]
        rc, results = client.monitor(monitor, PMIX_MONITOR_RESUSAGE_UPDATE,
                                     None)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: cancel monitor failed: %s"
                   % (myproc['nspace'], myproc['rank'],
                      client.error_string(rc)))
            sys.exit(7)

    # call fence to synchronize with our peers - no need to
    # collect any info as we didn't "put" anything
    info = [{'key': PMIX_COLLECT_DATA, 'value': False, 'val_type': PMIX_BOOL}]
    rc = client.fence([proc], info)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Fence failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        sys.exit(8)

    return done()


def done():
    # finalize us
    eprint("Client ns %s rank %d: Finalizing"
           % (myproc['nspace'], myproc['rank']))
    rc = client.finalize(None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d:PMIx_Finalize failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        sys.exit(9)
    eprint("Client ns %s rank %d:PMIx_Finalize successfully completed"
           % (myproc['nspace'], myproc['rank']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
