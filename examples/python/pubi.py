#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/pubi.c
#
# The same publish/lookup dance as pub.py, but with no fence between the
# publish and the lookup: the lookup carries the PMIX_WAIT directive
# instead, telling the server to hold the request until the data shows up.
# Both keys are looked up in one call.

import sys

from examples import *


def main():
    client = PMIxClient()

    # init us
    rc, myproc = client.init(None)
    if PMIX_SUCCESS != rc:
        eprint("Client: PMIx_Init failed: %d" % rc)
        sys.exit(0)
    eprint("Client ns %s rank %d: Running"
           % (myproc['nspace'], myproc['rank']))

    # get our universe size
    proc = {'nspace': myproc['nspace'], 'rank': PMIX_RANK_WILDCARD}
    rc, val = client.get(proc, PMIX_UNIV_SIZE, None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Get universe size failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(client, myproc)
    eprint("Client %s:%d universe size %d"
           % (myproc['nspace'], myproc['rank'], val['value']))

    # publish something
    if 0 == myproc['rank']:
        eprint("%s:%d publishing two keys"
               % (myproc['nspace'], myproc['rank']))
        info = [{'key': "FOOBAR", 'value': 1, 'val_type': PMIX_UINT8},
                {'key': "PANDA", 'value': 123456, 'val_type': PMIX_SIZE}]
        rc = client.publish(info)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Publish failed: %d"
                   % (myproc['nspace'], myproc['rank'], rc))
            return done(client, myproc)
        eprint("%s:%d publish complete" % (myproc['nspace'], myproc['rank']))

    # lookup something - PMIX_WAIT of 0 means "wait for all of it"
    if 0 != myproc['rank']:
        eprint("%s:%d looking up key FOOBAR"
               % (myproc['nspace'], myproc['rank']))
        info = [{'key': PMIX_WAIT, 'value': 0, 'val_type': PMIX_INT}]
        rc, pdata = client.lookup([{'key': "FOOBAR"}, {'key': "PANDA"}], info)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Lookup failed: %d"
                   % (myproc['nspace'], myproc['rank'], rc))
            return done(client, myproc)
        # check the return for value and source
        for n in range(2):
            if myproc['nspace'] != pdata[n]['proc']['nspace']:
                eprint("Client ns %s rank %d: PMIx_Lookup returned wrong "
                       "nspace: %s" % (myproc['nspace'], myproc['rank'],
                                       pdata[n]['proc']['nspace']))
                return done(client, myproc)
            if 0 != pdata[n]['proc']['rank']:
                eprint("Client ns %s rank %d: PMIx_Lookup returned wrong "
                       "rank: %d" % (myproc['nspace'], myproc['rank'],
                                     pdata[n]['proc']['rank']))
                return done(client, myproc)
        if PMIX_UINT8 != pdata[0]['val_type']:
            eprint("Client ns %s rank %d: PMIx_Lookup returned wrong type: %d"
                   % (myproc['nspace'], myproc['rank'], pdata[0]['val_type']))
            return done(client, myproc)
        if 1 != pdata[0]['value']:
            eprint("Client ns %s rank %d: PMIx_Lookup returned wrong value: %d"
                   % (myproc['nspace'], myproc['rank'], pdata[0]['value']))
            return done(client, myproc)
        if PMIX_SIZE != pdata[1]['val_type']:
            eprint("Client ns %s rank %d: PMIx_Lookup returned wrong type: %d"
                   % (myproc['nspace'], myproc['rank'], pdata[1]['val_type']))
            return done(client, myproc)
        if 123456 != pdata[1]['value']:
            eprint("Client ns %s rank %d: PMIx_Lookup returned wrong value: %d"
                   % (myproc['nspace'], myproc['rank'], pdata[1]['value']))
            return done(client, myproc)
        eprint("PUBLISH-LOOKUP SUCCEEDED")

    # call fence so rank 0 waits before leaving
    rc = client.fence([proc], None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Fence failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))
        return done(client, myproc)

    if 0 == myproc['rank']:
        eprint("%s:%d unpublishing two keys"
               % (myproc['nspace'], myproc['rank']))
        rc = client.unpublish(["FOOBAR", "PANDA"], None)
        if PMIX_SUCCESS != rc:
            eprint("Client ns %s rank %d: PMIx_Unpublish failed: %d"
                   % (myproc['nspace'], myproc['rank'], rc))
            return done(client, myproc)
        eprint("UNPUBLISH SUCCEEDED")

    # call fence again so everyone waits for rank 0 before leaving
    rc = client.fence([proc], None)
    if PMIX_SUCCESS != rc:
        eprint("Client ns %s rank %d: PMIx_Fence failed: %d"
               % (myproc['nspace'], myproc['rank'], rc))

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
