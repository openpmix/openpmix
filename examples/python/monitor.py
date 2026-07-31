#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/monitor.c
#
# Rank 0 walks the four resource-usage monitors in turn - process, node,
# disk and network. For each it starts the monitor, prints the initial
# reading, waits for two update events to arrive, and then cancels it.
#
# The empty data array in each request means "everything you have": no
# specific procs, nodes, disks or networks are named.

import sys
import threading

from examples import *

client = PMIxClient()
myproc = {}
# the C version spins on a volatile counter waiting for updates; an Event
# is both cheaper and easier to read
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
        _, tmp = client.info_string(item)
        eprint(tmp, end='')
    eprint("\n")

    updates.release()
    return PMIX_EVENT_ACTION_COMPLETE, None


# the four monitors, in the order the C version runs them:
#   (attribute, element type of the (empty) target array, monitor id, label)
MONITORS = [
    (PMIX_MONITOR_PROC_RESOURCE_USAGE, PMIX_PROC, "mymonitor", "PROC",
     "procs"),
    (PMIX_MONITOR_NODE_RESOURCE_USAGE, PMIX_STRING, "nodemon", "NODE",
     "nodes"),
    (PMIX_MONITOR_DISK_RESOURCE_USAGE, PMIX_STRING, "dkmon", "DISK",
     "disks"),
    (PMIX_MONITOR_NET_RESOURCE_USAGE, PMIX_STRING, "netmon", "NETWORK",
     "networks"),
]


def run_monitor(attr, elt_type, monid, label, plural, rate):
    """Start one monitor, wait for two updates, then cancel it."""
    monitor = [{'key': attr, 'value': {'type': elt_type, 'array': []},
                'val_type': PMIX_DATA_ARRAY}]
    # every request carries the sampling rate. The C version only spells
    # it out for the first monitor, but it reuses the same directives[1]
    # slot for the rest, so all four are sampled at the same rate - and
    # without a rate the monitor never fires an update
    directives = [{'key': PMIX_MONITOR_ID, 'value': monid,
                   'val_type': PMIX_STRING},
                  {'key': PMIX_MONITOR_RESOURCE_RATE, 'value': rate,
                   'val_type': PMIX_UINT32}]
    rc, results = client.monitor(monitor, PMIX_MONITOR_RESUSAGE_UPDATE,
                                 directives)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Process_monitor failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return False

    if 0 == len(results):
        eprint("No %s found" % plural)
    else:
        eprint("INITIAL %s RESULTS:" % label)
        for item in results:
            _, tmp = client.info_string(item)
            eprint(tmp, end='')
        eprint("\n")
        # wait for two updates to arrive
        for _ in range(2):
            updates.acquire()

    # cancel the monitor
    monitor = [{'key': PMIX_MONITOR_CANCEL, 'value': monid,
                'val_type': PMIX_STRING}]
    rc, results = client.monitor(monitor, PMIX_MONITOR_RESUSAGE_UPDATE, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: cancel monitor failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return False
    return True


def main():
    global myproc

    # init us - note that the call to "init" includes the return of
    # any job-related info provided by the RM.
    rc, myproc = client.init(None)
    if PMIX_SUCCESS != rc:
        eprint("Client: PMIx_Init failed: %d" % rc)
        sys.exit(0)
    eprint("Client ns %s rank %d: Running"
           % (myproc['nspace'], myproc['rank']))

    # register our default event handler - again, this isn't strictly
    # required, but is generally good practice
    rc, _ = client.register_event_handler(None, None, notification_fn)
    if PMIX_SUCCESS != rc:
        eprint("[%s:%d] Default handler registration failed"
               % (myproc['nspace'], myproc['rank']))
        return done()

    # register the monitor update event handler
    rc, _ = client.register_event_handler([PMIX_MONITOR_RESUSAGE_UPDATE],
                                          None, update)
    if PMIX_SUCCESS != rc:
        eprint("[%s:%d] Update handler registration failed"
               % (myproc['nspace'], myproc['rank']))
        return done()

    # job-related info is found in our nspace, assigned to the
    # wildcard rank as it doesn't relate to a specific rank. Setup
    # a name to retrieve such values
    proc = {'nspace': myproc['nspace'], 'rank': PMIX_RANK_WILDCARD}

    # get our job size
    rc, val = client.get(proc, PMIX_JOB_SIZE, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get job size failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done()
    eprint("Client %s:%d job size %d"
           % (myproc['nspace'], myproc['rank'], val['value']))

    if 0 == myproc['rank']:
        for attr, elt, monid, label, plural in MONITORS:
            if not run_monitor(attr, elt, monid, label, plural, 3):
                return done()

    # call fence to synchronize with our peers - no need to
    # collect any info as we didn't "put" anything
    info = [{'key': PMIX_COLLECT_DATA, 'value': False, 'val_type': PMIX_BOOL}]
    rc = client.fence([proc], info)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Fence failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))

    return done()


def done():
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
