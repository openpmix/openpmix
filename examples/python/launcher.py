#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/launcher.c
#
# Act as a launcher: come up as a tool that does not connect to anything,
# ask the system server to spawn an application, and block until the
# job-termination event arrives.

import sys

from examples import *

# the C version passes a pointer to its myrel_t as PMIX_EVENT_RETURN_OBJECT
# and recovers it from the handler's info array. Python has no such
# pointer, so the handler uses this module-global instead
myrel = MyRel()
tool = PMIxTool()
myproc = {}


def notification_fn(evhdlr, status, source, info, results):
    # the job-termination status and the affected proc ride along with
    # the event; not every host supplies all of them
    jobstatus = find_key(info, PMIX_JOB_TERM_STATUS)
    msg = find_key(info, PMIX_EVENT_TEXT_MESSAGE)

    # save the status
    if jobstatus is not None:
        myrel.status = jobstatus
    if msg is not None:
        myrel.nspace = msg
    # release the lock
    myrel.wakeup()

    # we _always_ have to complete the event chain or else the event
    # progress engine will hang
    return PMIX_EVENT_ACTION_COMPLETE, None


def main():
    global myproc

    # we need to attach to a "system" PMIx server so we can ask it to
    # spawn applications for us. There can only be one such connection on
    # a node, so we will instruct the tool library to only look for it
    info = [{'key': PMIX_TOOL_DO_NOT_CONNECT, 'value': True,
             'val_type': PMIX_BOOL},
            {'key': PMIX_LAUNCHER, 'value': True, 'val_type': PMIX_BOOL},
            {'key': PMIX_IOF_LOCAL_OUTPUT, 'value': True,
             'val_type': PMIX_BOOL}]

    # initialize the library and make the connection
    rc, myproc = tool.init(info)
    if PMIX_SUCCESS != rc:
        eprint("PMIx_tool_init failed: %d" % rc)
        sys.exit(rc)

    # register an event handler so we can be notified when our spawned
    # job completes, or if it fails (even at launch)
    code = [PMIX_ERR_PROC_ABORTING,
            PMIX_ERR_PROC_ABORTED,
            PMIX_ERR_PROC_REQUESTED_ABORT,
            PMIX_ERR_JOB_TERMINATED,
            PMIX_ERR_UNREACH,
            PMIX_ERR_LOST_CONNECTION_TO_SERVER]
    rc, _ = tool.register_event_handler(code, None, notification_fn)
    if PMIX_SUCCESS != rc:
        eprint("[%s:%d] Default handler registration failed"
               % (myproc['nspace'], myproc['rank']))
        return done()

    # create our array of app structs describing the application we want
    # launched. Environmental params can also be provided in app['env']
    app = [{'cmd': "hello", 'argv': ["hello"], 'maxprocs': 1,
            'env': None, 'info': []}]

    # spawn the application
    rc, appspace = tool.spawn(None, app)

    # If the launch itself failed, the library reports it via the return
    # code rather than a job-termination event - so no event will ever
    # arrive to wake us, and we must not wait for one. Only block on the
    # completion event when the job actually started
    if PMIX_SUCCESS != rc:
        eprint("[%s:%d] Spawn failed: %s"
               % (myproc['nspace'], myproc['rank'], tool.error_string(rc)))
    else:
        myrel.wait()

    return done()


def done():
    tool.finalize()
    return 0


if __name__ == '__main__':
    sys.exit(main())
