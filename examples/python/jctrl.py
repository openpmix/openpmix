#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/jctrl.c
#
# Job control: tell the RM we are preemptible and describe how we can be
# checkpointed, then ask to be monitored by heartbeat and send one.
#
# Both requests use the non-blocking API and wait on a lock, exactly as
# the C version does.

import signal
import sys

from examples import *


# this is the event notification function we pass down below when
# registering for general events - i.e., the default handler. We don't
# technically need to register one, but it is usually good practice to
# catch any events that occur
def notification_fn(evhdlr, status, source, info, results):
    return PMIX_EVENT_ACTION_COMPLETE, None


def infocbfunc(status, results, lock):
    # release the caller. The binding has already converted the results
    # and released the library's copy
    lock.wakeup(status)


def main():
    client = PMIxClient()

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
        return done(client, myproc)

    # job-related info is found in our nspace, assigned to the
    # wildcard rank as it doesn't relate to a specific rank. Setup
    # a name to retrieve such values
    proc = {'nspace': myproc['nspace'], 'rank': PMIX_RANK_WILDCARD}

    # get our universe size
    rc, val = client.get(proc, PMIX_UNIV_SIZE, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get universe size failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(client, myproc)
    eprint("Client %s:%d universe size %d"
           % (myproc['nspace'], myproc['rank'], val['value']))

    # inform the RM that we are preemptible, and that our checkpoint
    # methods are "signal" on SIGUSR2 and event on PMIX_JCTRL_CHECKPOINT.
    # The checkpoint methods are a nested data array of infos - which in
    # Python is just a list inside the value dict
    methods = [{'key': PMIX_JOB_CTRL_CHECKPOINT_SIGNAL,
                'value': int(signal.SIGUSR2), 'val_type': PMIX_INT},
               {'key': PMIX_JOB_CTRL_CHECKPOINT_EVENT,
                'value': PMIX_JCTRL_CHECKPOINT, 'val_type': PMIX_STATUS}]
    info = [{'key': PMIX_JOB_CTRL_PREEMPTIBLE, 'value': True,
             'val_type': PMIX_BOOL},
            {'key': PMIX_JOB_CTRL_CHECKPOINT_METHOD,
             'value': {'type': PMIX_INFO, 'array': methods},
             'val_type': PMIX_DATA_ARRAY}]

    # since this is informational and not a requested operation, the
    # target parameter doesn't mean anything and can be ignored
    lock = MyLock()
    rc = client.job_control_nb(None, info, infocbfunc, lock)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Job_control_nb failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(client, myproc)
    lock.wait()
    if PMIX_SUCCESS != lock.status:
        eprint("Client ns %s rank %d: PMIx_Job_control_nb failed: %d"
               % (myproc['nspace'], myproc['rank'], lock.status))
        return done(client, myproc)

    # now request that this process be monitored using heartbeats. The C
    # version marks the request with a valueless PMIX_POINTER; a Python
    # program says the same thing with a boolean
    monitor = [{'key': PMIX_MONITOR_HEARTBEAT, 'value': True,
                'val_type': PMIX_BOOL}]
    info = [{'key': PMIX_MONITOR_ID, 'value': "MONITOR1",
             'val_type': PMIX_STRING},
            # require a heartbeat every 5 seconds
            {'key': PMIX_MONITOR_HEARTBEAT_TIME, 'value': 5,
             'val_type': PMIX_UINT32},
            # two heartbeats can be missed before declaring us "stalled"
            {'key': PMIX_MONITOR_HEARTBEAT_DROPS, 'value': 2,
             'val_type': PMIX_UINT32}]

    # make the request
    lock = MyLock()
    rc = client.monitor_nb(monitor, PMIX_MONITOR_HEARTBEAT_ALERT, info,
                           infocbfunc, lock)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Process_monitor_nb failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(client, myproc)
    lock.wait()
    if PMIX_SUCCESS != lock.status:
        eprint("Client ns %s rank %d: PMIx_Process_monitor_nb failed: %d"
               % (myproc['nspace'], myproc['rank'], lock.status))
        return done(client, myproc)

    # send a heartbeat
    client.heartbeat()

    # call fence to synchronize with our peers - no need to
    # collect any info as we didn't "put" anything
    info = [{'key': PMIX_COLLECT_DATA, 'value': False, 'val_type': PMIX_BOOL}]
    rc = client.fence([proc], info)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Fence failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))

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
