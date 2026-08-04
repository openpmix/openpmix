#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Drive the non-blocking client bindings against a real, remote PMIx server.
#
# swarm_group.py already covers group_construct_nb/_destruct_nb.  This covers
# the rest of the _nb family - fence, get, publish/lookup/unpublish, connect/
# disconnect, query, log, job_control - because that machinery is where the
# bindings do their most delicate work and the least of it is reachable
# without a server:
#
#   * the caddy keeps the C input arrays alive past the method's return,
#     since PMIx does not copy them.  A lifetime bug there is a
#     use-after-free that only bites once the request is genuinely
#     outstanding - which it is not when the local datastore answers
#     immediately.  A peer behind a different prted makes it outstanding.
#   * the callback and its cbdata live in a module-global registry, and the
#     registry entry is the ownership token for the caddy.  Whether the
#     trampoline and the method's error path can both try to free it is only
#     visible under real completions.
#   * the trampolines run on the library's progress thread and must take the
#     GIL.  A missing "with gil" deadlocks or crashes there, not here.
#
# Every operation is run enough times to shake out a leak or a double free
# that a single call would not.
#
# Prints "PMIXPY <rank> <PASS|FAIL> <name>" per check and a final DONE line.
# Exits non-zero if any check on this rank failed.

from pmix import *
import sys
import threading

myrank = -1
nfail = 0

#: how many times to repeat each operation - enough that a caddy leak or a
#: double free has somewhere to show itself
REPEAT = 20

#: how long to wait for a completion before calling it hung
TIMEOUT = 60


def check(name, ok, detail=""):
    global nfail
    if not ok:
        nfail += 1
    print("PMIXPY %d %s %s%s" % (myrank, "PASS" if ok else "FAIL", name,
                                 ("  [" + str(detail) + "]") if detail else ""),
          flush=True)


class Completion(object):
    """Collect what a non-blocking callback was handed.

    The callback runs on the library's progress thread, so it must not do
    anything but record and wake - certainly not call back into a blocking
    PMIx operation.
    """

    def __init__(self):
        self.done = threading.Event()
        self.status = None
        self.results = None
        self.cbdata = None

    def op(self, status, cbdata):
        self.status = status
        self.cbdata = cbdata
        self.done.set()

    def info(self, status, results, cbdata):
        self.status = status
        self.results = results
        self.cbdata = cbdata
        self.done.set()

    def value(self, status, val, cbdata):
        self.status = status
        self.results = val
        self.cbdata = cbdata
        self.done.set()

    def wait(self):
        return self.done.wait(TIMEOUT)


def run(name, start, accept, token):
    """Start one non-blocking operation and wait for its completion.

    'start' is called with the callback and the cbdata token; 'accept' is
    the set of statuses the completion may legitimately report.  Returns
    (ok, detail).
    """
    comp = Completion()
    rc = start(comp, token)
    if PMIX_SUCCESS != rc:
        # the contract is that a callback fires if and only if the request
        # was accepted, so a refusal here is a clean outcome as long as the
        # status is one we expect
        if rc in accept:
            return True, "refused with %d (no callback)" % rc
        return False, "%s returned %d" % (name, rc)
    if not comp.wait():
        return False, "%s never completed" % name
    if comp.cbdata is not token:
        return False, "%s handed back the wrong cbdata" % name
    if comp.status not in accept:
        return False, "%s completed with %d" % (name, comp.status)
    return True, ""


def repeat(name, start, accept):
    """Run an operation REPEAT times, stopping at the first failure."""
    for i in range(REPEAT):
        token = {'iteration': i, 'name': name}
        ok, detail = run(name, start, accept, token)
        if not ok:
            check(name, False, "pass %d: %s" % (i, detail))
            return False
    check(name, True, "%d completions" % REPEAT)
    return True


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

    jobproc = {'nspace': nspace, 'rank': PMIX_RANK_WILDCARD}
    rc, val = client.get(jobproc, PMIX_JOB_SIZE, [])
    if PMIX_SUCCESS != rc or val is None:
        check("get PMIX_JOB_SIZE", False, rc)
        client.finalize([])
        return 1
    nprocs = val['value']
    check("job size >= 2 (multi-rank run)", nprocs >= 2, nprocs)

    everyone = [{'nspace': nspace, 'rank': PMIX_RANK_WILDCARD}]

    # --- a bad callback must be refused before anything is allocated ------
    #
    # A callback that can never run would strand the caddy for the life of
    # the process, so the methods check first
    for name, call in (
            ("fence_nb", lambda: client.fence_nb(everyone, [], None)),
            ("get_nb", lambda: client.get_nb(jobproc, PMIX_JOB_SIZE, [], 42)),
            ("publish_nb", lambda: client.publish_nb([], "not callable")),
            ("query_nb", lambda: client.query_nb([], None))):
        check("%s rejects a non-callable callback" % name,
              PMIX_ERR_BAD_PARAM == call())

    # --- fence_nb ---------------------------------------------------------
    #
    # A collective across every rank, so this one genuinely blocks on the
    # other nodes.  Note that every rank must post the same number of
    # fences in the same order for the collective to match up.
    ok = True
    for i in range(REPEAT):
        comp = Completion()
        token = {'iteration': i}
        rc = client.fence_nb(everyone, [], comp.op, token)
        if PMIX_SUCCESS != rc:
            check("fence_nb", False, "pass %d returned %d" % (i, rc))
            ok = False
            break
        if not comp.wait():
            check("fence_nb", False, "pass %d never completed" % i)
            ok = False
            break
        if PMIX_SUCCESS != comp.status or comp.cbdata is not token:
            check("fence_nb", False,
                  "pass %d status=%s" % (i, comp.status))
            ok = False
            break
    if ok:
        check("fence_nb across all ranks", True, "%d completions" % REPEAT)

    # --- put/commit, then get_nb of a peer's key --------------------------
    #
    # This is the remote fetch: the value lives behind another prted, so the
    # request is genuinely outstanding when get_nb returns and the caddy has
    # to keep the proc and the key alive until the callback fires.
    rc = client.put(PMIX_GLOBAL, "swarm.nb.rank",
                    {'value': myrank, 'val_type': PMIX_INT32})
    check("put", PMIX_SUCCESS == rc, rc)
    rc = client.put(PMIX_GLOBAL, "swarm.nb.blob",
                    {'value': {'bytes': bytes(range(256)), 'size': 256},
                     'val_type': PMIX_BYTE_OBJECT})
    check("put byte object", PMIX_SUCCESS == rc, rc)
    rc = client.commit()
    check("commit", PMIX_SUCCESS == rc, rc)
    rc = client.fence(everyone, [])
    check("fence", PMIX_SUCCESS == rc, rc)

    peer = (myrank + 1) % nprocs
    peerproc = {'nspace': nspace, 'rank': peer}

    ok = True
    for i in range(REPEAT):
        comp = Completion()
        token = {'iteration': i}
        rc = client.get_nb(peerproc, "swarm.nb.rank", [], comp.value, token)
        if PMIX_SUCCESS != rc or not comp.wait():
            check("get_nb of a peer's key", False,
                  "pass %d rc=%d completed=%s" % (i, rc, comp.done.is_set()))
            ok = False
            break
        if PMIX_SUCCESS != comp.status or comp.results is None \
           or comp.results.get('value') != peer:
            check("get_nb of a peer's key", False,
                  "pass %d status=%s val=%s" % (i, comp.status, comp.results))
            ok = False
            break
        if comp.cbdata is not token:
            check("get_nb of a peer's key", False, "wrong cbdata")
            ok = False
            break
    if ok:
        check("get_nb of a peer's key", True, "%d completions" % REPEAT)

    # the same, for a binary payload - the value trampoline converts the
    # library's pmix_value_t before the library releases it, and a payload
    # full of NUL and high bytes is where treating it as a string shows
    comp = Completion()
    rc = client.get_nb(peerproc, "swarm.nb.blob", [], comp.value, None)
    if PMIX_SUCCESS == rc and comp.wait() and PMIX_SUCCESS == comp.status:
        got = comp.results['value'] if comp.results else None
        payload = got.get('bytes') if isinstance(got, dict) else None
        check("get_nb of a binary byte object",
              payload == bytes(range(256)),
              "%d bytes" % (len(payload) if payload is not None else -1))
    else:
        check("get_nb of a binary byte object", False,
              "rc=%d status=%s" % (rc, comp.status))

    # a key that does not exist must report NOT_FOUND through the callback
    # rather than hanging or handing back a value
    comp = Completion()
    rc = client.get_nb(peerproc, "swarm.nb.absent",
                       [{'key': PMIX_TIMEOUT, 'value': 5,
                         'val_type': PMIX_INT}], comp.value, None)
    if PMIX_SUCCESS == rc:
        completed = comp.wait()
        check("get_nb of a missing key reports an error",
              completed and PMIX_SUCCESS != comp.status, comp.status)
    else:
        check("get_nb of a missing key reports an error",
              PMIX_SUCCESS != rc, rc)

    # --- publish / lookup / unpublish, non-blocking -----------------------
    #
    # These go to the server and back, and lookup_nb is the only user of the
    # pdata trampoline
    key = "swarm.nb.pub.%d" % myrank
    comp = Completion()
    rc = client.publish_nb([{'key': key, 'value': "published-%d" % myrank,
                             'val_type': PMIX_STRING}], comp.op)
    published = (PMIX_SUCCESS == rc and comp.wait()
                 and PMIX_SUCCESS == comp.status)
    check("publish_nb", published, "rc=%d status=%s" % (rc, comp.status))

    rc = client.fence(everyone, [])
    check("fence after publish", PMIX_SUCCESS == rc, rc)

    if published:
        peerkey = "swarm.nb.pub.%d" % peer
        comp = Completion()
        rc = client.lookup_nb([peerkey],
                              [{'key': PMIX_WAIT, 'value': 1,
                                'val_type': PMIX_INT}], comp.info)
        if PMIX_SUCCESS == rc and comp.wait() and PMIX_SUCCESS == comp.status:
            got = None
            for entry in (comp.results or []):
                if entry.get('key') == peerkey:
                    got = entry.get('value')
            if isinstance(got, bytes):
                got = got.decode('ascii')
            check("lookup_nb finds a peer's published key",
                  got == "published-%d" % peer, got)
        else:
            check("lookup_nb finds a peer's published key", False,
                  "rc=%d status=%s" % (rc, comp.status))

        comp = Completion()
        rc = client.unpublish_nb([key], [], comp.op)
        check("unpublish_nb",
              PMIX_SUCCESS == rc and comp.wait() and PMIX_SUCCESS == comp.status,
              "rc=%d status=%s" % (rc, comp.status))

    rc = client.fence(everyone, [])
    check("fence after unpublish", PMIX_SUCCESS == rc, rc)

    # --- query_nb ---------------------------------------------------------
    #
    # This one carries no info array of its own; the qualifiers ride inside
    # each query, and the caddy owns the whole pmix_query_t array
    repeat("query_nb",
           lambda c, t: client.query_nb(
               [{'keys': [PMIX_QUERY_NAMESPACES], 'qualifiers': []}],
               c.info, t),
           (PMIX_SUCCESS, PMIX_ERR_NOT_FOUND, PMIX_ERR_NOT_SUPPORTED))

    # a query with no qualifiers key at all - the builder used to index the
    # dict directly and raise KeyError
    ok, detail = run("query_nb without qualifiers",
                     lambda c, t: client.query_nb(
                         [{'keys': [PMIX_QUERY_NAMESPACES]}], c.info, t),
                     (PMIX_SUCCESS, PMIX_ERR_NOT_FOUND,
                      PMIX_ERR_NOT_SUPPORTED),
                     {'token': 1})
    check("query_nb without a qualifiers key", ok, detail)

    # --- log_nb -----------------------------------------------------------
    repeat("log_nb",
           lambda c, t: client.log_nb(
               [{'key': PMIX_LOG_STDERR, 'value': "swarm nb log\n",
                 'val_type': PMIX_STRING}], [], c.op, t),
           (PMIX_SUCCESS, PMIX_ERR_NOT_SUPPORTED))

    # --- job_control_nb ---------------------------------------------------
    #
    # carries two info arrays and a proc array in the same caddy
    repeat("job_control_nb",
           lambda c, t: client.job_control_nb(
               [{'nspace': nspace, 'rank': myrank}],
               [{'key': PMIX_JOB_CTRL_ID, 'value': "swarm-nb",
                 'val_type': PMIX_STRING}], c.info, t),
           (PMIX_SUCCESS, PMIX_ERR_NOT_SUPPORTED, PMIX_ERR_NOT_FOUND,
            PMIX_ERR_BAD_PARAM))

    # --- connect_nb / disconnect_nb ---------------------------------------
    #
    # collectives again, so every rank must post them in the same order
    comp = Completion()
    rc = client.connect_nb(everyone, [], comp.op)
    connected = (PMIX_SUCCESS == rc and comp.wait()
                 and comp.status in (PMIX_SUCCESS, PMIX_ERR_NOT_SUPPORTED))
    check("connect_nb", connected, "rc=%d status=%s" % (rc, comp.status))
    if connected and PMIX_SUCCESS == comp.status:
        comp = Completion()
        rc = client.disconnect_nb(everyone, [], comp.op)
        check("disconnect_nb",
              PMIX_SUCCESS == rc and comp.wait() and PMIX_SUCCESS == comp.status,
              "rc=%d status=%s" % (rc, comp.status))

    # --- an empty attribute list must reach the library as NULL -----------
    #
    # A non-NULL info array with a count of zero tells the library the array
    # is terminated by an end marker, and it walks off the allocation
    # looking for one
    ok, detail = run("fence_nb with an empty info list",
                     lambda c, t: client.fence_nb(everyone, [], c.op, t),
                     (PMIX_SUCCESS,), {'token': 2})
    check("fence_nb with an empty info list", ok, detail)
    ok, detail = run("fence_nb with a None info list",
                     lambda c, t: client.fence_nb(everyone, None, c.op, t),
                     (PMIX_SUCCESS,), {'token': 3})
    check("fence_nb with a None info list", ok, detail)

    rc = client.finalize([])
    check("finalize", PMIX_SUCCESS == rc, rc)
    print("PMIXPY %d DONE %d" % (myrank, nfail), flush=True)
    return 1 if nfail else 0


if __name__ == '__main__':
    sys.exit(main())
