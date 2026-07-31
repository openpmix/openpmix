#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/pset.c
#
# Wait to be told that a process set has been defined, then read back the
# process sets we now belong to.

import sys

from examples import *

# the C version passes a pointer to its myrel_t as PMIX_EVENT_RETURN_OBJECT
# and recovers it from the handler's info array. Python has no such
# pointer, so the handler uses this module-global instead
myrel = MyRel()
client = PMIxClient()


# this is the event notification function we pass down below when
# registering for general events - i.e., the default handler. We don't
# technically need to register one, but it is usually good practice to
# catch any events that occur
def notification_fn(evhdlr, status, source, info, results):
    eprint("Default error handler called with status %s"
           % client.error_string(status))
    return PMIX_EVENT_ACTION_COMPLETE, None


# this is an event notification function that we explicitly request be
# called when the PMIX_PROCESS_SET_DEFINE notification is issued
def release_fn(evhdlr, status, source, info, results):
    # the status will be PMIX_PROCESS_SET_DEFINE since that is the code
    # we registered to receive, so just return success
    myrel.status = PMIX_SUCCESS
    myrel.answer = find_key(info, PMIX_PSET_NAME)
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

    # register our default event handler
    rc, _ = client.register_event_handler(None, None, notification_fn)
    if PMIX_SUCCESS != rc:
        eprint("[%s:%d] Default handler registration failed"
               % (myproc['nspace'], myproc['rank']))
        return done(client, myproc)

    # register for process set name being defined
    rc, _ = client.register_event_handler([PMIX_PROCESS_SET_DEFINE], None,
                                          release_fn)
    if PMIX_SUCCESS != rc:
        eprint("[%s:%d] Debug handler registration failed"
               % (myproc['nspace'], myproc['rank']))
        return done(client, myproc)

    # wait for process set name to be defined
    myrel.wait()
    if myrel.answer is not None:
        eprint("Received process set name %s" % myrel.answer)
    else:
        eprint("Received bad answer")

    # check if I can retrieve my new pset membership
    rc, val = client.get(myproc, PMIX_PSET_NAMES, None)
    if PMIX_SUCCESS != rc:
        eprint("[%s:%d] PMIx_Get PMIX_PSET_NAMES returned %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(client, myproc)
    eprint("[%s:%d] belongs to psets %s"
           % (myproc['nspace'], myproc['rank'], val['value']))

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
