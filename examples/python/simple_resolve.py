#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/simple_resolve.c
#
# Ask the server which procs and which nodes belong to a namespace -
# first our own, then globally (a NULL namespace means "everything the
# server knows about").

import os
import sys

from examples import *


def main():
    pid = os.getpid()
    eprint("Client %d: Running" % pid)

    client = PMIxClient()

    # init us - note that the call to "init" includes the return of
    # any job-related info provided by the RM. This includes any
    # debugger flag instructing us to stop-in-init. If such a directive
    # is included, then the process will be stopped in this call until
    # the "debugger release" notification arrives
    rc, myproc = client.init(None)
    if PMIX_SUCCESS != rc:
        eprint("Client: PMIx_Init failed:", client.error_string(rc))
        sys.exit(0)
    eprint("Client ns %s rank %d pid %d: Running"
           % (myproc['nspace'], myproc['rank'], pid))

    rc, procs = client.resolve_peers(None, myproc['nspace'])
    eprint("ResPeers returned:", client.error_string(rc))
    if PMIX_SUCCESS == rc:
        for p in procs:
            eprint("\t%s:%u" % (p['nspace'], p['rank']))

    rc, nodelist = client.resolve_nodes(myproc['nspace'])
    eprint("ResNodes returned:", client.error_string(rc))
    if PMIX_SUCCESS == rc:
        eprint("\t%s" % nodelist)

    # now do global request
    rc, procs = client.resolve_peers(None, None)
    eprint("ResPeers global returned:", client.error_string(rc))
    if PMIX_SUCCESS == rc:
        for p in procs:
            eprint("\t%s:%u" % (p['nspace'], p['rank']))

    rc, nodelist = client.resolve_nodes(None)
    eprint("ResNodes global returned:", client.error_string(rc))
    if PMIX_SUCCESS == rc:
        eprint("\t%s" % nodelist)

    # finalize us
    rc = client.finalize(None)
    return rc


if __name__ == '__main__':
    sys.exit(main())
