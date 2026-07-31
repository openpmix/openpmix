#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/fault.c
#
# Fault notification: everyone registers for proc-aborted and
# job-terminated events restricted to our own namespace, then rank 0 exits
# with an error. The survivors block until the resulting event arrives.

import sys
import time

from examples import *

# the C version passes a pointer to its myrel_t as PMIX_EVENT_RETURN_OBJECT
# and recovers it from the handler's info array. Python has no such
# pointer, so the handler uses this module-global instead
myrel = MyRel()


def notification_fn(evhdlr, status, source, info, results):
    # not every RM will provide an exit code, but check if one was given
    exit_code = find_key(info, PMIX_EXIT_CODE)
    affected = find_key(info, PMIX_EVENT_AFFECTED_PROC)

    eprint("DEBUGGER DAEMON NOTIFIED TERMINATED - AFFECTED %s"
           % ("NULL" if affected is None else affected['nspace']))

    if exit_code is not None:
        myrel.exit_code = exit_code
        myrel.exit_code_given = True
    myrel.wakeup()

    # tell the event handler state machine that we are the last step
    return PMIX_EVENT_ACTION_COMPLETE, None


def main():
    client = PMIxClient()

    # init us
    rc, myproc = client.init(None)
    if PMIX_SUCCESS != rc:
        eprint("Client: PMIx_Init failed:", client.error_string(rc))
        sys.exit(0)
    eprint("Client ns %s rank %d: Running"
           % (myproc['nspace'], myproc['rank']))

    proc = {'nspace': myproc['nspace'], 'rank': PMIX_RANK_WILDCARD}

    # get our universe size
    rc, val = client.get(proc, PMIX_UNIV_SIZE, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get universe size failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(client, myproc, None)
    eprint("Client %s:%d universe size %d"
           % (myproc['nspace'], myproc['rank'], val['value']))

    # register a handler specifically for when the target job completes.
    # Only call me back when one of us terminates
    code = [PMIX_ERR_PROC_ABORTED, PMIX_ERR_JOB_TERMINATED]
    info = [{'key': PMIX_NSPACE, 'value': myproc['nspace'],
             'val_type': PMIX_STRING}]
    rc, refid = client.register_event_handler(code, info, notification_fn)
    eprint("Client %s:%d ERRHANDLER REGISTRATION CALLBACK CALLED WITH "
           "STATUS %d, ref=%d" % (myproc['nspace'], myproc['rank'], rc, refid))
    if PMIX_SUCCESS != rc:
        return done(client, myproc, None)

    # call fence to sync
    rc = client.fence([proc], None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Fence failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(client, myproc, refid)

    # rank=0 exits with an error
    if 0 == myproc['rank']:
        time.sleep(2)
        eprint("Client ns %s rank %d: exiting with error"
               % (myproc['nspace'], myproc['rank']))
        sys.exit(1)

    # everyone simply waits
    myrel.wait()

    return done(client, myproc, refid)


def done(client, myproc, refid):
    # finalize us
    eprint("Client ns %s rank %d: Finalizing"
           % (myproc['nspace'], myproc['rank']))
    if refid is not None:
        rc = client.deregister_event_handler(refid)
        eprint("Client %s:%d OP CALLBACK CALLED WITH STATUS %d"
               % (myproc['nspace'], myproc['rank'], rc))

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
