#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/abort.c
#
# Rank 0 waits a moment and then aborts the job; everyone else waits long
# enough to be killed by it. Run with more than one process to see the
# abort propagate.

import os
import sys
import time

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
        if PMIX_ERR_UNREACH == rc:
            eprint("Client: Cannot operate as singleton")
        else:
            eprint("Client: PMIx_Init failed:", client.error_string(rc))
        sys.exit(1)
    eprint("Client ns %s rank %d pid %d: Running"
           % (myproc['nspace'], myproc['rank'], pid))

    # rank=0 waits for a few seconds
    if 0 == myproc['rank']:
        time.sleep(2)
        # now call abort
        client.abort(PMIX_ERROR, "die", None)
    else:
        time.sleep(10)
        # finalize us
        eprint("Client ns %s rank %d: Finalizing"
               % (myproc['nspace'], myproc['rank']))
        rc = client.finalize(None)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d:PMIx_Finalize failed: %s"
                   % (myproc['nspace'], myproc['rank'],
                      client.error_string(rc)))
        else:
            eprint("Client ns %s rank %d:PMIx_Finalize successfully completed"
                   % (myproc['nspace'], myproc['rank']))

    return 0


if __name__ == '__main__':
    sys.exit(main())
