#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/bad_exit.c
#
# Query the process sets, then have rank 0 exit non-zero WITHOUT calling
# finalize while everyone else finalizes normally - so the host sees an
# abnormal termination it has to report.
#
# Takes an optional delay, in seconds, applied before the exit.

import os
import socket
import sys
import time

from examples import *


# this is a callback function for the query API. The query will call back
# with a status indicating whether the request could be fully satisfied,
# partially satisfied, or completely failed; each returned entry carries
# the key that was provided in the query.
def cbfunc(status, results, lock):
    lock.status = status

    eprint("Query returned %d values status %s"
           % (len(results), lock.client.error_string(status)))
    # print out the returned keys and values
    for item in results:
        eprint("KEY: %s" % item['key'])
        rc, tmp = lock.client.data_print(
            None, {'value': item['value'], 'val_type': item['val_type']})
        if PMIX_SUCCESS != rc:
            lock.status = rc
            break
        eprint("Key %s Type %s(%d)"
               % (item['key'],
                  lock.client.data_type_string(item['val_type']),
                  item['val_type']))

    # release the block
    lock.wakeup()


def main():
    delay = 0
    if 1 < len(sys.argv):
        delay = int(sys.argv[1])

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
        eprint("Client: PMIx_Init failed:", client.error_string(rc))
        sys.exit(0)

    # get our local rank
    rc, val = client.get(myproc, PMIX_LOCAL_RANK, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get local rank failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(client, myproc, delay)
    localrank = val['value']

    eprint("Client ns %s rank %d pid %d: Running on host %s localrank %d"
           % (myproc['nspace'], myproc['rank'], pid, hostname, localrank))

    query = [{'keys': [PMIX_QUERY_NUM_PSETS, PMIX_QUERY_PSET_NAMES],
              'qualifiers': None}]
    # setup the lock used to retrieve the data
    lock = MyLock()
    lock.client = client
    # execute the query
    rc = client.query_nb(query, cbfunc, lock)
    if PMIX_SUCCESS != rc:
        eprint("PMIx_Query_info failed: %d" % rc)
        return done(client, myproc, delay)
    lock.wait()

    time.sleep(delay)

    return done(client, myproc, 0)


def done(client, myproc, delay):
    if 0 == myproc['rank']:
        # leave without finalizing, and with a non-zero status
        sys.exit(1)
    time.sleep(2)
    # finalize us
    eprint("Client ns %s rank %d: Finalizing"
           % (myproc['nspace'], myproc['rank']))
    rc = client.finalize(None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d:PMIx_Finalize failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
    else:
        eprint("Client ns %s rank %d:PMIx_Finalize successfully completed"
               % (myproc['nspace'], myproc['rank']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
