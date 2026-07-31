#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Exercise the src/client APIs against real, *separate* PMIx servers.
#
#   ./run-client-tests.sh linux    # in the 10-container swarm
#                                  #   (requires: ./build.sh && docker compose up -d)
#   ./run-client-tests.sh macos    # single-host subset natively
#                                  #   (requires: ./build.sh macos)
#
# WHY THIS ONE *IS* A MULTI-NODE TEST
#
# Unlike run-class-tests.sh, whose subject is single-process data structures,
# essentially every interesting path in src/client is "the request could not
# be answered locally, so pack a command and round-trip to the server".  A
# singleton exercises none of it: PMIx_Fence and PMIx_Commit short-circuit,
# PMIx_Get never leaves the local datastore, and publish/lookup/spawn/connect
# all bail out at the "am I connected?" check.  test/unit/client_api.c covers
# what a client can decide by itself; this covers what it cannot.
#
# Spreading the ranks over several nodes matters specifically because each
# node runs its own prted, hence its own PMIx server.  That is what makes
# these paths real rather than loopback:
#
#   * PMIx_Get for a peer on another node cannot be satisfied from the data
#     the local server pushed down at init, so it takes the full
#     pack -> PMIX_PTL_SEND_RECV -> _getnb_cbfunc -> GDS-store -> deliver
#     path, including the pending-request coalescing that makes several
#     outstanding gets for one proc share a single request.
#   * PMIx_Fence with collect-data has to modex across daemons.
#   * publish/lookup resolves through the daemons up to a common ancestor.
#   * PMIx_Resolve_peers / PMIx_Resolve_nodes have something to resolve that
#     is not just "this node".
#   * PMIx_Spawn plus PMIx_Connect/PMIx_Disconnect exercise the multi-nspace
#     job-info exchange in PMIx_Connect_nb.
#
# The realm-directed gets (node/app/session info) are called out separately
# because that parser is the densest code in the directory and its answers
# differ per node -- so getting them right is only observable when the ranks
# are actually on different nodes.
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

# The examples this runner drives.  Each is an ordinary PMIx client, so each
# one is a live test of the client library rather than of the example.
CLIENT_EXAMPLES="${CLIENT_EXAMPLES:-client dmodex nodeinfo pub resolve dynamic}"

# Per-example launch geometry and the marker that says the client got all the
# way through.  Ranks are always split across at least two nodes so the data
# a client asks for lives behind a different server than its own.
#
#   * dmodex has rank 0 sleep before committing, so the other ranks' gets
#     block in the client's pending-request list until the reply arrives --
#     the coalescing path.  It needs at least two ranks.
#   * dynamic has rank 0 PMIx_Spawn a child job and then connect to it, so
#     the allocation must leave slots free for the child.
#   * every example ends by reporting a successful PMIx_Finalize, which is
#     also the marker that nothing hung on the way there.
cl_geom() {
    HOSTS="node1:2,node2:2"; NP=4
    WANT='PMIx_Finalize successfully completed'
    case "$1" in
        dmodex)
            HOSTS="node1:2,node2:2"; NP=4 ;;
        dynamic)
            HOSTS="node1:4,node2:4"; NP=2 ;;
        resolve)
            HOSTS="node1:2,node2:2,node3:2"; NP=6
            # resolve.c reports through its exit code rather than a banner
            WANT='' ;;
    esac
}

# The PMIx these clients link against must be the one build.sh installed into
# the shared volume (/opt/prte/pmix), not one baked into an image: the clients
# are COMPILED in a throwaway builder container and RUN in the long-lived node
# containers, and only the volume is guaranteed to be the same in both.  An
# image-local /usr/local install satisfies the compile and then fails at run
# time -- or, on a swarm whose nodes predate the current image, the reverse.
# Insisting on the volume keeps that whole class of confusion out.
PMIX_PREFIX=/opt/prte/pmix

OUT=""
WANT=""
RC=0
# Launch a client through a transient DVM with the geometry cl_geom picks.
# The program is named by absolute path because prterun ships the resolved
# path to the other daemons, whose non-login launch environment does not have
# /opt/prte/tests-client on PATH.  --timeout bounds every launch so a genuine
# hang surfaces as a failure rather than stalling the suite.
launch() {
    local prog="$1"; shift
    cl_geom "$prog"
    OUT="$(RUN "prterun --host $HOSTS -np $NP --map-by node --timeout 60 /opt/prte/tests-client/$prog $* 2>&1")"
    RC=$?
}

# Judge one launch: a hang is always a failure; otherwise require the marker
# (when the example has one) and the absence of an error report.
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
    if echo "$OUT" | grep -qiE 'PMIX_ERROR|PMIX_ERR_|connection refused|Segmentation'; then
        bad "$name: completed but reported an error: $(echo "$OUT" | grep -iE 'PMIX_ERROR|PMIX_ERR_|Segmentation' | head -1)"
        return
    fi
    ok "$name: $what"
}

########################################################################
# Linux: the 10-node swarm.  Build the clients in a builder container
# against the PMIx build.sh installed, then drive them with prterun.
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

    # Same check run-tests.sh makes, and for the same reason: the nodes are
    # long-lived containers while the install they read is replaced under
    # them, so a node that predates the current image runs a different PMIx
    # than the builder container compiles against.  The symptom is a missing
    # header or an undefined symbol, and nothing in either message mentions
    # containers.
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
        echo "     (from contrib/dockerswarm, so the pinned project name applies)" >&2
        return
    fi

    banner "preflight: PMIx installed in the shared volume"
    if ON 1 "test -f $PMIX_PREFIX/include/pmix_common.h"; then
        ok "PMIx headers/libs under $PMIX_PREFIX"
    else
        bad "no $PMIX_PREFIX in the volume -- run ./build.sh first"
        echo "     These clients must link the PMIx you are reviewing, which is the" >&2
        echo "     one build.sh installs into the shared volume. A PMIx baked into" >&2
        echo "     the image is not it." >&2
        return
    fi

    banner "build the client examples"
    # Compiled here rather than relying on build.sh's list, so this runner
    # stays self-contained: adding an example to CLIENT_EXAMPLES is enough.
    # -I/pmix-src/examples is needed for examples.h, which every example
    # includes and which is not part of the install.
    docker run --rm \
        -v "$root":/pmix-src:ro \
        -v "$VOLUME":/opt/prte \
        -e EXAMPLES="$CLIENT_EXAMPLES" \
        -e PFX="$PMIX_PREFIX" \
        "$IMAGE" bash -euo pipefail -c '
            mkdir -p /opt/prte/tests-client
            for ex in $EXAMPLES; do
                gcc -O0 -g -o "/opt/prte/tests-client/$ex" "/pmix-src/examples/$ex.c" \
                    -I"$PFX/include" -I/pmix-src/examples \
                    -L"$PFX/lib" -lpmix -Wl,-rpath,"$PFX/lib"
            done
        '
    rc=$?
    if [ "$rc" != 0 ]; then
        bad "could not build the client examples (rc=$rc)"
        return
    fi
    ok "built: $CLIENT_EXAMPLES"

    banner "client APIs across separate PMIx servers"
    for ex in $CLIENT_EXAMPLES; do
        cleanup_swarm
        if ! RUN "test -x /opt/prte/tests-client/$ex"; then
            skp "$ex (not built)"; continue
        fi
        launch "$ex"
        case "$ex" in
            client)   judge "$ex" "put/commit/fence/get across two servers" ;;
            dmodex)   judge "$ex" "direct modex get with requests pending" ;;
            nodeinfo) judge "$ex" "node/app/session realm gets per node" ;;
            pub)      judge "$ex" "publish/lookup/unpublish across servers" ;;
            resolve)  judge "$ex" "resolve peers/nodes across three nodes" ;;
            dynamic)  judge "$ex" "spawn + connect/disconnect across nspaces" ;;
            *)        judge "$ex" "completed" ;;
        esac
        [ "$(prted_count 1 2 3 4 5 6 7 8 9 10)" = 0 ] \
            || bad "$ex: stray prted left behind"
    done

    # A get whose answer is on another node, issued by many ranks at once, is
    # the case the pending-request coalescing exists for: only the first
    # requester sends, the rest are satisfied from the one reply.  Widening
    # dmodex over more nodes makes that list actually get deep.
    banner "wide direct-modex (pending-request coalescing under load)"
    cleanup_swarm
    if RUN 'test -x /opt/prte/tests-client/dmodex'; then
        OUT="$(RUN 'prterun --host node1:2,node2:2,node3:2,node4:2,node5:2 -np 10 --map-by node --timeout 90 /opt/prte/tests-client/dmodex 2>&1')"
        RC=$?
        WANT='PMIx_Finalize successfully completed'
        judge "dmodex x10" "10 ranks over 5 servers completed their gets"
    else
        skp "dmodex not built"
    fi
    cleanup_swarm
}

########################################################################
# macOS: natively on this host.  One PMIx server, so this is a
# smoke/regression pass rather than the multi-server case above -- but it
# still drives the connected client paths a singleton cannot reach.
########################################################################

test_macos() {
    local ex prefix="$root/../build/master" jobs

    jobs="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    [ -d "$prefix" ] || prefix="/opt/prte/pmix"

    banner "native single-host client pass"
    if [ ! -x "$prefix/bin/prterun" ] && ! command -v prterun >/dev/null; then
        skp "no prterun available -- run ./build.sh macos first"
        return
    fi

    mac_isolate "$prefix" 2>/dev/null || true

    for ex in $CLIENT_EXAMPLES; do
        local bin="/tmp/pmix-client-$ex.$$"
        if ! cc -O0 -g -o "$bin" "$root/examples/$ex.c" \
                -I"$root/include" -I"$root" \
                -L"$root/src/.libs" -lpmix -Wl,-rpath,"$root/src/.libs" \
                >/dev/null 2>&1; then
            skp "$ex (would not compile against the in-tree library)"
            continue
        fi
        OUT="$(prterun -np 4 --timeout 60 "$bin" 2>&1)"; RC=$?
        WANT='PMIx_Finalize successfully completed'
        [ "$ex" = resolve ] && WANT=''
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
