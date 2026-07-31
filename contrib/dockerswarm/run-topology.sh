#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Exercise the src/hwloc topology + locality layer across the container swarm
# produced by build.sh.
#
#   ./run-topology.sh linux    # multi-node suite in the 10-container swarm
#                              #   (requires: ./build.sh && docker compose up -d)
#   ./run-topology.sh macos    # single-host subset natively on this host
#                              #   (requires: ./build.sh macos)
#
# WHY THIS NEEDS THE SWARM.  The in-tree unit test (test/unit/hwloc_datatype)
# covers pack/unpack/copy/print and the string routines in one process against
# a topology that process discovered for itself.  Three things it structurally
# cannot reach are what this script is for:
#
#   * the topology HANDOFF -- a launched client does not discover its topology,
#     it adopts the one its local server published (an hwloc shared-memory
#     segment, with XML as the fallback).  Nothing in a standalone process
#     takes those paths.
#   * PMIX_LOCALITY_STRING as the HOST actually stores it -- the string the
#     server generated with PMIx_server_generate_locality_string, handed
#     straight to PMIx_Get_relative_locality.  A unit test that writes its own
#     literals proves only that the literal matches the parser: that is exactly
#     how a producer/consumer format mismatch survived for years while every
#     process reported no shared locality with its own node-mates.
#   * the MULTI-NODE answer -- on one host every peer is a node-mate, so a
#     single-host run cannot distinguish a correct result from one that claims
#     everything is local.  The exerciser fails itself if it finds no off-node
#     peer, which is why the linux mode below always spans at least two nodes.
#
# Prints PASS/FAIL per case and a summary; exits non-zero if anything failed.

set -uo pipefail

mode="${1:-linux}"
pass=0 fail=0 skip=0
ok()   { pass=$((pass+1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
skp()  { skip=$((skip+1)); printf '  \033[33mSKIP\033[0m %s\n' "$1"; }
banner() { printf '\n=== %s ===\n' "$1"; }

. "$(dirname "$0")/swarm-common.sh"

# Tally the per-rank summary lines the exerciser prints:
#   TOPO rank <n>: <k> passed, <j> failed
# $1 = captured output; echoes "<summaries seen> <checks failed>".
# A rank that never printed a summary is a failure in its own right -- it
# crashed or hung -- and is not the same thing as a rank that reported zero
# failures, so count the summaries too.
tally() {
    local out="$1" summaries failed
    summaries=$(echo "$out" | grep -c 'TOPO rank .*passed,')
    failed=$(echo "$out" | grep 'TOPO rank .*passed,' | sed 's/.*, //; s/ failed//' \
             | awk '{s+=$1} END {print s+0}')
    echo "$summaries $failed"
}

########################################################################
# Linux: the full swarm
########################################################################

test_linux() {
    swarm_up_or_die

    banner "preflight"
    if RUN 'test -x /opt/prte/tests/topology'; then
        ok "topology exerciser present in the shared volume"
    else
        bad "topology not built -- rerun ./build.sh"; return
    fi

    # --- two nodes, two ranks each -----------------------------------------
    # The minimum geometry that makes every assertion live: each rank has a
    # node-mate (so the locality comparison runs) and an off-node peer (so the
    # "everything is local" failure mode is detectable).
    banner "topology + locality across two nodes"
    cleanup_swarm
    OUT="$(RUN 'prterun --host node1:2,node2:2 -np 4 --map-by node --timeout 120 /opt/prte/tests/topology 2>&1')"
    read -r summaries failed <<<"$(tally "$OUT")"
    if echo "$OUT" | grep -qiE 'time limit for job|timed out|DVM timeout'; then
        bad "HUNG (hit the launch timeout)"
    elif [ "$summaries" != 4 ]; then
        bad "only $summaries of 4 ranks reported: $(echo "$OUT" | tr '\n' ' ' | tail -c 240)"
    elif [ "$failed" != 0 ]; then
        bad "$failed checks failed across 4 ranks:"
        echo "$OUT" | grep 'TOPO rank .*FAIL' | sed 's/^/       /'
    else
        ok "all 4 ranks loaded a topology and agreed on locality"
    fi

    # --- wider: four nodes, two ranks each ---------------------------------
    # Same assertions with three times as many off-node peers per rank, and
    # four distinct servers publishing four topologies.
    banner "topology + locality across four nodes"
    cleanup_swarm
    OUT="$(RUN 'prterun --host node1:2,node2:2,node3:2,node4:2 -np 8 --map-by node --timeout 120 /opt/prte/tests/topology 2>&1')"
    read -r summaries failed <<<"$(tally "$OUT")"
    if echo "$OUT" | grep -qiE 'time limit for job|timed out|DVM timeout'; then
        bad "HUNG (hit the launch timeout)"
    elif [ "$summaries" != 8 ]; then
        bad "only $summaries of 8 ranks reported"
    elif [ "$failed" != 0 ]; then
        bad "$failed checks failed across 8 ranks:"
        echo "$OUT" | grep 'TOPO rank .*FAIL' | sed 's/^/       /'
    else
        ok "all 8 ranks across 4 nodes agreed on locality"
    fi

    # --- with the shared-memory topology path disabled ---------------------
    # pmix_hwloc_hole_kind=none makes the server skip the shmem segment
    # entirely, so every client falls back to the XML string.  Both paths have
    # to produce a topology that answers the same questions -- and the XML path
    # is the one that runs whenever a VM hole cannot be found, which is common
    # enough in containers that it must not be the untested one.
    #
    # PROVE the path actually changed before believing the result.  A wrong
    # parameter name is accepted in silence and simply leaves you on the shmem
    # path, so a green run here would mean the default case ran twice.  (That
    # is not hypothetical: this case was first written with the parameter
    # spelled "hwloc_hole_kind" -- the name the guide gave -- and passed
    # without ever exercising XML.  The registered name is
    # pmix_hwloc_hole_kind; ask pmix_info, do not guess.)  A persistent DVM
    # rather than prterun, so the daemons are up and inspectable while the
    # segment either exists or does not.
    banner "same, with the shmem topology path disabled (XML fallback)"
    cleanup_swarm
    RUN 'prte --daemonize --host node1:2,node2:2 --pmixmca pmix_hwloc_hole_kind none >/dev/null 2>&1; sleep 3'
    stillshmem=""
    for n in 1 2; do
        ON "$n" 'find /tmp -maxdepth 3 -name hwloc.sm 2>/dev/null | grep -q .' \
            && stillshmem="$stillshmem node$n"
    done
    if [ -n "$stillshmem" ]; then
        bad "shmem still enabled on:$stillshmem -- the XML path was NOT exercised"
    else
        ok "no shmem segment: the clients must use the XML string"
        OUT="$(RUN 'prun -np 4 --map-by node --timeout 120 /opt/prte/tests/topology 2>&1')"
        read -r summaries failed <<<"$(tally "$OUT")"
        if echo "$OUT" | grep -qiE 'time limit for job|timed out|DVM timeout'; then
            bad "HUNG on the XML fallback path"
        elif [ "$summaries" != 4 ]; then
            bad "only $summaries of 4 ranks reported on the XML path"
        elif [ "$failed" != 0 ]; then
            bad "$failed checks failed on the XML path:"
            echo "$OUT" | grep 'TOPO rank .*FAIL' | sed 's/^/       /'
        else
            ok "all 4 ranks agreed on locality with shmem disabled"
        fi
    fi
    RUN 'pterm >/dev/null 2>&1'
    cleanup_swarm

    # --- the server leaves nothing behind ----------------------------------
    # setup_topology writes the shmem segment into the session dir.  This
    # checks the user-visible property -- nothing named hwloc.sm survives a
    # clean teardown -- which the session-dir removal also delivers; it does
    # not isolate pmix_hwloc_finalize's own unlink.  Still worth having: a
    # segment left behind after the daemons are gone is a real leak whichever
    # layer was supposed to remove it.
    banner "shmem segment is reclaimed at teardown"
    cleanup_swarm
    RUN 'prterun --host node1:2,node2:2 -np 4 --map-by node --timeout 120 /opt/prte/tests/topology >/dev/null 2>&1'
    leftovers=""
    for n in 1 2; do
        if ON "$n" 'ls /tmp/*/hwloc.sm /tmp/hwloc.sm' >/dev/null 2>&1; then
            leftovers="$leftovers node$n"
        fi
    done
    if [ -z "$leftovers" ]; then
        ok "no hwloc.sm left behind on node1/node2"
    else
        bad "hwloc.sm survived teardown on:$leftovers"
    fi
    cleanup_swarm
}

########################################################################
# macOS: native, single host
########################################################################
#
# Deliberately a SUBSET.  Everything about cross-node locality is unavailable
# here, and the exerciser reports its own "job actually spans more than one
# node" check as a failure on a single host -- correctly.  So on macOS we only
# assert that the topology handoff itself works through a real launcher, and
# report the rest as skipped rather than pretending it ran.

test_macos() {
    local root prefix out
    root="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
    prefix="$root/vpath-macos-prte/install"
    if [ ! -x "$prefix/bin/prterun" ]; then
        echo "native build missing -- run: ./build.sh macos" >&2; exit 2
    fi
    export DYLD_LIBRARY_PATH="$prefix/lib:$root/vpath-macos-pmix/install/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
    export PRTE_ALLOW_RUN_AS_ROOT=1 PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1
    mac_isolate "$prefix"

    banner "macOS: topology handoff through a real launcher (single host)"
    if [ ! -x "$prefix/bin/topology" ]; then
        skp "topology not built"; macpk; return
    fi
    macpk; sleep 1
    out="$("$MAC_BIN/prterun" -np 4 --timeout 120 "$prefix/bin/topology" 2>&1)"
    if [ "$(echo "$out" | grep -c 'TOPO rank .*passed,')" != 4 ]; then
        skp "only $(echo "$out" | grep -c 'TOPO rank .*passed,') of 4 ranks reported (native Darwin DVM can be flaky)"
    elif [ "$(echo "$out" | grep -c 'PASS load_topology')" = 4 ]; then
        ok "all 4 ranks loaded the topology their server published"
    else
        bad "a rank could not load a topology: $(echo "$out" | grep 'FAIL load_topology' | tr '\n' ' ')"
    fi
    skp "cross-node locality (needs the linux swarm)"
    macpk
}

########################################################################

case "$mode" in
    linux) test_linux ;;
    macos) test_macos ;;
    *) echo "usage: $0 [linux|macos]" >&2; exit 2 ;;
esac

printf '\n================  %d passed, %d failed, %d skipped  ================\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ]
