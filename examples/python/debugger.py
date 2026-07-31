#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/debugger.c
#
# The debugger "front end". On an initial launch it asks the RM what it
# supports (co-spawn? stop-on-exec?), launches the application paused for
# attach, then launches one debugger daemon per node pointed at the
# application's namespace. With --attach <nspace> it instead verifies
# that the named job exists.

import os
import sys

from examples import *

tool = PMIxTool()
myproc = {}
waiting_for_debugger = MyLock()


# this is a callback function for the query API. The query will call back
# with a status indicating whether the request could be fully satisfied,
# partially satisfied, or completely failed; each returned entry carries
# the key that was provided in the query.
def querycbfunc(status, results, mq):
    for item in results:
        eprint("Transferring %s" % as_key(item['key']))
    mq.info = results
    # release the block
    mq.wakeup(status)


# this is the event notification function we pass down below when
# registering for general events - i.e., the default handler
def notification_fn(evhdlr, status, source, info, results):
    return PMIX_EVENT_ACTION_COMPLETE, None


# registered for the debugger job's termination
def release_fn(evhdlr, status, source, info, results):
    waiting_for_debugger.wakeup()
    return PMIX_EVENT_ACTION_COMPLETE, None


def attach_to_running_job(nspace):
    """Verify that the named namespace exists."""
    q = MyQuery()
    rc = tool.query_nb([{'keys': [PMIX_QUERY_NAMESPACES],
                         'qualifiers': None}], querycbfunc, q)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Query_info failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return -1
    q.wait()

    if not q.info:
        eprint("Query returned no info")
        return -1
    # the query should have returned a comma-delimited list of nspaces
    if PMIX_STRING != q.info[0]['val_type']:
        eprint("Query returned incorrect data type: %d"
               % q.info[0]['val_type'])
        return -1
    if q.info[0]['value'] is None:
        eprint("Query returned no active nspaces")
        return -1

    eprint("Query returned %s" % q.info[0]['value'])
    return 0


def spawn_debugger(appspace):
    """Launch one debugger daemon per node, pointed at appspace."""
    cwd = os.getcwd()
    debuggerd = os.path.join(cwd, "python", "debuggerd.py") \
        if os.path.isdir(os.path.join(cwd, "python")) \
        else os.path.join(os.path.dirname(os.path.abspath(__file__)),
                          "debuggerd.py")
    debugger = [{'cmd': debuggerd, 'argv': [debuggerd], 'cwd': cwd,
                 'maxprocs': 1, 'env': None, 'info': []}]
    # provide directives so the daemons go where we want, and let the RM
    # know these are debugger daemons
    dinfo = [
        # instruct the RM to launch one copy of the executable on each node
        {'key': PMIX_MAPBY, 'value': "ppr:1:node", 'val_type': PMIX_STRING},
        # these are debugger daemons
        {'key': PMIX_DEBUGGER_DAEMONS, 'value': True,
         'val_type': PMIX_BOOL},
        # the nspace being debugged
        {'key': PMIX_DEBUG_JOB, 'value': appspace,
         'val_type': PMIX_STRING},
        # notify us when the debugger job completes
        {'key': PMIX_NOTIFY_COMPLETION, 'value': True,
         'val_type': PMIX_BOOL}]
    # spawn the daemons
    eprint("Debugger: spawning %s" % debugger[0]['cmd'])
    rc, dspace = tool.spawn(dinfo, debugger)
    if PMIX_SUCCESS != rc:
        eprint("Debugger daemons failed to launch with error: %s"
               % tool.error_string(rc))
    eprint("SPAWNED DEBUGGERD")
    return rc


def main():
    global myproc

    nspace = None

    # Process any arguments we were given
    i = 1
    while i < len(sys.argv):
        arg = sys.argv[i]
        if arg in ("-h", "--help"):
            # print the usage message and exit
            pass
        if arg in ("-a", "--attach"):
            if nspace is not None:
                # can only support one
                eprint("Cannot attach to more than one nspace")
                sys.exit(1)
            # the next argument must be the nspace
            i += 1
            if i == len(sys.argv):
                # they goofed
                eprint("The %s option requires an <nspace> argument" % arg)
                sys.exit(1)
            nspace = sys.argv[i]
        else:
            eprint("Unknown option: %s" % arg)
            sys.exit(1)
        i += 1

    # use the system connection first, if available. Note that the C
    # version builds this directive but then passes ninfo == 0, so the
    # library never sees it - the port keeps that behavior
    info = []

    # init as a tool
    rc, myproc = tool.init(info)
    if PMIX_SUCCESS != rc:
        eprint("PMIx_tool_init failed: %d" % rc)
        sys.exit(rc)

    eprint("Tool ns %s rank %d: Running"
           % (myproc['nspace'], myproc['rank']))

    # register a default event handler
    tool.register_event_handler(None, None, notification_fn)

    # register another handler specifically for when the debugger job
    # completes
    tool.register_event_handler([PMIX_ERR_JOB_TERMINATED], None, release_fn)

    # if we are attaching to a running job, then attach to it
    if nspace is not None:
        rc = attach_to_running_job(nspace)
        if PMIX_SUCCESS != rc:
            eprint("Failed to attach to nspace %s: error code %d"
                   % (nspace, rc))
            return done(rc)
        return done(rc)

    # this is an initial launch - we need to launch the application plus
    # the debugger daemons, letting the RM know we are debugging so that
    # it will "pause" the app procs until we are ready. First we need to
    # know if this RM supports co-spawning of daemons with the
    # application, or if we need to launch the daemons as a separate spawn
    # command. The former is faster and more scalable, but not every RM
    # may support it. We also need to ask for debug support so we know if
    # the RM can stop-on-exec, or only supports stop-in-init
    myquery_data = MyQuery()
    eprint("Debugger: querying capabilities")
    rc = tool.query_nb([{'keys': [PMIX_QUERY_SPAWN_SUPPORT,
                                  PMIX_QUERY_DEBUG_SUPPORT],
                         'qualifiers': None}], querycbfunc, myquery_data)
    if PMIX_SUCCESS != rc:
        eprint("PMIx_Query_info failed: %d" % rc)
        return done(rc)
    myquery_data.wait()

    # we should have received back two info structs, one containing a
    # comma-delimited list of PMIx spawn attributes the RM supports, and
    # the other containing a comma-delimited list of PMIx debugger
    # attributes it supports
    if 2 != len(myquery_data.info):
        # this is an error
        eprint("PMIx Query returned an incorrect number of results: %lu"
               % len(myquery_data.info))
        return done(rc)

    # we would like to co-spawn the debugger daemons with the app, but
    # let's first check to see if this RM supports that operation by
    # looking for the PMIX_COSPAWN_APP attribute in the spawn support.
    #
    # We will also check to see if "stop_on_exec" is supported. Few RMs
    # do so, which is why we have to check.
    #
    # Note that the PMIx reference server always returns the query results
    # in the same order as the query keys. However, this is not
    # guaranteed, so we search the returned info for the desired key
    cospawn = False
    stop_on_exec = False
    for item in myquery_data.info:
        if key_is(item['key'], PMIX_QUERY_SPAWN_SUPPORT):
            # see if the cospawn attribute is included
            cospawn = as_key(PMIX_COSPAWN_APP) in item['value']
        elif key_is(item['key'], PMIX_QUERY_DEBUG_SUPPORT):
            stop_on_exec = as_key(PMIX_DEBUG_STOP_ON_EXEC) in item['value']

    # if cospawn is true, then we can launch both the app and the debugger
    # daemons at the same time
    if cospawn:
        pass
    else:
        # we must do these as separate launches, so do the app first
        cwd = os.getcwd()   # point us to our current directory
        app = [{'cmd': "client", 'argv': ["./client"], 'cwd': cwd,
                'maxprocs': 2, 'env': None, 'info': []}]
        # provide job-level directives so the apps do what the user
        # requested
        info = [
            # map by slot
            {'key': PMIX_MAPBY, 'value': "slot", 'val_type': PMIX_STRING},
            # procs are to stop on first instruction, or else pause in
            # init for debugger attach
            {'key': PMIX_DEBUG_STOP_ON_EXEC if stop_on_exec
             else PMIX_DEBUG_STOP_IN_INIT, 'value': True,
             'val_type': PMIX_BOOL},
            # forward stdout and stderr to me
            {'key': PMIX_FWD_STDOUT, 'value': True, 'val_type': PMIX_BOOL},
            {'key': PMIX_FWD_STDERR, 'value': True, 'val_type': PMIX_BOOL}]

        # spawn the job - the call returns when the app has been launched
        eprint("Debugger: spawning %s" % app[0]['cmd'])
        rc, appspace = tool.spawn(info, app)
        if PMIX_SUCCESS != rc:
            eprint("Application failed to launch with error: %s(%d)"
                   % (tool.error_string(rc), rc))
            return done(rc)

        # now launch the debugger daemons
        rc = spawn_debugger(appspace)
        if PMIX_SUCCESS != rc:
            return done(rc)

    # this is where a debugger tool would wait until the debug operation
    # is complete
    waiting_for_debugger.wait()

    return done(rc)


def done(rc):
    tool.finalize()
    return rc


if __name__ == '__main__':
    sys.exit(main())
