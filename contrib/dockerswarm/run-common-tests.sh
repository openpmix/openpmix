#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Exercise the src/common role-shared APIs against real, *separate* PMIx
# servers.
#
#   ./run-common-tests.sh linux    # in the 10-container swarm
#                                  #   (requires: ./build.sh && docker compose up -d)
#   ./run-common-tests.sh macos    # single-host subset natively
#                                  #   (requires: ./build.sh macos)
#
# WHY THIS IS A MULTI-NODE TEST
#
# src/common holds the public entry points that are shared by the client,
# server and tool roles -- query, log, job control, allocation, monitoring,
# session control, credentials, IOF.  Almost every one of them is the same
# shape: decide whether the request can be answered here, and if not, pack
# it and round-trip it to a server (or hand it up to the host).
# test/unit/common_api.c covers the first half -- what the library resolves
# or rejects on its own -- because a singleton reaches nothing else: with no
# server, PMIx_Query_info answers only the two ABI-version keys,
# PMIx_Log falls back to the local plog, PMIx_Job_control and
# PMIx_Allocation_request stop at the "am I connected?" check, and
# PMIx_Process_monitor never gets as far as its local/remote split.
#
# This runner is the second half, and putting the ranks on different nodes
# is what makes it real rather than loopback:
#
#   * PMIx_Query_info for keys the local library does not hold takes the
#     full pack -> PMIX_PTL_SEND_RECV -> query_cbfunc -> results-list path,
#     including the "partially resolved locally, ask for the rest" split
#     that pmix_parse_localquery implements.
#   * PMIx_Log routes through the server's plog rather than the client's,
#     which is the only way the PMIX_ERR_NOT_AVAILABLE fallback in
#     log_cbfunc is ever taken.
#   * PMIx_Job_control and PMIx_Allocation_request reach a host that
#     actually implements them, so the caddy's completion path -- the one
#     that used to hand the caller's own directives back as the results --
#     runs against a real answer.
#   * The monitor examples are the point of spreading out at all:
#     pmix_monitor_processing resolves the requested targets into "local",
#     "remote", or both, and merges pstat results with the host's. On one
#     node every target is local and the remote and mixed branches -- where
#     the host upcall and the result merge live -- are never entered.
#     monitor_remote and monitor_multi exist for exactly that.
#
# Prints PASS/FAIL per case and a summary; exits non-zero if anything failed.

set -uo pipefail

mode="${1:-linux}"
pass=0 fail=0 skip=0
ok()   { pass=$((pass+1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
skp()  { skip=$((skip+1)); printf '  \033[33mSKIP\033[0m %s\n' "$1"; }
banner() { printf '\n=== %s ===\n' "$1"; }

# RUN/ON, cleanup_swarm, swarm_up_or_die and the swarm naming (PMIX_SWARM,
# NODE, IMAGE, VOLUME) all live in swarm-common.sh.
. "$(dirname "$0")/swarm-common.sh"

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"

# The examples this runner drives.  Each is an ordinary PMIx client that
# calls one family of src/common entry points, so each one is a live test of
# the shared layer rather than of the example.
COMMON_EXAMPLES="${COMMON_EXAMPLES:-log jctrl alloc monitor monitor_multi monitor_remote}"

# See the note in run-client-tests.sh: the PMIx these clients link against
# must be the one build.sh installed into the shared volume, because they are
# COMPILED in a throwaway builder container and RUN in the long-lived node
# containers, and only the volume is the same in both.
PMIX_PREFIX=/opt/prte/pmix

OUT=""
WANT=""
EXTRA=""
RC=0

# Per-example launch geometry and the marker that says the client got all
# the way through.  Ranks are always split across at least two nodes so that
# "the answer is behind a different server" is the normal case rather than a
# special one.
#
#   * monitor_remote and monitor_multi name targets that are deliberately
#     NOT all on the requesting rank's node, which is the whole point: it is
#     the only way pmix_monitor_processing's remote and mixed branches run.
#   * alloc and jctrl need a host that implements allocate/job_control;
#     prte does, and simptest does not, which is why these cannot be
#     test/simple cases.
cm_geom() {
    HOSTS="node1:2,node2:2"; NP=4
    WANT='PMIx_Finalize successfully completed'
    EXTRA=""
    case "$1" in
        monitor_remote|monitor_multi)
            # spread wider so "remote" means something: with ranks on three
            # nodes a per-node or per-proc target list is guaranteed to
            # straddle at least two servers
            HOSTS="node1:2,node2:2,node3:2"; NP=6 ;;
        alloc)
            # the allocation request goes up to the scheduler, so keep the
            # job small and leave the rest of the allocation free
            HOSTS="node1:2,node2:2"; NP=2 ;;
    esac
}

# Launch a client through a transient DVM with the geometry cm_geom picks.
# --timeout bounds every launch so a genuine hang -- which is what most of
# the defects this suite guards against actually look like -- surfaces as a
# failure rather than stalling the suite.
launch() {
    local prog="$1"; shift
    cm_geom "$prog"
    OUT="$(RUN "prterun --host $HOSTS -np $NP --map-by node $EXTRA --timeout 60 /opt/prte/tests-common/$prog $* 2>&1")"
    RC=$?
}

# Judge one launch.  A hang is always a failure; otherwise require the
# completion marker and the absence of a crash.
#
# Deliberately NOT "any line mentioning PMIX_ERR_*": several of these
# examples ask for things the swarm has no provider for -- alloc asks the
# scheduler for resources prte will decline, and the monitors ask for
# per-process statistics that pstat cannot always produce -- and the correct
# answer there is an error the example prints and carries on from.  What
# must never happen is a crash, a lost connection, or a client that never
# reaches PMIx_Finalize.
judge() {
    local name="$1" what="$2"
    if echo "$OUT" | grep -qiE 'time limit for job|timed out|DVM timeout'; then
        bad "$name: HUNG (hit the launch timeout)"
        return
    fi
    if [ -n "$WANT" ] && ! echo "$OUT" | grep -qiE "$WANT"; then
        bad "$name: no completion (output: $(echo "$OUT" | tr '\n' ' ' | tail -c 200))"
        return
    fi
    if [ "$RC" != 0 ]; then
        bad "$name: exit $RC (output: $(echo "$OUT" | tr '\n' ' ' | tail -c 200))"
        return
    fi
    if echo "$OUT" | grep -qiE 'Segmentation|core dumped|connection refused|: ERROR!'; then
        bad "$name: $(echo "$OUT" | grep -iE 'Segmentation|core dumped|connection refused|: ERROR!' | head -1)"
        return
    fi
    ok "$name: $what"
}

########################################################################
# Linux: the 10-node swarm.
########################################################################

test_linux() {
    local ex rc

    swarm_up_or_die

    banner "preflight: install present in shared volume"
    if RUN 'command -v prterun prte pterm >/dev/null'; then
        ok "prterun/prte/pterm on PATH"
    else
        bad "tools missing -- did ./build.sh run?"; return
    fi

    if ! docker volume inspect "$VOLUME" >/dev/null 2>&1; then
        bad "build volume $VOLUME missing -- run ./build.sh first"
        return
    fi

    # Same check the other runners make, for the same reason: the nodes are
    # long-lived containers while the install they read is replaced under
    # them, so a node that predates the current image runs a different PMIx
    # than the builder container compiles against.
    banner "preflight: containers are running the current image"
    local imgid cimg imgn stalenodes=""
    imgid=$(docker images --no-trunc --format '{{.ID}}' "$IMAGE" 2>/dev/null | head -1)
    for imgn in $(seq 1 10); do
        cimg=$(docker inspect "$NODE$imgn" --format '{{.Image}}' 2>/dev/null)
        [ "$cimg" = "$imgid" ] || stalenodes="$stalenodes node$imgn"
    done
    if [ -z "$stalenodes" ]; then
        ok "all 10 containers are on the current $IMAGE"
    else
        bad "containers predate $IMAGE:$stalenodes"
        echo "     Recreate them: ${SWARM_ENV}docker compose up -d --force-recreate" >&2
        return
    fi

    banner "preflight: PMIx installed in the shared volume"
    if ON 1 "test -f $PMIX_PREFIX/include/pmix_common.h"; then
        ok "PMIx headers/libs under $PMIX_PREFIX"
    else
        bad "no $PMIX_PREFIX in the volume -- run ./build.sh first"
        return
    fi

    banner "build the common-API examples"
    docker run --rm \
        -v "$root":/pmix-src:ro \
        -v "$VOLUME":/opt/prte \
        -e EXAMPLES="$COMMON_EXAMPLES" \
        -e PFX="$PMIX_PREFIX" \
        "$IMAGE" bash -euo pipefail -c '
            mkdir -p /opt/prte/tests-common
            for ex in $EXAMPLES; do
                gcc -O0 -g -o "/opt/prte/tests-common/$ex" "/pmix-src/examples/$ex.c" \
                    -I"$PFX/include" -I/pmix-src/examples \
                    -L"$PFX/lib" -lpmix -Wl,-rpath,"$PFX/lib"
            done
        '
    rc=$?
    if [ "$rc" != 0 ]; then
        bad "could not build the common-API examples (rc=$rc)"
        return
    fi
    ok "built: $COMMON_EXAMPLES"

    banner "role-shared APIs across separate PMIx servers"
    for ex in $COMMON_EXAMPLES; do
        cleanup_swarm
        if ! RUN "test -x /opt/prte/tests-common/$ex"; then
            skp "$ex (not built)"; continue
        fi
        launch "$ex"
        case "$ex" in
            log)      judge "$ex" "PMIx_Log relayed through the local server" ;;
            jctrl)    judge "$ex" "PMIx_Job_control handed to the host and answered" ;;
            alloc)    judge "$ex" "PMIx_Allocation_request forwarded to the scheduler" ;;
            monitor)  judge "$ex" "heartbeat monitoring registered and cancelled" ;;
            monitor_multi)
                      judge "$ex" "monitor over several targets on several nodes" ;;
            monitor_remote)
                      judge "$ex" "monitor whose targets are behind other servers" ;;
            *)        judge "$ex" "completed" ;;
        esac
        [ "$(prted_count 1 2 3 4 5 6 7 8 9 10)" = 0 ] \
            || bad "$ex: stray prted left behind"
    done

    # IOF is the other half of src/common that a singleton cannot reach: the
    # forwarding path only exists once there is a server holding the read
    # end of somebody else's stdout.  Driving a client with output-to-file
    # makes pmix_iof_setup name and open the per-rank sinks, which is where
    # the pattern expansion and the residual/line-splitting logic run.
    #
    # The PATTERN qualifier is what selects "the name is mine to compose"
    # over the default "annotate the stem with nspace and rank". WITHOUT it
    # a name containing '%' is not an error and not expanded -- it is just a
    # stem with odd characters in it, and the files come out as
    # "%h-rank%R.<nspace>.<rank>.err". That is the correct behavior, so the
    # qualifier has to be here or this case silently tests nothing. Note it
    # is a QUALIFIER on the file directive ("file=NAME:pattern"), not a
    # directive of its own -- comma-separating it is rejected outright.
    #
    # %h expands to the host of the process WRITING the file, which for a
    # forwarded stream is the daemon that received it rather than the one
    # the rank ran on. That is what src/common/pmix_iof.h documents, and it
    # is why this only asserts that the conversions were expanded at all,
    # not which host they named.
    #
    # The pattern deliberately puts a conversion in the DIRECTORY part
    # (%h/), because taking the dirname of the raw pattern would create a
    # directory literally called "%h" and then fail to open the file in the
    # one the pattern actually named.
    banner "IOF output-to-file with a pattern, across nodes"
    cleanup_swarm
    if RUN 'test -x /opt/prte/tests-common/log'; then
        OUT="$(RUN 'rm -rf /tmp/iofpat && mkdir -p /tmp/iofpat && prterun --host node1:2,node2:2 -np 4 --map-by node --timeout 60 --output "file=/tmp/iofpat/%h/rank%R:pattern" /opt/prte/tests-common/log 2>&1')"
        RC=$?
        if echo "$OUT" | grep -qiE 'time limit for job|timed out'; then
            bad "iof-pattern: HUNG"
        elif [ "$RC" != 0 ]; then
            bad "iof-pattern: exit $RC"
        elif RUN 'ls /tmp/iofpat/*/rank*.err >/dev/null 2>&1'; then
            # nothing may be left holding a '%': an unexpanded conversion
            # anywhere in the name means the walker did not run
            if RUN 'ls /tmp/iofpat/* | grep -q "%"'; then
                bad "iof-pattern: conversions left unexpanded ($(RUN 'ls -1 /tmp/iofpat/* 2>&1' | tr '\n' ' '))"
            else
                ok "iof-pattern: per-node directory and per-rank files named by the pattern"
            fi
        else
            bad "iof-pattern: no expanded output files ($(RUN 'ls -1R /tmp/iofpat 2>&1' | tr '\n' ' '))"
        fi
    else
        skp "iof-pattern (log not built)"
    fi
    cleanup_swarm
}

########################################################################
# macOS: natively on this host.  One PMIx server, so the local/remote
# split in the monitor collapses to "all local" -- but the connected
# query/log/job-control paths a singleton cannot reach are still real.
########################################################################

test_macos() {
    local ex prefix="$root/../build/master"

    [ -d "$prefix" ] || prefix="/opt/prte/pmix"

    banner "native single-host common-API pass"
    if [ ! -x "$prefix/bin/prterun" ] && ! command -v prterun >/dev/null; then
        skp "no prterun available -- run ./build.sh macos first"
        return
    fi

    mac_isolate "$prefix" 2>/dev/null || true

    for ex in $COMMON_EXAMPLES; do
        local bin="/tmp/pmix-common-$ex.$$"
        if ! cc -O0 -g -o "$bin" "$root/examples/$ex.c" \
                -I"$root/include" -I"$root" \
                -L"$root/src/.libs" -lpmix -Wl,-rpath,"$root/src/.libs" \
                >/dev/null 2>&1; then
            skp "$ex (would not compile against the in-tree library)"
            continue
        fi
        OUT="$(prterun -np 4 --timeout 60 "$bin" 2>&1)"; RC=$?
        WANT='PMIx_Finalize successfully completed'
        judge "$ex" "completed against a single local server"
        rm -f "$bin"
    done

    mac_done 2>/dev/null || true
}

case "$mode" in
    linux) test_linux ;;
    macos) test_macos ;;
    *) echo "usage: $0 [linux|macos]" >&2; exit 2 ;;
esac

banner "summary"
printf '  %d passed, %d failed, %d skipped\n\n' "$pass" "$fail" "$skip"
[ "$fail" = 0 ]
