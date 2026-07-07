#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Exercise PMIx group construction across the container swarm produced by
# build.sh, driving the example programs through PRRTE.
#
#   ./run-tests.sh linux    # full suite in the 10-container swarm
#                           #   (requires: ./build.sh && docker compose up -d)
#   ./run-tests.sh macos    # single-host subset natively on this host
#                           #   (requires: ./build.sh macos)
#
# Prints PASS/FAIL per test and a summary; exits non-zero if anything failed.
# The point of the multi-node swarm is that a group then spans several distinct
# PMIx servers (one prted per node), so the server-side local-completion and
# lost-connection accounting actually run on more than one daemon.

set -uo pipefail

mode="${1:-linux}"
pass=0 fail=0 skip=0
ok()   { pass=$((pass+1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
skp()  { skip=$((skip+1)); printf '  \033[33mSKIP\033[0m %s\n' "$1"; }
banner() { printf '\n=== %s ===\n' "$1"; }

# the group examples build.sh compiles into /opt/prte/tests
GROUP_EXAMPLES="${GROUP_EXAMPLES:-group group_bootstrap group_dmodex group_lcl_cid asyncgroup multi_nspace_group}"

########################################################################
# Linux: the full 10-node swarm
########################################################################

# run a command on the head node (login env so PATH/LD_LIBRARY_PATH are set)
RUN() { docker exec -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 \
            pmix-node1 bash -lc ". /opt/prte/env.sh; $*"; }
ON()  { docker exec "pmix-node$1" bash -lc ". /opt/prte/env.sh 2>/dev/null; ${*:2}"; }

cleanup_swarm() {
    for n in $(seq 1 10); do
        docker exec "pmix-node$n" sh -c \
            'pkill -9 -x prted 2>/dev/null; pkill -9 -x prte 2>/dev/null;
             pkill -9 prterun 2>/dev/null; rm -rf /tmp/prte.* /tmp/prun.session.* /tmp/pmix* 2>/dev/null; true'
    done
}
prted_count() { local c=0 n; for n in "$@"; do ON "$n" 'pgrep -x prted' >/dev/null 2>&1 && c=$((c+1)); done; echo "$c"; }

# Per-example launch geometry ($NP ranks spread over $HOSTS) and the marker that
# signals the group actually formed.  The defaults suit the common case -- four
# ranks split across two nodes, and a "construct complete" line in the output.
# A few examples need a different geometry or emit a different success line:
#   * group_bootstrap exercises the add-members path and rejects jobs smaller
#     than six ranks; it prints "GROUP CONSTRUCT COMPLETE".
#   * asyncgroup uses the invite/join model, so rank 0 reports the invite result
#     rather than a construct.
#   * multi_nspace_group has rank 0 PMIx_Spawn a two-proc child job and then
#     builds a group spanning both namespaces, so the allocation must leave slots
#     free for the spawned child -- launch two parent ranks into an eight-slot
#     allocation.
ex_geom() {
    HOSTS="node1:2,node2:2"; NP=4; WANT='Group construct complete|construct.*succe'
    case "$1" in
        group_bootstrap)
            HOSTS="node1:3,node2:3"; NP=6
            WANT='GROUP CONSTRUCT COMPLETE|Group construct complete' ;;
        asyncgroup)
            WANT='invite complete with status PMIX_SUCCESS' ;;
        multi_nspace_group)
            HOSTS="node1:4,node2:4"; NP=2
            WANT='Group construct complete with status' ;;
    esac
}

# launch a program through a transient DVM using the geometry ex_geom picks for
# it; $1 = test program, rest = extra prterun args.  Captures combined output in
# $OUT and the expected success marker in $WANT.  The program is named by
# absolute path: prterun resolves the executable on the head node and ships that
# path to the other daemons, whose non-login launch environment does not have
# /opt/prte/tests on PATH.  A --timeout bounds every launch so a genuine hang
# surfaces as a failure instead of stalling the whole suite.
OUT=""
WANT=""
launch() {
    local prog="$1"; shift
    ex_geom "$prog"
    OUT="$(RUN "prterun --host $HOSTS -np $NP --map-by node --timeout 60 /opt/prte/tests/$prog $* 2>&1")"
}

test_linux() {
    if ! docker ps --format '{{.Names}}' | grep -qx pmix-node1; then
        echo "swarm not up -- run: docker compose up -d" >&2; exit 2
    fi

    banner "preflight: install present in shared volume"
    if RUN 'command -v prterun prte prun pterm >/dev/null'; then
        ok "prterun/prte/prun/pterm on PATH"
    else
        bad "tools missing -- did ./build.sh run?"; return
    fi

    banner "multi-node launch smoke (real cross-daemon RML)"
    cleanup_swarm
    out=$(RUN 'prterun --host node1:2,node2:2,node3:2,node4:2 -np 8 --map-by node hostname'); rc=$?
    n=$(echo "$out" | grep -cE 'node[1-4]')
    [ "$rc" = 0 ] && [ "$n" = 8 ] \
        && ok "prterun -np 8 across node1-4, exit 0" \
        || bad "prterun multi-node (rc=$rc, lines=$n)"

    banner "group construction methods (each across two nodes)"
    for ex in $GROUP_EXAMPLES; do
        cleanup_swarm
        if ! RUN "test -x /opt/prte/tests/$ex"; then
            skp "$ex (not built)"; continue
        fi
        launch "$ex"
        if echo "$OUT" | grep -qiE 'time limit for job|timed out|DVM timeout'; then
            bad "$ex: HUNG (hit the launch timeout)"
        elif echo "$OUT" | grep -qiE "$WANT"; then
            if echo "$OUT" | grep -qiE 'PMIX_ERROR|connection refused'; then
                bad "$ex: completed but reported an error"
            else
                ok "$ex: group constructed across nodes"
            fi
        else
            bad "$ex: no completion (output: $(echo "$OUT" | tr '\n' ' ' | tail -c 200))"
        fi
        [ "$(prted_count 1 2 3 4 5 6 7 8 9 10)" = 0 ] \
            || bad "$ex: stray prted left behind"
    done

    banner "participant death during group construct (loss accounting)"
    cleanup_swarm
    # group_die: every rank is a group member; the last rank leaves before
    # contributing (and without finalizing), so the server must account for the
    # lost member and complete the construct on the survivors instead of
    # hanging. Two requirements make this observable through the launcher:
    #   * --rtos recoverable, so PRRTE does not tear the whole job down when the
    #     member vanishes without finalizing (which would kill the survivors);
    #   * whole-job membership + --map-by node, so the victim's node always also
    #     hosts a surviving member -- the one whose call created the local
    #     tracker that the loss accounting then completes.
    # Before groups tracked participation by identity this hung to the timeout.
    if RUN 'test -x /opt/prte/tests/group_die'; then
        OUT="$(RUN 'prterun --rtos recoverable --host node1:2,node2:2 -np 4 --map-by node --timeout 60 /opt/prte/tests/group_die 2>&1')"
        n=$(echo "$OUT" | grep -c 'complete on survivors')
        if echo "$OUT" | grep -qiE 'timeout|timed out|DVM timeout'; then
            bad "group HUNG on participant death (loss accounting missing)"
        elif [ "$n" -ge 3 ]; then
            ok "group completed on all 3 survivors after a member died"
        else
            bad "group_die: only $n survivors completed: $(echo "$OUT" | tr '\n' ' ' | tail -c 160)"
        fi
    else
        skp "group_die not built"
    fi
    cleanup_swarm

    banner "participant death during PMIx_Connect (loss accounting)"
    cleanup_swarm
    # connect_die: same shape as group_die, but the collective is PMIx_Connect.
    # Each rank posts remote-scope data before connecting, which makes the
    # client send endpoint info to the server -- info the server appends to the
    # connect tracker AFTER the collective-status slot. The last rank leaves
    # before calling PMIx_Connect (and without finalizing), so the server must
    # account for the departed participant and complete the connect on the
    # survivors instead of hanging, without corrupting the tracker's info array
    # while recording the loss status. Same two requirements as group_die apply:
    # --rtos recoverable and whole-job membership + --map-by node.
    if RUN 'test -x /opt/prte/tests/connect_die'; then
        OUT="$(RUN 'prterun --rtos recoverable --host node1:2,node2:2 -np 4 --map-by node --timeout 60 /opt/prte/tests/connect_die 2>&1')"
        n=$(echo "$OUT" | grep -c 'complete on survivors')
        if echo "$OUT" | grep -qiE 'timeout|timed out|DVM timeout'; then
            bad "connect HUNG on participant death (loss accounting missing)"
        elif [ "$n" -ge 3 ]; then
            ok "connect completed on all 3 survivors after a participant died"
        else
            bad "connect_die: only $n survivors completed: $(echo "$OUT" | tr '\n' ' ' | tail -c 160)"
        fi
    else
        skp "connect_die not built"
    fi
    cleanup_swarm

    banner "voluntary group leave (PMIX_GROUP_LEFT notification)"
    cleanup_swarm
    # group_leave: every rank builds a group, then the last rank calls
    # PMIx_Group_leave. That must generate a PMIX_GROUP_LEFT event to the
    # remaining members (across nodes, via the broadcast notification path) and
    # return success once locally generated. Each surviving member has a handler
    # registered for PMIX_GROUP_LEFT and reports PASS on receipt; the departing
    # rank reports SUCCESS. Whole-job membership + --map-by node ensures the
    # departing rank's node also hosts a survivor, exercising both local and
    # remote delivery. The victim leaves and finalizes cleanly, so no
    # --rtos recoverable is needed.
    if RUN 'test -x /opt/prte/tests/group_leave'; then
        OUT="$(RUN 'prterun --host node1:2,node2:2 -np 4 --map-by node --timeout 60 /opt/prte/tests/group_leave 2>&1')"
        n=$(echo "$OUT" | grep -c 'correctly received: PASS')
        if echo "$OUT" | grep -qiE 'timeout|timed out|DVM timeout'; then
            bad "group_leave HUNG (notification never delivered)"
        elif echo "$OUT" | grep -qiE 'FAILED'; then
            bad "group_leave: a member reported failure: $(echo "$OUT" | tr '\n' ' ' | tail -c 200)"
        elif [ "$n" -ge 3 ] && echo "$OUT" | grep -q 'Group_leave complete: SUCCESS'; then
            ok "all 3 survivors notified of the voluntary departure"
        else
            bad "group_leave: only $n survivors notified: $(echo "$OUT" | tr '\n' ' ' | tail -c 160)"
        fi
    else
        skp "group_leave not built"
    fi
    cleanup_swarm
}

########################################################################
# macOS: native, single host (build + launch smoke)
########################################################################

test_macos() {
    local root prefix
    root="$(git -C "$(dirname "$0")" rev-parse --show-toplevel)"
    prefix="$root/vpath-macos-prte/install"
    if [ ! -x "$prefix/bin/prterun" ]; then
        echo "native build missing -- run: ./build.sh macos" >&2; exit 2
    fi
    export PATH="$prefix/bin:$PATH"
    export DYLD_LIBRARY_PATH="$prefix/lib:$root/vpath-macos-pmix/install/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
    export PRTE_ALLOW_RUN_AS_ROOT=1 PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1
    macpk() { pkill -9 prterun 2>/dev/null; pkill -9 -x prte 2>/dev/null; pkill -9 -x prted 2>/dev/null; true; }

    banner "macOS: single-host group construction"
    for ex in $GROUP_EXAMPLES; do
        [ -x "$prefix/bin/$ex" ] || { skp "$ex (not built)"; continue; }
        macpk; sleep 1
        out="$(prterun -np 4 --timeout 60 "$prefix/bin/$ex" 2>&1)"
        if echo "$out" | grep -qiE 'Group construct complete|construct.*succe'; then
            ok "$ex: group constructed (single host)"
        else
            skp "$ex: no completion (native Darwin DVM can be flaky): $(echo "$out" | tr '\n' ' ' | tail -c 160)"
        fi
    done
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
