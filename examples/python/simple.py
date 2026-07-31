#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/simple.c
#
# Init, finalize, then init and finalize again - the smallest possible
# check that the library can be brought up and torn down repeatedly.

import sys

from examples import *


def main():
    client = PMIxClient()

    # init us - note that the call to "init" includes the return of
    # any job-related info provided by the RM. This includes any
    # debugger flag instructing us to stop-in-init. If such a directive
    # is included, then the process will be stopped in this call until
    # the "debugger release" notification arrives
    rc, myproc = client.init(None)
    if PMIX_SUCCESS != rc:
        if PMIX_ERR_UNREACH != rc:
            eprint("Client: PMIx_Init failed:", client.error_string(rc))
            sys.exit(1)

    rc = client.finalize(None)
    if PMIX_SUCCESS != rc:
        eprint("Finalize failed:", client.error_string(rc))

    rc, myproc = client.init(None)
    if PMIX_SUCCESS != rc:
        if PMIX_ERR_UNREACH != rc:
            eprint("Client: PMIx_Init failed:", client.error_string(rc))
            sys.exit(1)

    # finalize us
    rc = client.finalize(None)
    if PMIX_SUCCESS != rc:
        eprint("Finalize failed:", client.error_string(rc))

    return rc


if __name__ == '__main__':
    sys.exit(main())
