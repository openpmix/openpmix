#!/usr/bin/env python3
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Cover the *server-role* cpuset bindings against a real hwloc topology.
#
# PMIxServer.generate_cpuset_string and .generate_locality_string wrap
# PMIx_server_generate_*, so they are not reachable from a launched client rank
# (see swarm_client.py) and they are not meaningfully testable on macOS, where
# hwloc has no CPU-binding API to report against.  This script therefore stands
# up a bare PMIx server on one node -- no launcher, no clients -- and exercises
# them directly.
#
# It runs standalone: `python3 swarm_cpuset.py`.
#
# Prints "PMIXPY 0 <PASS|FAIL> <name>" per check plus a final DONE line, so
# run-python.sh can tally it exactly like the launched clients.

from pmix import *
import sys

nfail = 0


def check(name, ok, detail=""):
    global nfail
    if not ok:
        nfail += 1
    print("PMIXPY 0 %s %s%s" % ("PASS" if ok else "FAIL", name,
                                ("  [" + str(detail) + "]") if detail else ""),
          flush=True)


def noop_connected(proc, cbfunc, cbdata):
    return PMIX_OPERATION_SUCCEEDED


def main():
    srv = PMIxServer()
    rc = srv.init([], {'clientconnected': noop_connected})
    if PMIX_SUCCESS != rc:
        print("PMIXPY 0 FAIL server init [%d]" % rc, flush=True)
        return 1
    check("server init", True)

    rc = srv.load_topology()
    check("load_topology", PMIX_SUCCESS == rc, rc)

    # --- generate_cpuset_string -------------------------------------------
    cpus = {'source': 'hwloc', 'cpus': [0, 1, 2, 8]}
    rc, txt = srv.generate_cpuset_string(cpus)
    ok = PMIX_SUCCESS == rc and isinstance(txt, str) and txt.startswith('hwloc:')
    check("generate_cpuset_string", ok, txt)

    # and back again -- the library collapses runs into ranges ("0-2,8"),
    # so compare the expanded cpu lists, not the strings
    if ok:
        rc2, back = srv.parse_cpuset_string(txt)
        check("parse_cpuset_string round-trips generate_cpuset_string",
              PMIX_SUCCESS == rc2 and back['cpus'] == cpus['cpus'],
              "%s -> %s" % (txt, back))

    # a range-token input must produce the same thing as the index list
    rc, txt2 = srv.generate_cpuset_string({'source': 'hwloc', 'cpus': ['0-2', '8']})
    check("generate_cpuset_string accepts range tokens",
          PMIX_SUCCESS == rc and txt2 == txt, "%s vs %s" % (txt2, txt))

    # a preformatted range list must too
    rc, txt3 = srv.generate_cpuset_string({'source': 'hwloc', 'cpus': '0-2,8'})
    check("generate_cpuset_string accepts a preformatted range list",
          PMIX_SUCCESS == rc and txt3 == txt, "%s vs %s" % (txt3, txt))

    # --- generate_locality_string -----------------------------------------
    rc, loc = srv.generate_locality_string({'source': 'hwloc', 'cpus': [0]})
    check("generate_locality_string returns a locality string",
          PMIX_SUCCESS == rc and isinstance(loc, str) and len(loc) > 0, loc)

    if PMIX_SUCCESS == rc and isinstance(loc, str):
        # Two localities from the same cpuset must share everything.
        #
        # KNOWN LIBRARY DEFECT -- this check currently FAILS, and that is the
        # point of having it.  pmix.h documents PMIx_Get_relative_locality's
        # arguments as "String returned by the PMIx_server_generate_locality_string
        # API", but the generator emits a bare "SK0:CR0:HT0" while
        # pmix_hwloc_get_relative_locality requires an "hwloc:" prefix and
        # otherwise returns PMIX_ERR_TAKE_NEXT_OPTION (-1366).  So the
        # documented usage yields no locality at all.  PRRTE stores the
        # generator's output verbatim as PMIX_LOCALITY_STRING, so every
        # consumer of that attribute hits this.
        #
        # Do NOT "fix" this by prepending the prefix here -- that would hide
        # the defect.  See README.md, "Known failing check".
        rc2, loc2 = srv.generate_locality_string({'source': 'hwloc', 'cpus': [0]})
        if PMIX_SUCCESS == rc2:
            rc3, bits = srv.get_relative_locality(loc, loc2)
            check("get_relative_locality accepts generate_locality_string output",
                  PMIX_SUCCESS == rc3 and isinstance(bits, list) and len(bits) > 0,
                  "rc=%d bits=%s -- KNOWN DEFECT, see README" % (rc3, bits))
            # the same comparison works once the prefix is supplied by hand,
            # which is what pins the diagnosis on the missing prefix
            rc4, bits4 = srv.get_relative_locality("hwloc:" + loc, "hwloc:" + loc2)
            check("get_relative_locality works when 'hwloc:' is prepended",
                  PMIX_SUCCESS == rc4 and isinstance(bits4, list) and len(bits4) > 0,
                  bits4)

    # --- malformed input must be reported, not crash ----------------------
    #
    # These are the guards added after the bindings' cpuset layer went in: a
    # cpuset with no source used to segfault the library from inside
    # strncasecmp, and the annotations used to reject None before the body
    # could report a status.
    rc, val = srv.generate_cpuset_string(None)
    check("generate_cpuset_string(None) reports BAD_PARAM",
          PMIX_ERR_BAD_PARAM == rc and val is None, rc)

    rc, val = srv.generate_cpuset_string({'source': '', 'cpus': [0]})
    check("generate_cpuset_string(empty source) reports BAD_PARAM",
          PMIX_ERR_BAD_PARAM == rc and val is None, rc)

    rc, val = srv.generate_locality_string(None)
    check("generate_locality_string(None) reports BAD_PARAM",
          PMIX_ERR_BAD_PARAM == rc and val is None, rc)

    rc, val = srv.parse_cpuset_string(None)
    check("parse_cpuset_string(None) reports BAD_PARAM", PMIX_ERR_BAD_PARAM == rc, rc)

    rc, val = srv.parse_cpuset_string("no-delimiter")
    check("parse_cpuset_string(malformed) reports an error", PMIX_SUCCESS != rc, rc)

    # a source naming another provider is "not mine", not a hard error
    rc, val = srv.generate_cpuset_string({'source': 'someoneelse', 'cpus': [0]})
    check("generate_cpuset_string(foreign source) passes rather than crashing",
          PMIX_SUCCESS != rc and val is None, rc)

    srv.finalize()
    print("PMIXPY 0 DONE %d" % nfail, flush=True)
    return 1 if nfail else 0


if __name__ == '__main__':
    sys.exit(main())
