#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/nodeinfo.c
#
# Retrieve node-level information: the list of nodes in the job, a peer's
# hostname, and that peer's fabric coordinates - first by asking for the
# peer's rank, then by using the PMIX_NODE_INFO/PMIX_HOSTNAME qualifiers
# to ask about the host itself.

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
        eprint("Client: PMIx_Init failed:", client.error_string(rc))
        sys.exit(0)
    eprint("Client ns %s rank %d: Running"
           % (myproc['nspace'], myproc['rank']))

    # job-related info is found in our nspace, assigned to the
    # wildcard rank as it doesn't relate to a specific rank. Setup
    # a name to retrieve such values
    proc = {'nspace': myproc['nspace'], 'rank': PMIX_RANK_WILDCARD}

    # get our job size
    rc, val = client.get(proc, PMIX_JOB_SIZE, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get job size failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(client, myproc)
    nprocs = val['value']
    eprint("Client %s:%d job size %d"
           % (myproc['nspace'], myproc['rank'], nprocs))

    # get the list of nodes being used
    rc, val = client.get(proc, PMIX_NODE_LIST, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get node list failed: %s"
               % (myproc['nspace'], myproc['rank'], client.error_string(rc)))
        return done(client, myproc)
    if 0 == myproc['rank']:
        eprint("Client ns %s rank %d: Host list %s"
               % (myproc['nspace'], myproc['rank'], val['value']))
    nodes = val['value'].split(',')
    if 0 == len(nodes):
        return done(client, myproc)

    # get some node-specific info
    if 1 < nprocs:
        proc['rank'] = myproc['rank'] + 1
        if nprocs == proc['rank']:
            proc['rank'] = 0
        rc, val = client.get(proc, PMIX_HOSTNAME, None)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Get hostname for rank %u "
                   "failed: %s" % (myproc['nspace'], myproc['rank'],
                                   proc['rank'], client.error_string(rc)))
            return done(client, myproc)
        eprint("Client ns %s rank %d: Rank %u is on host %s"
               % (myproc['nspace'], myproc['rank'], proc['rank'],
                  val['value']))
        hostname = val['value']

        rc, val = client.get(proc, PMIX_FABRIC_COORDINATES, None)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Get coordinates for rank %u "
                   "failed: %s" % (myproc['nspace'], myproc['rank'],
                                   proc['rank'], client.error_string(rc)))
        else:
            _, tmp = client.value_string(val)
            eprint("Client ns %s rank %d: Rank %u has coordinates\n    %s"
                   % (myproc['nspace'], myproc['rank'], proc['rank'], tmp))

        # try with directive
        proc['rank'] = PMIX_RANK_WILDCARD
        info = [{'key': PMIX_NODE_INFO, 'value': True, 'val_type': PMIX_BOOL},
                {'key': PMIX_HOSTNAME, 'value': hostname,
                 'val_type': PMIX_STRING}]
        rc, val = client.get(proc, PMIX_FABRIC_COORDINATES, info)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Get coordinates with directive "
                   "for host %s failed: %s"
                   % (myproc['nspace'], myproc['rank'], hostname,
                      client.error_string(rc)))
            return done(client, myproc)
        _, tmp = client.value_string(val)
        eprint("Client ns %s rank %d: Host %s has coordinates\n    %s"
               % (myproc['nspace'], myproc['rank'], hostname, tmp))

    return done(client, myproc)


def done(client, myproc):
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
