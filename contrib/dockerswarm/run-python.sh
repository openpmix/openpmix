#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Exercise the PMIx **Python bindings** in the container swarm produced by
# build.sh.
#
#   ./run-python.sh linux   # in the 10-container swarm
#                           #   (requires: ./build.sh && docker compose up -d)
#   ./run-python.sh macos   # native single-host subset
#                           #   (requires: ./build.sh macos)
#
# Why this exists as its own script, alongside run-tests.sh and
# run-group-events.sh: the bindings' in-tree `make check` coverage is
# single-host, and the developer machine is usually macOS, where several
# cpuset/topology methods cannot run at all (Darwin has no CPU-binding API
# behind hwloc_get_cpubind, so get_cpuset returns PMIX_ERR_NOT_FOUND and
# compute_distances PMIX_ERR_UNREACH no matter what the bindings do).  Running
# them here gives the bindings (a) a Linux build, and (b) real multi-node
# clients, where a PMIx_Get of a peer's key is served by a *different* prted and
# so actually travels through the bindings' unload path.
#
# Prints PASS/FAIL per test and a summary; exits non-zero if anything failed.

set -uo pipefail

mode="${1:-linux}"
pass=0 fail=0 skip=0
ok()   { pass=$((pass+1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
skp()  { skip=$((skip+1)); printf '  \033[33mSKIP\033[0m %s\n' "$1"; }
banner() { printf '\n=== %s ===\n' "$1"; }

PYDIR=/opt/prte/tests-python

RUN() { docker exec -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 \
            pmix-node1 bash -lc ". /opt/prte/env.sh; $*"; }

cleanup_swarm() {
    for n in $(seq 1 10); do
        docker exec "pmix-node$n" sh -c \
            'pkill -9 -x prted 2>/dev/null; pkill -9 -x prte 2>/dev/null;
             pkill -9 prterun 2>/dev/null; rm -rf /tmp/prte.* /tmp/prun.session.* /tmp/pmix* 2>/dev/null; true'
    done
}

# Count the PMIXPY PASS/FAIL lines a client emitted.  Every swarm client prints
# "PMIXPY <rank> <PASS|FAIL> <name>" per check and, once it has run to the end,
# a final "PMIXPY <rank> DONE <nfail>".
#
# The DONE line is load-bearing: a client that dies partway -- an exception, a
# segfault -- simply stops printing, so counting only PASS/FAIL lines reports a
# clean run for a client that never finished.  Requiring one DONE per expected
# rank is what closes that hole.
tally() {
    local out="$1" label="$2" want_ranks="$3"
    local npass nfailed ndone
    npass=$(printf '%s\n' "$out" | grep -c 'PMIXPY [0-9-]* PASS ')
    nfailed=$(printf '%s\n' "$out" | grep -c 'PMIXPY [0-9-]* FAIL ')
    ndone=$(printf '%s\n' "$out" | grep -c 'PMIXPY [0-9-]* DONE ')
    if printf '%s\n' "$out" | grep -qiE 'time limit for job|timed out|DVM timeout'; then
        bad "$label: HUNG (hit the launch timeout)"
    elif [ "$nfailed" -gt 0 ]; then
        bad "$label: $nfailed check(s) failed"
        printf '%s\n' "$out" | grep 'PMIXPY [0-9-]* FAIL ' | sed 's/^/        /' | head -20
    elif [ "$npass" -eq 0 ]; then
        bad "$label: no checks reported: $(printf '%s' "$out" | tr '\n' ' ' | tail -c 240)"
    elif [ "$ndone" -lt "$want_ranks" ]; then
        bad "$label: only $ndone of $want_ranks ranks ran to completion"
        printf '%s\n' "$out" | grep -iE 'Traceback|Error|error:' | sed 's/^/        /' | head -10
    else
        ok "$label: $npass checks passed on $ndone ranks"
    fi
}

########################################################################
# Linux: the swarm
########################################################################

test_linux() {
    if ! docker ps --format '{{.Names}}' | grep -qx pmix-node1; then
        echo "swarm not up -- run: docker compose up -d" >&2; exit 2
    fi

    banner "preflight: bindings present and importable"
    if ! RUN "test -d $PYDIR"; then
        bad "$PYDIR missing -- rerun ./build.sh (with PYTHON_BINDINGS=yes)"; return
    fi
    out=$(RUN "python3 -c 'import pmix; print(pmix.PMIxClient().get_version())'" 2>&1)
    if [ $? = 0 ] && [ -n "$out" ]; then
        ok "import pmix on node1 (PMIx $(printf '%s' "$out" | tr -d '\r'))"
    else
        bad "cannot import pmix: $(printf '%s' "$out" | tr '\n' ' ' | tail -c 240)"; return
    fi

    # the extension must be importable on EVERY node that will host ranks,
    # otherwise a multi-node launch fails in a confusing way
    missing=""
    for n in 1 2 3 4; do
        docker exec "pmix-node$n" bash -lc \
            '. /opt/prte/env.sh; python3 -c "import pmix"' >/dev/null 2>&1 \
            || missing="$missing node$n"
    done
    [ -z "$missing" ] && ok "pmix importable on node1-4" \
                      || bad "pmix not importable on:$missing"

    banner "standalone unit suite (test_bindings.py)"
    cleanup_swarm
    out=$(RUN "cd $PYDIR && python3 ./test_bindings.py -v 2>&1"); rc=$?
    n=$(printf '%s\n' "$out" | grep -cE '\.\.\. ok$')
    if [ "$rc" = 77 ]; then
        skp "test_bindings.py skipped (extension not importable)"
    elif [ "$rc" = 0 ] && [ "$n" -gt 0 ]; then
        ok "test_bindings.py: $n unit tests passed on Linux"
    else
        bad "test_bindings.py failed (rc=$rc): $(printf '%s' "$out" | grep -E 'FAIL|Error' | tr '\n' ' ' | tail -c 300)"
    fi

    banner "connected client/server round-trip (server.py launches client.py)"
    cleanup_swarm
    # server.py forks client.py under its own PMIx server -- single host by
    # construction, but it is the only script that drives the *server* bindings
    # (register_nspace / register_client / setup_fork) and the upcall path.
    out=$(RUN "cd $PYDIR && timeout 120 python3 ./server.py 2>&1"); rc=$?
    if [ "$rc" = 124 ]; then
        bad "server.py HUNG"
    elif [ "$rc" = 0 ] && printf '%s\n' "$out" | grep -qi 'client finalized\|CLIENTFINALIZED'; then
        ok "server.py <-> client.py round-trip completed"
    else
        bad "server.py round-trip (rc=$rc): $(printf '%s' "$out" | tr '\n' ' ' | tail -c 300)"
    fi

    banner "server-role cpuset bindings against real hwloc"
    cleanup_swarm
    # Needs no launcher: stands up a bare PMIx server and drives the two
    # string generators, which a launched client rank cannot reach and which
    # macOS cannot exercise at all (no CPU-binding API behind hwloc).
    OUT="$(RUN "cd $PYDIR && timeout 120 python3 ./swarm_cpuset.py 2>&1")"
    tally "$OUT" "swarm_cpuset.py: generate/parse cpuset + locality strings" 1

    banner "multi-node Python clients (4 ranks over 2 nodes)"
    cleanup_swarm
    # This is the part that needs the swarm: each rank's peers are behind a
    # different prted, so the peer gets below are real server round trips.
    OUT="$(RUN "prterun --host node1:2,node2:2 -np 4 --map-by node --timeout 120 \
                    python3 $PYDIR/swarm_client.py 2>&1")"
    tally "$OUT" "swarm_client.py: put/commit/fence/get across nodes" 4
    cleanup_swarm

    banner "multi-node Python clients (8 ranks over 4 nodes)"
    cleanup_swarm
    OUT="$(RUN "prterun --host node1:2,node2:2,node3:2,node4:2 -np 8 --map-by node --timeout 120 \
                    python3 $PYDIR/swarm_client.py 2>&1")"
    tally "$OUT" "swarm_client.py: 8 ranks over 4 nodes" 8
    cleanup_swarm

    banner "multi-node Python group operations"
    cleanup_swarm
    # a group built from Python that spans two PMIx servers, both blocking and
    # non-blocking forms
    OUT="$(RUN "prterun --host node1:2,node2:2 -np 4 --map-by node --timeout 120 \
                    python3 $PYDIR/swarm_group.py 2>&1")"
    tally "$OUT" "swarm_group.py: group construct/destruct across nodes" 4
    cleanup_swarm
}

########################################################################
# macOS: native, single host
########################################################################

test_macos() {
    local root pfx pydir
    root="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
    pfx="$root/vpath-macos-pmix/install"
    if [ ! -d "$pfx" ]; then
        echo "native build missing -- run: ./build.sh macos" >&2; exit 2
    fi
    pydir="$(ls -d "$root"/vpath-macos-pmix/bindings/python/build/lib.* 2>/dev/null | head -1)"
    if [ -z "$pydir" ]; then
        skp "bindings not built natively (configure with --enable-python-bindings)"
        return
    fi
    export PYTHONPATH="$pydir${PYTHONPATH:+:$PYTHONPATH}"
    export DYLD_LIBRARY_PATH="$pfx/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"

    banner "macOS: standalone unit suite"
    if out=$(cd "$root/test/python" && python3 ./test_bindings.py 2>&1); then
        ok "test_bindings.py passed natively"
    else
        bad "test_bindings.py: $(printf '%s' "$out" | tr '\n' ' ' | tail -c 240)"
    fi
    skp "multi-node Python clients (needs the Linux swarm)"
    skp "get_cpuset / compute_distances (no CPU-binding API on Darwin)"
}

########################################################################

case "$mode" in
    linux) test_linux ;;
    macos) test_macos ;;
    *) echo "usage: $0 [linux|macos]" >&2; exit 2 ;;
esac

printf '\n================  %d passed, %d failed, %d skipped  ================\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ]
