#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/simple_server.c
#
# Bring a PMIx server up and down twice.
#
# One deviation from the C version: it declares a server module whose
# every entry is NULL, which the C API accepts. The Python binding
# deliberately refuses an empty module map ("SERVER REQUIRES AT LEAST ONE
# MODULE FUNCTION TO OPERATE"), so this port registers a single trivial
# handler instead. What the example is really exercising - that a server
# can be initialized and finalized repeatedly - is unchanged.

import sys

from examples import *


def clientconnected(proc, args):
    """The one module function we register, so init() has something."""
    return PMIX_SUCCESS


def main():
    server = PMIxServer()
    mymodule = {'clientconnected': clientconnected}

    # init us - note that the call to "init" includes the return of
    # any job-related info provided by the RM. This includes any
    # debugger flag instructing us to stop-in-init. If such a directive
    # is included, then the process will be stopped in this call until
    # the "debugger release" notification arrives
    rc = server.init(None, mymodule)
    if PMIX_SUCCESS != rc:
        eprint("Server_init failed:", server.error_string(rc))
        sys.exit(1)

    rc = server.finalize()
    if PMIX_SUCCESS != rc:
        eprint("Server finalize failed:", server.error_string(rc))

    eprint("Pass one completed")

    rc = server.init(None, mymodule)
    if PMIX_SUCCESS != rc:
        eprint("Server_init second time failed:", server.error_string(rc))
        sys.exit(1)

    # finalize us
    rc = server.finalize()
    if PMIX_SUCCESS != rc:
        eprint("Server finalize second time failed:",
               server.error_string(rc))

    eprint("Pass two completed")

    return rc


if __name__ == '__main__':
    sys.exit(main())
