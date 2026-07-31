#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/tool.c
#
# A general-purpose tool. What it does depends on the command line:
#
#   (no options)             list the active namespaces
#   -u|--url <uri>           connect to the server at that URI
#   --nspace <nspace>        report the job size of that namespace
#   --uri [<node>]           report a node's PMIx server URI
#   --spawn "<cmd args>"     spawn that command and wait for it to finish

import socket
import sys

from examples import *

tool = PMIxTool()
myproc = {}
# the C version passes a pointer to its myrel_t as PMIX_EVENT_RETURN_OBJECT
# and recovers it from the handler's info array. Python has no such
# pointer, so the handler uses this module-global instead
myrel = MyRel()


def querycbfunc(status, results, mq):
    mq.info = results
    mq.wakeup(status)


def release_fn(evhdlr, status, source, info, results):
    myrel.status = PMIX_SUCCESS
    myrel.wakeup()
    return PMIX_EVENT_ACTION_COMPLETE, None


def main():
    global myproc

    hostname = socket.gethostname()
    server_uri = None
    nspace = None
    nodename = None
    geturi = False
    spawn = None

    argv = sys.argv
    n = 1
    while n < len(argv):
        if argv[n] in ("-u", "--url"):
            if n + 1 >= len(argv):
                eprint("Must provide URI argument to %s option" % argv[n])
                sys.exit(1)
            server_uri = argv[n + 1]
            n += 1
        elif argv[n] in ("-nspace", "--nspace"):
            if n + 1 >= len(argv):
                eprint("Must provide nspace argument to %s option" % argv[n])
                sys.exit(1)
            nspace = argv[n + 1]
            n += 1
        elif argv[n] in ("-uri", "--uri"):
            # retrieve the PMIx server's uri from the indicated node
            nodename = argv[n + 1] if n + 1 < len(argv) else None
            geturi = True
            n += 1
        elif argv[n] in ("-spawn", "--spawn"):
            if n + 1 >= len(argv):
                eprint("Must provide executable argument to %s option"
                       % argv[n])
                sys.exit(1)
            spawn = argv[n + 1]
            n += 1
        n += 1

    info = []
    if server_uri is not None:
        info.append({'key': PMIX_SERVER_URI, 'value': server_uri,
                     'val_type': PMIX_STRING})
        eprint("Connecting to %s" % server_uri)
    if spawn is not None:
        info.append({'key': PMIX_TOOL_CONNECT_OPTIONAL, 'value': True,
                     'val_type': PMIX_BOOL})
        info.append({'key': PMIX_LAUNCHER, 'value': True,
                     'val_type': PMIX_BOOL})

    # init us
    rc, myproc = tool.init(info)
    if PMIX_SUCCESS != rc:
        eprint("PMIx_tool_init failed: %d" % rc)
        sys.exit(rc)

    if geturi:
        qualifiers = None
        if nodename is not None:
            qualifiers = [{'key': PMIX_HOSTNAME, 'value': nodename,
                           'val_type': PMIX_STRING}]
        mydata = MyQuery()
        rc = tool.query_nb([{'keys': [PMIX_SERVER_URI],
                             'qualifiers': qualifiers}],
                           querycbfunc, mydata)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Query_info failed: %d"
                   % (myproc['nspace'], myproc['rank'], rc))
            return done(rc)
        mydata.wait()
        # find the response
        if PMIX_SUCCESS == mydata.status:
            # should be in the first key
            if mydata.info and key_is(mydata.info[0]['key'],
                                      PMIX_SERVER_URI):
                eprint("PMIx server URI for node %s: %s"
                       % (hostname if nodename is None else nodename,
                          mydata.info[0]['value']))
            else:
                eprint("Query returned wrong info key at first posn: %s"
                       % (as_key(mydata.info[0]['key'])
                          if mydata.info else None))
        else:
            eprint("Query returned error:",
                   tool.error_string(mydata.status))
        return done(rc)

    # if we want to spawn a proc, then do so
    if spawn is not None:
        # setup notification so we know when the child has terminated.
        # Only call me back when this specific job terminates - the
        # children will have our nspace
        proc = {'nspace': myproc['nspace'], 'rank': PMIX_RANK_WILDCARD}
        hinfo = [{'key': PMIX_EVENT_AFFECTED_PROC, 'value': proc,
                  'val_type': PMIX_PROC}]
        rc, _ = tool.register_event_handler([PMIX_ERR_JOB_TERMINATED],
                                            hinfo, release_fn)

        argvlist = spawn.split(' ')
        app = [{'cmd': argvlist[0], 'argv': argvlist, 'maxprocs': 1,
                'env': None, 'info': []}]
        rc, child = tool.spawn(None, app)
        if PMIX_SUCCESS != rc and PMIX_OPERATION_SUCCEEDED != rc:
            eprint("Failed to spawn %s" % tool.error_string(rc))
            return done(rc)
        # wait here
        myrel.wait()
        return done(rc)

    if nspace is None:
        # query the list of active nspaces
        mydata = MyQuery()
        rc = tool.query_nb([{'keys': [PMIX_QUERY_NAMESPACE_INFO],
                             'qualifiers': None}], querycbfunc, mydata)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Query_info failed: %d"
                   % (myproc['nspace'], myproc['rank'], rc))
            return done(rc)
        mydata.wait()
        # find the response
        if PMIX_SUCCESS == mydata.status:
            # should be in the first key
            if mydata.info and key_is(mydata.info[0]['key'],
                                      PMIX_QUERY_NAMESPACE_INFO):
                darray = mydata.info[0]['value']
                eprint("ACTIVE NSPACES:")
                if darray is None or 0 == len(darray.get('array', [])):
                    eprint("\tNone")
                else:
                    for entry in darray['array']:
                        dptr = entry['value']
                        if dptr is None or 0 == len(dptr.get('array', [])):
                            eprint("Error in array %s"
                                   % ("NULL" if dptr is None else "NON-NULL"))
                            break
                        for item in dptr['array']:
                            if key_is(item['key'], PMIX_PROC_INFO_ARRAY):
                                eprint("")
                            _, txt = tool.info_string(item)
                            eprint("\t%s" % txt, end='')
                        eprint("\n")
            else:
                eprint("Query returned wrong info key at first posn: %s"
                       % (as_key(mydata.info[0]['key'])
                          if mydata.info else None))
        else:
            eprint("Query returned error:",
                   tool.error_string(mydata.status))
    else:
        mydata = MyQuery()
        rc = tool.query_nb([{'keys': [PMIX_JOB_SIZE],
                             'qualifiers': [{'key': PMIX_NSPACE,
                                             'value': nspace,
                                             'val_type': PMIX_STRING}]}],
                           querycbfunc, mydata)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Query_info failed: %d"
                   % (myproc['nspace'], myproc['rank'], rc))
            return done(rc)
        mydata.wait()
        # find the response
        if PMIX_SUCCESS == mydata.status:
            # should be in the first key
            if mydata.info and key_is(mydata.info[0]['key'], PMIX_JOB_SIZE):
                eprint("JOB SIZE FOR NSPACE %s: %lu"
                       % (nspace, mydata.info[0]['value']))
            else:
                eprint("Query returned wrong info key at first posn: %s"
                       % (as_key(mydata.info[0]['key'])
                          if mydata.info else None))
        else:
            eprint("Query returned error:",
                   tool.error_string(mydata.status))

    return done(rc)


def done(rc):
    # finalize us
    tool.finalize()
    return rc


if __name__ == '__main__':
    sys.exit(main())
