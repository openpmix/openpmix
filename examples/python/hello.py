#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/hello.c
#
# Report where we are running, then ask the server about the process sets
# it knows about using the non-blocking query. Pass "true" as the first
# argument to add the refresh-cache qualifier to the query.

import os
import socket
import sys

from examples import *


# this is a callback function for the query API. The query will callback
# with a status indicating if the request could be fully satisfied,
# partially satisfied, or completely failed. The results parameter holds
# the returned data, each entry carrying the key that was provided in the
# query call - so you can correlate the returned value to the requested
# key.
#
# Unlike the C version there is no release_fn to call: the binding has
# already converted the library's data to Python and released it before
# this function runs.
def cbfunc(status, results, lock):
    lock.status = status

    eprint("Query returned", len(results), "values status",
           lock.client.error_string(status))
    # print out the returned keys and values
    for item in results:
        _, txt = lock.client.info_string(item)
        eprint(txt)

    # release the block
    lock.wakeup()


def main():
    refresh = False
    singleton = False

    if 1 < len(sys.argv):
        if "true" in sys.argv[1]:
            refresh = True

    pid = os.getpid()
    hostname = socket.gethostname()

    client = PMIxClient()

    # init us - note that the call to "init" includes the return of
    # any job-related info provided by the RM. This includes any
    # debugger flag instructing us to stop-in-init. If such a directive
    # is included, then the process will be stopped in this call until
    # the "debugger release" notification arrives
    rc, myproc = client.init(None)
    if PMIX_SUCCESS != rc:
        if PMIX_ERR_UNREACH == rc:
            # we are operating as a singleton. init() only fills in our
            # identity when it succeeds, so supply a placeholder - the C
            # version reads the (untouched) struct here
            singleton = True
            myproc = {'nspace': "UNKNOWN", 'rank': PMIX_RANK_UNDEF}
        else:
            eprint("Client: PMIx_Init failed:", client.error_string(rc))
            sys.exit(0)

    # get our local rank
    rc, val = client.get(myproc, PMIX_LOCAL_RANK, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get local rank failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(client, rc)
    localrank = val['value']

    eprint("Client ns %s rank %d pid %d: Running on host %s%slocalrank %d"
           % (myproc['nspace'], myproc['rank'], pid, hostname,
              " as singleton " if singleton else " ", localrank))

    if not singleton:
        query = {'keys': [PMIX_QUERY_NUM_PSETS, PMIX_QUERY_PSET_NAMES],
                 'qualifiers': None}
        if refresh:
            query['qualifiers'] = [{'key': PMIX_QUERY_REFRESH_CACHE,
                                    'value': True, 'val_type': PMIX_BOOL}]
        # setup the lock used to retrieve the data
        lock = MyLock()
        lock.client = client
        # execute the query
        rc = client.query_nb([query], cbfunc, lock)
        if PMIX_SUCCESS != rc:
            eprint("PMIx_Query_info failed: %d" % rc)
            return done(client, rc)
        lock.wait()
        if PMIX_SUCCESS != lock.status:
            rc = lock.status

    return done(client, rc)


def done(client, rc):
    # finalize us
    ret = client.finalize(None)
    if PMIX_SUCCESS != ret:
        eprint("Finalize failed:", client.error_string(ret))
        if PMIX_SUCCESS == rc:
            rc = ret
    return rc


if __name__ == '__main__':
    sys.exit(main())
