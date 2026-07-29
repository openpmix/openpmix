#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# A PMIx Python client for the dockerswarm harness, launched by prterun so that
# prted is its PMIx server.  Run it across at least two nodes -- that is the
# whole point.  The in-tree test/python scripts run a client and server in one
# process tree on one host, so every PMIx_Get is satisfied out of the local
# datastore and the remote-fetch path in the bindings is never touched.  Here
# each rank's peers live behind a *different* prted, so a get of a peer's key
# goes out to the server and comes back through the bindings' unload path.
#
# Every check prints a single "PMIXPY <rank> <PASS|FAIL> <name>" line so the
# driver can count results without parsing prose.  Exits non-zero if any check
# on this rank failed.

from pmix import *
import os
import sys

myrank = -1
nfail = 0


def check(name, ok, detail=""):
    global nfail
    if not ok:
        nfail += 1
    print("PMIXPY %d %s %s%s" % (myrank, "PASS" if ok else "FAIL", name,
                                 ("  [" + str(detail) + "]") if detail else ""),
          flush=True)


def main():
    global myrank

    client = PMIxClient()
    rc, myproc = client.init([])
    if PMIX_SUCCESS != rc:
        print("PMIXPY -1 FAIL init [%d]" % rc, flush=True)
        return 1
    myrank = myproc['rank']
    nspace = myproc['nspace']
    check("init", True, "%s:%d" % (nspace, myrank))

    # How big is this job?  PMIX_JOB_SIZE is job-level data, so it is keyed to
    # the WILDCARD rank, not to any individual proc -- asking for it against
    # our own rank returns PMIX_ERR_NOT_FOUND.
    jobproc = {'nspace': nspace, 'rank': PMIX_RANK_WILDCARD}
    rc, val = client.get(jobproc, PMIX_JOB_SIZE, [])
    check("get PMIX_JOB_SIZE", PMIX_SUCCESS == rc and val is not None, rc)
    if PMIX_SUCCESS != rc or val is None:
        client.finalize([])
        return 1
    nprocs = val['value']
    check("job size >= 2 (multi-rank run)", nprocs >= 2, nprocs)

    # confirm the ranks really are spread over more than one node, otherwise
    # the remote-fetch assertions below are not actually testing anything
    rc, hval = client.get(myproc, PMIX_HOSTNAME, [])
    myhost = hval['value'] if (PMIX_SUCCESS == rc and hval) else "?"
    check("get PMIX_HOSTNAME", PMIX_SUCCESS == rc, myhost)

    # --- put / commit / fence / get across nodes ---------------------------
    #
    # Each rank publishes two values under global scope, fences, then reads
    # every *other* rank's copies.  Any peer on another node forces the server
    # round trip.
    rc = client.put(PMIX_GLOBAL, "swarm.rank",
                    {'value': myrank, 'val_type': PMIX_INT32})
    check("put INT32", PMIX_SUCCESS == rc, rc)
    rc = client.put(PMIX_GLOBAL, "swarm.text",
                    {'value': "rank-%d" % myrank, 'val_type': PMIX_STRING})
    check("put STRING", PMIX_SUCCESS == rc, rc)

    rc = client.commit()
    check("commit", PMIX_SUCCESS == rc, rc)

    procs = [{'nspace': nspace, 'rank': PMIX_RANK_WILDCARD}]
    rc = client.fence(procs, [])
    check("fence", PMIX_SUCCESS == rc, rc)

    remote_ok = True
    remote_seen = 0
    for peer in range(nprocs):
        if peer == myrank:
            continue
        p = {'nspace': nspace, 'rank': peer}
        rc, v = client.get(p, 'swarm.rank', [])
        if PMIX_SUCCESS != rc or v is None or v['value'] != peer:
            remote_ok = False
            check("get peer %d swarm.rank" % peer, False, "rc=%d v=%s" % (rc, v))
            continue
        rc, v = client.get(p, 'swarm.text', [])
        want = "rank-%d" % peer
        got = v['value'] if v else None
        if isinstance(got, bytes):
            got = got.decode('ascii')
        if PMIX_SUCCESS != rc or got != want:
            remote_ok = False
            check("get peer %d swarm.text" % peer, False, "rc=%d got=%s" % (rc, got))
            continue
        remote_seen += 1
    check("get every peer's INT32 + STRING", remote_ok, "%d peers" % remote_seen)

    # --- topology / cpuset, the part macOS cannot reach --------------------
    #
    # A connected client can ask for its own binding.  Linux has a real
    # CPU-binding API behind hwloc_get_cpubind, so unlike the mac these return
    # actual data and exercise pmix_unload_cpuset for real.
    rc = client.load_topology()
    check("load_topology", PMIX_SUCCESS == rc, rc)

    # NOTE: only the *client-role* cpuset calls belong here. The two string
    # generators are PMIxServer methods (they wrap PMIx_server_generate_*),
    # so a launched rank cannot call them -- swarm_cpuset.py covers those.
    rc, cpus = client.get_cpuset(PMIX_CPUBIND_PROCESS)
    if PMIX_SUCCESS == rc:
        shape_ok = (isinstance(cpus, dict)
                    and isinstance(cpus.get('cpus'), list)
                    and all(isinstance(c, int) for c in cpus['cpus'])
                    and isinstance(cpus.get('source'), str))
        check("get_cpuset returns {'source': str, 'cpus': [int]}", shape_ok, cpus)
        # the source must be a clean provider name, not the whole
        # "hwloc:0-3" string with the prefix left glued on
        check("get_cpuset source has no ':' left in it",
              ':' not in cpus.get('source', ''), cpus.get('source'))

        # compute_distances must accept exactly what get_cpuset produced.
        # It legitimately reports NOT_FOUND / NOT_AVAILABLE on a container with
        # no interesting devices; what must not happen is a type error or a
        # crash from the shape mismatch the bindings used to have.
        rc5, dists = client.compute_distances(cpus, [])
        shape_ok = isinstance(dists, list) and all(
            isinstance(d, dict) and 'uuid' in d and 'mindist' in d for d in dists)
        check("compute_distances accepts the get_cpuset dict",
              shape_ok and rc5 in (PMIX_SUCCESS, PMIX_ERR_NOT_FOUND,
                                   PMIX_ERR_NOT_AVAILABLE, PMIX_ERR_NOT_SUPPORTED,
                                   PMIX_ERR_UNREACH),
              "rc=%d n=%d" % (rc5, len(dists)))
    else:
        # unbound is a legitimate state; report it rather than failing
        check("get_cpuset (unbound is OK here)",
              rc in (PMIX_ERR_NOT_FOUND, PMIX_ERR_NOT_SUPPORTED), rc)

    # parse_cpuset_string is a client-role call and needs no binding at all
    rc, parsed = client.parse_cpuset_string("hwloc:0-3,8")
    check("parse_cpuset_string",
          PMIX_SUCCESS == rc and parsed.get('cpus') == [0, 1, 2, 3, 8], parsed)

    # relative locality between two peers, which runs the locality bit
    # conversion (a list, not a bitmask, on the Python side)
    rc, loc = client.get_relative_locality("hwloc:NM0:SK0:CR0:HT0",
                                           "hwloc:NM0:SK0:CR0:HT1")
    check("get_relative_locality returns a list of bits",
          PMIX_SUCCESS == rc and isinstance(loc, list) and len(loc) > 0, loc)

    rc = client.finalize([])
    check("finalize", PMIX_SUCCESS == rc, rc)
    # A final DONE line is what lets the driver tell "every check passed" from
    # "the process died partway through and simply stopped printing".
    print("PMIXPY %d DONE %d" % (myrank, nfail), flush=True)
    return 1 if nfail else 0


if __name__ == '__main__':
    sys.exit(main())
