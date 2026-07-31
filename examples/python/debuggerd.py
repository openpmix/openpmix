#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/debuggerd.c
#
# The per-node debugger daemon. It attaches as a tool, learns which job it
# is meant to debug, asks its local server for the proc table of that
# job's *local* processes (so no single process has to fetch and scatter
# the whole table), and then notifies those processes that they are
# released - the PMIx equivalent of setting the MPIR breakpoint.
#
# NOTE: like the C version, this returns early after announcing itself.
# Everything below that point is kept in place - and kept in step with the
# C - but is not reached as written.

import sys
import time

from examples import *

tool = PMIxTool()
myproc = {}


# this is a callback function for the query API. The query will call back
# with a status indicating whether the request could be fully satisfied,
# partially satisfied, or completely failed; each returned entry carries
# the key that was provided in the query.
def querycbfunc(status, results, mq):
    for item in results:
        eprint("Transferring %s" % item['key'])
    mq.info = results
    # release the block
    mq.wakeup(status)


# this is the event notification function we pass down below when
# registering for general events - i.e., the default handler. We don't
# technically need to register one, but it is usually good practice to
# catch any events that occur
def notification_fn(evhdlr, status, source, info, results):
    return PMIX_EVENT_ACTION_COMPLETE, None


def main():
    global myproc

    eprint("I AM HERE")
    time.sleep(10)
    sys.exit(0)

    # init us - since we were launched by the RM, our connection info
    # will have been provided at startup
    rc, myproc = tool.init(None)
    if PMIX_SUCCESS != rc:
        eprint("Debugger daemon: PMIx_tool_init failed: %d" % rc)
        sys.exit(0)
    eprint("Debugger daemon ns %s rank %d: Running"
           % (myproc['nspace'], myproc['rank']))

    # register our default event handler
    rc, _ = tool.register_event_handler(None, None, notification_fn)
    if PMIX_SUCCESS != rc:
        eprint("Client %s:%d EVENT HANDLER REGISTRATION FAILED WITH "
               "STATUS %d" % (myproc['nspace'], myproc['rank'], rc))
        sys.exit(rc)

    # get the nspace of the job we are to debug
    proc = {'nspace': myproc['nspace'], 'rank': PMIX_RANK_WILDCARD}
    rc, val = tool.get(proc, PMIX_DEBUG_JOB, None)
    if PMIX_SUCCESS != rc:
        eprint("[%s:%d] Failed to get job being debugged - error %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done()
    if val is None:
        eprint("Got NULL return")
        return done()
    target = val['value']
    eprint("[%s:%d] Debugging %s"
           % (myproc['nspace'], myproc['rank'], target))

    # get our local proctable - for scalability reasons, we don't want to
    # have our "root" debugger process get the proctable for everybody and
    # send it out to us. So ask the local PMIx server for the pid's of our
    # local target processes
    myquery_data = MyQuery()
    query = [{'keys': [PMIX_QUERY_LOCAL_PROC_TABLE],
              # the nspace we are enquiring about
              'qualifiers': [{'key': PMIX_NSPACE, 'value': target,
                              'val_type': PMIX_STRING}]}]
    # execute the query
    rc = tool.query_nb(query, querycbfunc, myquery_data)
    if PMIX_SUCCESS != rc:
        eprint("PMIx_Query_info failed: %d" % rc)
        return done()
    myquery_data.wait()
    eprint("[%s:%d] Local proctable received"
           % (myproc['nspace'], myproc['rank']))

    # now that we have the proctable for our local processes, we can do
    # our magic debugger stuff and attach to them. We then send a
    # "release" event to them - i.e., it's the equivalent to setting the
    # MPIR breakpoint. We do this with the event notification system.
    # We send the notification to just the local procs of the job being
    # debugged
    tproc = {'nspace': target, 'rank': PMIX_RANK_WILDCARD}
    info = [{'key': PMIX_EVENT_CUSTOM_RANGE, 'value': tproc,
             'val_type': PMIX_PROC}]
    eprint("[%s:%u] Sending release"
           % (myproc['nspace'], myproc['rank']))
    tool.notify_event(PMIX_ERR_DEBUGGER_RELEASE, None, PMIX_RANGE_LOCAL,
                      info)

    # do some debugger magic
    eprint("[%s:%u] Hanging around awhile, doing debugger magic"
           % (myproc['nspace'], myproc['rank']))
    for _ in range(5):
        time.sleep(0.001)

    return done()


def done():
    # finalize us
    eprint("Debugger daemon ns %s rank %d: Finalizing"
           % (myproc['nspace'], myproc['rank']))
    rc = tool.finalize()
    if PMIX_SUCCESS != rc:
        eprint("Debugger daemon ns %s rank %d:PMIx_Finalize failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
    else:
        eprint("Debugger daemon ns %s rank %d:PMIx_Finalize successfully "
               "completed" % (myproc['nspace'], myproc['rank']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
