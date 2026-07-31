#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/toolqry.c
#
# Attach as a tool, ask the server which namespaces are active, pick one
# that looks like an application job (its name ends in "@N" for non-zero
# N - "@0" is the DVM itself), and then read a couple of job-level values
# out of it.
#
# Pass --system-server to connect to the system-level server.
#
# The namespace list comes back as a data array of infos, each of which is
# itself a data array - so on the Python side it is a list of dicts whose
# values are lists of dicts.

import os
import socket
import sys

from examples import *


def querycbfunc(status, results, mq):
    mq.info = results
    # release the block
    mq.wakeup(status)


def main():
    system_server = False

    # check for directives
    for arg in sys.argv[1:]:
        if "--system-server" == arg:
            system_server = True

    tool = PMIxTool()

    # assign our own name
    hostname = socket.gethostname()
    toolname = os.path.basename(sys.argv[0])
    kptr = "%s.%s.%lu" % (toolname, hostname, os.getpid())
    info = [{'key': PMIX_TOOL_NSPACE, 'value': kptr,
             'val_type': PMIX_STRING},
            {'key': PMIX_TOOL_RANK, 'value': 0,
             'val_type': PMIX_PROC_RANK}]
    # if they want us to use a system server, then say so
    if system_server:
        info.append({'key': PMIX_CONNECT_TO_SYSTEM, 'value': True,
                     'val_type': PMIX_BOOL})

    # init as a tool
    rc, myproc = tool.init(info)
    if PMIX_SUCCESS != rc:
        eprint("PMIx_tool_init failed:", tool.error_string(rc))
        sys.exit(rc)
    proc = {'nspace': "", 'rank': 0}

    # query the list of active nspaces
    mydata = MyQuery()
    query = [{'keys': [PMIX_QUERY_NAMESPACE_INFO], 'qualifiers': None}]
    rc = tool.query_nb(query, querycbfunc, mydata)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Query_info failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(tool, rc)
    mydata.wait()
    rc = mydata.status
    # find the response
    if PMIX_SUCCESS != rc:
        eprint("Query returned error:", tool.error_string(mydata.status))
        return done(tool, rc)

    # should be in the first key
    if (not mydata.info
            or PMIX_QUERY_NAMESPACE_INFO != mydata.info[0]['key']):
        eprint("Query returned wrong info key at first posn: %s"
               % (mydata.info[0]['key'] if mydata.info else None))
        return done(tool, PMIX_ERROR)

    darray = mydata.info[0]['value']
    if darray is None or 0 == len(darray.get('array', [])):
        eprint("No namespaces active")
        return done(tool, PMIX_ERROR)

    # find our namespace
    for entry in darray['array']:
        dptr = entry['value']
        if dptr is None or 0 == len(dptr.get('array', [])):
            eprint("Error")
            return done(tool, PMIX_ERROR)
        iptr = dptr['array']
        # looking for nspaces with an "@N" at the end
        nsname = iptr[0]['value']
        _, _, suffix = nsname.rpartition('@')
        if not suffix or suffix == nsname:
            continue
        # ignore the "0" namespace as that is the DVM
        try:
            if 0 == int(suffix):
                continue
        except ValueError:
            continue
        # take this one
        proc = {'nspace': nsname, 'rank': 0}
        eprint("Received nspace %s" % proc['nspace'])
        break

    rc, val = tool.get(proc, PMIX_APPNUM, None)
    eprint("APPNUM RETURN:", tool.error_string(rc))
    if PMIX_SUCCESS == rc:
        _, tmp = tool.value_string(val)
        eprint("\t%s" % tmp)

    rc, val = tool.get(proc, PMIX_LOCALLDR, None)
    eprint("LOCALLDR RETURN:", tool.error_string(rc))
    if PMIX_SUCCESS == rc:
        _, tmp = tool.value_string(val)
        eprint("\t%s" % tmp)

    return done(tool, rc)


def done(tool, rc):
    tool.finalize()
    return rc


if __name__ == '__main__':
    sys.exit(main())
