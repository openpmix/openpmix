#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/dynamic.c
#
# Dynamic process management: rank 0 spawns a second job running this same
# script, shares the child namespace through the key-value store, and then
# both jobs connect, disconnect, and construct/destruct a group spanning
# the two namespaces.
#
# The child is distinguished by PMIX_ENV_VALUE being set in its
# environment; it takes the parent namespace and size as its arguments.

import os
import socket
import sys

from examples import *

MAXPROCS = 2


def main():
    hostname = socket.gethostname()
    maxprocs = MAXPROCS

    client = PMIxClient()

    # init us - a singleton comes up without a server and so init returns
    # PMIX_ERR_UNREACH rather than PMIX_SUCCESS. That is not a fatal
    # error: the library is fully usable, so accept it and carry on.
    rc, myproc = client.init(None)
    if PMIX_SUCCESS != rc and PMIX_ERR_UNREACH != rc:
        eprint("Client: PMIx_Init failed:", client.error_string(rc))
        sys.exit(0)
    eprint("Client ns %s rank %d: Running on host %s"
           % (myproc['nspace'], myproc['rank'], hostname))

    proc = {'nspace': myproc['nspace'], 'rank': PMIX_RANK_WILDCARD}

    # get our job size
    rc, val = client.get(proc, PMIX_JOB_SIZE, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get job size failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(client, myproc)
    nprocs = val['value']
    eprint("Client %s:%d job size %d"
           % (myproc['nspace'], myproc['rank'], nprocs))

    # call fence to sync
    rc = client.fence([proc], None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Fence failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(client, myproc)

    if os.getenv("PMIX_ENV_VALUE") is None:
        # we are the parent
        if 0 == myproc['rank']:
            # rank=0 calls spawn
            me = os.path.abspath(sys.argv[0])
            app = {'cmd': me,
                   'argv': [os.path.basename(me), myproc['nspace'],
                            str(nprocs)],
                   'maxprocs': maxprocs,
                   'env': ["PMIX_ENV_VALUE=3"],
                   'info': []}
            info = [{'key': PMIX_MAPBY, 'value': "node",
                     'val_type': PMIX_STRING}]
            eprint("Client ns %s rank %d: calling PMIx_Spawn"
                   % (myproc['nspace'], myproc['rank']))
            rc, nsp = client.spawn(info, [app])
            if PMIX_SUCCESS != rc:
                eprint("Client ns %s rank %d: PMIx_Spawn failed: %d"
                       % (myproc['nspace'], myproc['rank'],
                          rc))
                return done(client, myproc)

            # share the child namespace
            rc = client.put(PMIX_GLOBAL, "child",
                            {'value': nsp, 'val_type': PMIX_STRING})
            if PMIX_SUCCESS != rc:
                eprint("Client ns %s rank %d: PMIx_Put child nspace failed: %s"
                       % (myproc['nspace'], myproc['rank'],
                          client.error_string(rc)))
                return done(client, myproc)
            client.commit()

            # wait to sync with others
            client.fence([proc], None)
        else:
            client.fence([proc], None)
            # retrieve the child nspace
            proc['rank'] = 0
            rc, val = client.get(proc, "child", None)
            if PMIX_SUCCESS != rc:
                eprint("Client ns %s rank %d: PMIx_Get child nspace failed: %s"
                       % (myproc['nspace'], myproc['rank'],
                          client.error_string(rc)))
                return done(client, myproc)
            nsp = val['value']

        # everybody calls connect
        parray = [{'nspace': myproc['nspace'], 'rank': n}
                  for n in range(nprocs)]
        parray += [{'nspace': nsp, 'rank': n} for n in range(maxprocs)]
    else:
        # we are the child job
        nsp = sys.argv[1]
        maxprocs = int(sys.argv[2])
        # everybody calls connect
        parray = [{'nspace': nsp, 'rank': n} for n in range(maxprocs)]
        parray += [{'nspace': myproc['nspace'], 'rank': n}
                   for n in range(nprocs)]

    eprint("Client ns %s rank %d: calling PMIx_Connect"
           % (myproc['nspace'], myproc['rank']))
    rc = client.connect(parray, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Connect failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(client, myproc)
    if 0 == myproc['rank']:
        eprint("Client ns %s rank %d: PMIx_Connect GOOD"
               % (myproc['nspace'], myproc['rank']))

    rc = client.disconnect(parray, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Disconnect failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(client, myproc)
    if 0 == myproc['rank']:
        eprint("Client ns %s rank %d: PMIx_Disconnect GOOD"
               % (myproc['nspace'], myproc['rank']))

    rc, results = client.group_construct("mygrp", parray, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Group_construct failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(client, myproc)
    if 0 == myproc['rank']:
        eprint("Client ns %s rank %d: PMIx_Group_construct GOOD"
               % (myproc['nspace'], myproc['rank']))

    rc = client.group_destruct("mygrp", None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Group_destruct failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(client, myproc)
    if 0 == myproc['rank']:
        eprint("Client ns %s rank %d: PMIx_Group_destruct GOOD"
               % (myproc['nspace'], myproc['rank']))

    return done(client, myproc)


def done(client, myproc):
    # finalize us
    eprint("Client ns %s rank %d: Finalizing"
           % (myproc['nspace'], myproc['rank']))
    rc = client.finalize(None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d:PMIx_Finalize failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
    else:
        eprint("Client ns %s rank %d:PMIx_Finalize successfully completed"
               % (myproc['nspace'], myproc['rank']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
