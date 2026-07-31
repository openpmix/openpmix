#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Python port of examples/resolve.c
#
# Build a map of which node every process is running on, using
# PMIx_Resolve_nodes and PMIx_Resolve_peers - first for the original job,
# then again after spawning a second job and connecting to it, so the map
# spans both namespaces.
#
# The C version threads a "goto fn_fail" through every call; Python raises
# instead, and main() catches.

import os
import socket
import sys

from examples import *

SPAWN_PROCS = 2
SPAWN_NSPACE = "spawn.nspace"

client = PMIxClient()

# information about this process
own_proc = {}
parent_proc = {}
job_size = 0
parent_job_size = 0
is_spawned = False

# status and other
number_of_inits = 0
have_spawn = False
child_nspace = ""
nprocs = 0


class PMIxError(Exception):
    """The Python stand-in for the C version's CHECK_PMIX_ERR/goto."""


def check(rc, func_name):
    """The Python stand-in for CHECK_PMIX_ERR: any non-success bails."""
    if PMIX_SUCCESS != rc:
        print("[%s:%u]: Error on %s: %s"
              % (own_proc.get('nspace'), own_proc.get('rank', 0), func_name,
                 client.error_string(rc)))
        raise PMIxError(func_name)


def check_err(err):
    """The Python stand-in for CHECK_ERR.

    Note that this deliberately tests "> 0", exactly as the C macro does.
    That matters at the two places where the C passes a pmix_status_t to
    it: PMIx statuses are *negative*, so a failed connect or fence falls
    straight through and the example carries on. Reproducing the test
    keeps this port's behavior identical to the C version's.
    """
    if err > 0:
        raise PMIxError("check_err")


def do_basic_init():
    """Connect to the server, get our job size, and our spawned status."""
    global own_proc, parent_proc, job_size, is_spawned

    rc, own_proc = client.init(None)
    check(rc, "PMIx_Init")
    hostname = socket.gethostname()

    print("[%s:%u]: Running on node %s"
          % (own_proc['nspace'], own_proc['rank'], hostname))

    # Get job size
    wp = {'nspace': own_proc['nspace'], 'rank': PMIX_RANK_WILDCARD}
    rc, val = client.get(wp, PMIX_JOB_SIZE, None)
    check(rc, "PMIx_Get job size")
    job_size = val['value']

    # Get parent
    rc, val = client.get(own_proc, PMIX_PARENT_ID, None)
    if PMIX_ERR_NOT_FOUND == rc:
        is_spawned = False      # process not spawned
    elif PMIX_SUCCESS == rc:
        is_spawned = True       # spawned process
        parent_proc = {'nspace': val['value']['nspace'],
                       'rank': val['value']['rank']}
    else:
        is_spawned = False
        check(rc, "PMIx_Get parent ID")


def connect_procs():
    """Connect the two namespaces."""
    if is_spawned:
        # children load their own_proc nspace, and their parent
        procs = [{'nspace': own_proc['nspace'], 'rank': PMIX_RANK_WILDCARD},
                 {'nspace': parent_proc['nspace'], 'rank': 0}]
    else:
        # parent loads the child nspace, and their own nspace
        procs = [{'nspace': child_nspace, 'rank': PMIX_RANK_WILDCARD},
                 {'nspace': own_proc['nspace'], 'rank': 0}]
    print("[%s:%u]: Connect procs for %s with %s.0"
          % (own_proc['nspace'], own_proc['rank'], procs[0]['nspace'],
             procs[1]['nspace']))
    return client.connect(procs, None)


def get_spawned_nspace():
    """Get the nspace of the spawned procs.

    Used by every process that did NOT call spawn.
    """
    global child_nspace

    pid = os.getpid()
    rc, val = client.get(parent_proc, SPAWN_NSPACE, None)
    check(rc, "PMIx_Get spawned namespace name")
    child_nspace = val['value']
    print("[%s:%u]: Get spawned nspace (round %d, pid = %d) Result: %s "
          "Child %s" % (own_proc['nspace'], own_proc['rank'],
                        number_of_inits, pid, client.error_string(rc),
                        child_nspace))


def prepare():
    global nprocs, number_of_inits, have_spawn, parent_job_size

    nprocs = 0
    number_of_inits += 1
    pid = os.getpid()

    print("[%s:%u]: Init (round %d, pid = %d)"
          % (own_proc['nspace'], own_proc['rank'], number_of_inits, pid))

    # Create array with all active processes
    if 1 == number_of_inits:
        # 1st init - all processes in own namespace MUST be active
        nprocs += job_size

        if is_spawned:
            err = connect_procs()
            check_err(err)

            # Get job size of parent's nspace
            wp = {'nspace': parent_proc['nspace'],
                  'rank': PMIX_RANK_WILDCARD}
            rc, val = client.get(wp, PMIX_JOB_SIZE, None)
            check(rc, "PMIx_get size of parent's nspace")
            parent_job_size = val['value']
            nprocs += parent_job_size
    else:
        # From 2nd init onward
        if have_spawn:
            if parent_proc == own_proc:
                err = connect_procs()
                check_err(err)
            else:
                # Get name of new namespace
                get_spawned_nspace()
            # add size of spawned namespace to nprocs
            wp = {'nspace': child_nspace, 'rank': PMIX_RANK_WILDCARD}
            rc, val = client.get(wp, PMIX_JOB_SIZE, None)
            check(rc, "PMIx_get size of spawned nspace")
            nprocs += val['value']
            have_spawn = False

        nprocs += job_size


def do_spawn():
    """Spawn new processes and put the new nspace into the KVS."""
    global parent_proc, have_spawn, child_nspace

    # Rank 0 of original namespace is the parent of the spawn
    parent_proc = {'nspace': own_proc['nspace'], 'rank': 0}
    have_spawn = True

    if own_proc == parent_proc:
        print("[%s:%u] Spawning %d new processes."
              % (own_proc['nspace'], own_proc['rank'], SPAWN_PROCS))
        apps = [{'cmd': os.path.abspath(sys.argv[0]),
                 'maxprocs': SPAWN_PROCS, 'info': [], 'argv': None,
                 'env': None}]

        # spawn new procs
        rc, child_nspace = client.spawn(None, apps)
        check(rc, "PMIx_Spawn")

        # parent puts new child nspace into KVS
        rc = client.put(PMIX_GLOBAL, SPAWN_NSPACE,
                        {'value': child_nspace, 'val_type': PMIX_STRING})
        check(rc, "PMIx_Put child namespace (parent)")
        rc = client.commit()
        check(rc, "PMIx_Commit")

    # circulate the name of the child nspace
    jobproc = {'nspace': own_proc['nspace'], 'rank': PMIX_RANK_WILDCARD}
    err = client.fence([jobproc], None)
    check_err(err)


def get_node_list():
    """Return the list of nodes, in an order every process agrees on."""
    retain = []

    for i in range(2):
        if 0 == i:
            # Resolve nodes of own/parent nspace
            nspace = parent_proc['nspace'] if is_spawned \
                else own_proc['nspace']
        else:
            # Resolve nodes of child/own nspace
            nspace = own_proc['nspace'] if is_spawned else child_nspace
        rc, nodes = client.resolve_nodes(nspace)
        check(rc, "PMIx_Resolve_nodes")

        # Add non-duplicate nodes to list
        for node in nodes.split(','):
            is_duplicate = False
            for kept in retain:
                n = min(len(node), len(kept))
                if node[:n] == kept[:n]:
                    is_duplicate = True
            if not is_duplicate:
                retain.append(node)

    return retain


def get_proc_idx(p):
    if is_spawned:
        if p['nspace'] == parent_proc['nspace']:
            return p['rank']
        # p is a spawned proc, add parent job size
        return p['rank'] + parent_job_size
    if p['nspace'] == own_proc['nspace']:
        return p['rank']
    # p is a spawned proc, add job size
    return p['rank'] + job_size


def create_node_map():
    # Get list of all nodes
    nodelist = get_node_list()

    # Contains id of node on which each process is running, from 0
    nodemap = [-1] * nprocs
    # Contains either 0 (not used) or 1 (used) for each node
    used = [0] * len(nodelist)

    # Iterate over nodes to get processes (peers) running per node
    for i, node in enumerate(nodelist):
        for n in range(2):
            if 0 == n:
                # Resolve peers of own/parent nspace on node
                nspace = parent_proc['nspace'] if is_spawned \
                    else own_proc['nspace']
            else:
                # Resolve peers of child/own nspace on node
                if is_spawned:
                    nspace = own_proc['nspace']
                else:
                    if 0 == len(child_nspace):
                        break
                    nspace = child_nspace

            rc, node_procs = client.resolve_peers(node, nspace)
            if PMIX_ERR_INVALID_NAMESPACE == rc:
                print("[%s:%d] resolving peers: nspace %s is unknown"
                      % (own_proc['nspace'], own_proc['rank'], nspace))
            elif PMIX_ERR_NOT_FOUND == rc or 0 == len(node_procs):
                print("[%s:%d] resolving peers: nspace %s has no procs on "
                      "node %s" % (own_proc['nspace'], own_proc['rank'],
                                   nspace, node))
            elif PMIX_SUCCESS != rc:
                check(rc, "PMIx_Resolve_peers")
            else:
                print("[%s:%d] resolving peers: nspace %s has %lu procs on "
                      "node %s" % (own_proc['nspace'], own_proc['rank'],
                                   nspace, len(node_procs), node))

            if 0 < len(node_procs):
                used[i] = 1     # Remember if node is used
                # Iterate over peers to set their node id
                for p in node_procs:
                    nodemap[get_proc_idx(p)] = i

    # Print for debugging
    out_nodemap = "".join("%d " % m for m in nodemap)
    out_nodeused = "".join("%d " % u for u in used)
    print("[%s:%d] map: %s ### used %s"
          % (own_proc['nspace'], own_proc['rank'], out_nodemap, out_nodeused))


def main():
    try:
        do_basic_init()
        prepare()
        create_node_map()

        if not is_spawned:
            do_spawn()
            prepare()
            create_node_map()
    except PMIxError:
        print("[%s:%u]: ERROR!"
              % (own_proc.get('nspace'), own_proc.get('rank', 0)))

    rc = client.finalize(None)
    if PMIX_SUCCESS != rc:
        print("[%s:%u]: Error on PMIx_finalize: %s"
              % (own_proc.get('nspace'), own_proc.get('rank', 0),
                 client.error_string(rc)))
    print("[%s:%u]: Bye."
          % (own_proc.get('nspace'), own_proc.get('rank', 0)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
