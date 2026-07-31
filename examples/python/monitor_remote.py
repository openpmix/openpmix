#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/monitor_remote.c
#
# Monitor a process other than ourselves: rank 0 names rank 1 in
# PMIX_MONITOR_TARGET_PROCS, so the readings come from a process that may
# be on another node entirely. It then walks the node, disk and network
# monitors as well.

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


def drain():
    """Discard any updates left over from the previous monitor.

    The C version simply resets its counter to 0 between monitors.
    """
    while updates.acquire(blocking=False):
        pass


def run_monitor(monitor, directives, label, plural, wait_msg=False):
    """Start one monitor, wait for two updates, then cancel it."""
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
            _, txt = client.info_string(item)
            eprint(txt, end='')
        eprint("\n")
        if wait_msg:
            eprint("WAITING FOR UPDATES\n")
        for _ in range(2):
            updates.acquire()
    return True


def cancel_monitor(monid):
    monitor = [{'key': PMIX_MONITOR_CANCEL, 'value': monid,
                'val_type': PMIX_STRING}]
    rc, _ = client.monitor(monitor, PMIX_MONITOR_RESUSAGE_UPDATE, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: cancel monitor failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return False
    return True


def main():
    global myproc

    hostname = socket.gethostname()

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

    eprint("Client %s:%d node %s"
           % (myproc['nspace'], myproc['rank'], hostname))

    # call fence to synchronize with our peers - no need to
    # collect any info as we didn't "put" anything
    info = [{'key': PMIX_COLLECT_DATA, 'value': False, 'val_type': PMIX_BOOL}]
    rc = client.fence([proc], info)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Fence failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return rc

    if 0 == myproc['rank']:
        # ask to monitor process resource usage - on rank 1, not on us
        monitor = [{'key': PMIX_MONITOR_PROC_RESOURCE_USAGE,
                    'value': {'type': PMIX_PROC, 'array': []},
                    'val_type': PMIX_DATA_ARRAY}]
        targets = [{'nspace': myproc['nspace'], 'rank': 1}]
        directives = [{'key': PMIX_MONITOR_ID, 'value': "mymonitor",
                       'val_type': PMIX_STRING},
                      {'key': PMIX_MONITOR_RESOURCE_RATE, 'value': 3,
                       'val_type': PMIX_UINT32},
                      {'key': PMIX_MONITOR_TARGET_PROCS,
                       'value': {'type': PMIX_PROC, 'array': targets},
                       'val_type': PMIX_DATA_ARRAY}]
        if not run_monitor(monitor, directives, "PROC", "procs",
                           wait_msg=True):
            return done()
        if not cancel_monitor("mymonitor"):
            return done()

        # the node, disk and network monitors follow, each keeping the
        # rate directive the first request established
        rate = directives[1]
        for attr, monid, label, plural in (
                (PMIX_MONITOR_NODE_RESOURCE_USAGE, "nodemon", "NODE",
                 "nodes"),
                (PMIX_MONITOR_DISK_RESOURCE_USAGE, "dkmon", "DISK", "disks"),
                (PMIX_MONITOR_NET_RESOURCE_USAGE, "netmon", "NETWORK",
                 "networks")):
            drain()
            monitor = [{'key': attr,
                        'value': {'type': PMIX_STRING, 'array': []},
                        'val_type': PMIX_DATA_ARRAY}]
            directives = [{'key': PMIX_MONITOR_ID, 'value': monid,
                           'val_type': PMIX_STRING}, rate]
            if not run_monitor(monitor, directives, label, plural):
                return done()
            if not cancel_monitor(monid):
                return done()

    return done()


def done():
    proc = {'nspace': myproc['nspace'], 'rank': PMIX_RANK_WILDCARD}
    # call fence to synchronize with our peers - no need to
    # collect any info as we didn't "put" anything
    info = [{'key': PMIX_COLLECT_DATA, 'value': False, 'val_type': PMIX_BOOL}]
    rc = client.fence([proc], info)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Fence failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return rc

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
