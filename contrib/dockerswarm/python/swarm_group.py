#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Exercise the PMIx group bindings from Python across real, separate PMIx
# servers.  Launched by prterun across at least two nodes so a group spans more
# than one prted -- the same reason the C group examples are run here.
#
# This covers both forms:
#   * PMIx_Group_construct / _destruct (blocking), and
#   * PMIx_Group_construct_nb / _destruct_nb, whose Python callbacks run on the
#     progress thread.  The non-blocking path is the one that carries the
#     keepalive caddy and the callback registry, so a cross-node construct is
#     where a lifetime bug in that machinery would actually show up.
#
# Prints "PMIXPY <rank> <PASS|FAIL> <name>" per check; exits non-zero on any
# failure on this rank.

from pmix import *
import sys
import threading

myrank = -1
nfail = 0

constructed = threading.Event()
destructed = threading.Event()
cb_status = {'construct': None, 'destruct': None}


def check(name, ok, detail=""):
    global nfail
    if not ok:
        nfail += 1
    print("PMIXPY %d %s %s%s" % (myrank, "PASS" if ok else "FAIL", name,
                                 ("  [" + str(detail) + "]") if detail else ""),
          flush=True)


# These run on the PMIx progress thread.  Record and release only -- a blocking
# PMIx call from here would deadlock.
def construct_cb(status, results, cbdata):
    cb_status['construct'] = status
    constructed.set()


def destruct_cb(status, cbdata):
    cb_status['destruct'] = status
    destructed.set()


def main():
    global myrank

    client = PMIxClient()
    rc, myproc = client.init([])
    if PMIX_SUCCESS != rc:
        print("PMIXPY -1 FAIL init [%d]" % rc, flush=True)
        return 1
    myrank = myproc['rank']
    check("init", True, "%s:%d" % (myproc['nspace'], myrank))

    # --- blocking construct/destruct over the whole job --------------------
    #
    # peers=None means "the whole job", which is what makes this span every
    # node the ranks were mapped onto.
    rc, results = client.group_construct("pygroup-blocking", None, [])
    check("group_construct (blocking)", PMIX_SUCCESS == rc, rc)
    if PMIX_SUCCESS == rc:
        rc = client.group_destruct("pygroup-blocking", [])
        check("group_destruct (blocking)", PMIX_SUCCESS == rc, rc)

    # --- non-blocking construct/destruct -----------------------------------
    rc = client.group_construct_nb("pygroup-nb", None, [], construct_cb, None)
    check("group_construct_nb accepted", PMIX_SUCCESS == rc, rc)
    if PMIX_SUCCESS == rc:
        fired = constructed.wait(timeout=60)
        check("group_construct_nb callback fired", fired)
        if fired:
            check("group_construct_nb status", PMIX_SUCCESS == cb_status['construct'],
                  cb_status['construct'])

            # destruct the group we just built non-blockingly.  This is the
            # case that used to fail with PMIX_ERR_NOT_FOUND: the client only
            # recorded the group on the blocking wrapper's completion path.
            rc = client.group_destruct_nb("pygroup-nb", [], destruct_cb, None)
            check("group_destruct_nb accepted", PMIX_SUCCESS == rc, rc)
            if PMIX_SUCCESS == rc:
                fired = destructed.wait(timeout=60)
                check("group_destruct_nb callback fired", fired)
                if fired:
                    check("group_destruct_nb status",
                          PMIX_SUCCESS == cb_status['destruct'],
                          cb_status['destruct'])

    rc = client.finalize([])
    check("finalize", PMIX_SUCCESS == rc, rc)
    # see the note in swarm_client.py -- proves the rank ran to completion
    print("PMIXPY %d DONE %d" % (myrank, nfail), flush=True)
    return 1 if nfail else 0


if __name__ == '__main__':
    sys.exit(main())
