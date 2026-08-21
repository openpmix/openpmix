#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Exercise src/server -- the server-role half of libpmix -- across real,
# *separate* PMIx servers.
#
#   ./run-server-tests.sh linux    # in the 10-container swarm
#                                  #   (requires: ./build.sh && docker compose up -d)
#   ./run-server-tests.sh macos    # single-host subset natively
#                                  #   (requires: ./build.sh macos)
#
# WHY THIS IS A MULTI-NODE TEST
#
# The PMIx server library is what a launcher daemon (here prted) runs.  Its
# central decision -- taken in pmix_server_get() -- is whether a request can
# be answered out of what this server already holds, or has to be deferred
# and fetched from whichever server hosts the target process.  On one node
# that decision has one arm: every rank is local, every key is already in
# the local datastore, and the entire remote half of the file never runs.
# That half is not a corner: it is the direct-modex engine (local_reqs,
# remote_pnd, dmdx_cbfunc -> _process_dmdx_reply -> pmix_pending_resolve),
# and it is where the deferred-request lifetime bugs live.
#
# So every stage here puts ONE rank per node.  That is not a load-spreading
# preference -- with two ranks on a node, a rank asking for its neighbour's
# data is answered out of local storage and the request never leaves the
# server.  A run with two ranks per node can be green with the whole remote
# path broken.  Keep the geometry.
#
# What the stages reach that `make check` cannot:
#
#   * dmodex / modex_twice / group_dmodex -- a rank fetches a peer's data
#     from a server that is not its own.  This is the only way
#     pmix_server_get()'s local-vs-remote classification, the deferral onto
#     pmix_server_globals.local_reqs, the host direct_modex up-call, and the
#     reply ingest are exercised at all.  modex_twice repeats the fetch so
#     the second request finds an existing tracker rather than creating one.
#   * dynamic -- spawn plus connect, so a rank gets a key from a process in
#     a DIFFERENT namespace.  That is the diffnspace arm of
#     _satisfy_request(), which packs job-level data before the per-rank
#     data and is the arm whose not-found path used to strand the packed
#     buffer.
#   * multi_nspace_group -- a group whose members span namespaces and
#     nodes, which drives the two-level group block/tracker engine through
#     a real host rather than through simptest.
#   * resolve / simple_resolve -- pmix_server_resolve_peers() and
#     pmix_server_resolve_node() against a host (prte) that answers the
#     query itself.  NOTE what this does and does not cover: because prte
#     implements the query interface, these take the host arm and the
#     server's own local-datastore fallback
#     (pmix_server_locally_resolve_peers/_node) is NOT reached.  The
#     fallback is what runs under a host without query support; it has no
#     coverage here and saying so is more useful than implying otherwise.
#   * pub -- publish/lookup/unpublish, the setup-caddy family whose whole
#     job is to survive a host up-call and answer exactly once.
#   * IOF pull against a persistent DVM -- prterun attaches to the DVM as a
#     TOOL and calls PMIx_IOF_pull, which is the only thing that drives
#     pmix_server_iofreg()/pmix_server_iofdereg() on the daemon.  A
#     transient DVM does not: it is torn down with the job.  Running two
#     jobs in a row against one DVM means the second registers after the
#     first has deregistered, which is the sequence that fails if the
#     registration caddy is released while the host still owns it.
#   * a valgrind stage on the daemon itself.  Every other suite valgrinds a
#     client; the server library only ever runs inside prted, so leaks in
#     it are invisible to all of them.
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

# The examples this runner drives.  Each one puts the server library on a
# path a single-node run cannot take; see the header.
SERVER_EXAMPLES="${SERVER_EXAMPLES:-dmodex dmdx_departed modex_twice group_dmodex dynamic multi_nspace_group resolve simple_resolve pub}"

# See run-client-tests.sh: these are COMPILED in a throwaway builder
# container and RUN in the long-lived node containers, so they must link the
# PMIx that build.sh installed into the shared volume -- the only tree that
# is the same in both.
PMIX_PREFIX=/opt/prte/pmix

OUT=""
WANT=""
RC=0
HOSTS=""
NP=0

# Launch geometry.  ONE rank per node, always -- see the header.  The
# default spread is four nodes; a couple of the examples want more ranks to
# make their point.
sv_geom() {
    HOSTS="node1:1,node2:1,node3:1,node4:1"; NP=4
    WANT='PMIx_Finalize successfully completed'
    case "$1" in
        dynamic|multi_nspace_group|resolve)
            # These three call PMIx_Spawn. --host IS the allocation, so a
            # parent job that fills every listed slot leaves the spawn with
            # nowhere to land and the job blocks until the launch timeout --
            # which looks exactly like a library hang and is not one. Give
            # the DVM twice the nodes the parent uses, so the child job gets
            # servers of its own: that is also what makes the subsequent
            # cross-namespace get cross a server boundary rather than being
            # answered out of the parent's own datastore.
            HOSTS="node1:1,node2:1,node3:1,node4:1"; NP=2 ;;
    esac
    case "$1" in
        # Not every example announces itself the same way. Match what each
        # one actually prints on the way out, or the case passes/fails on
        # the marker rather than on the behavior.
        modex_twice)    WANT='second fence: new values visible, old values kept' ;;
        # the point is that PMIx ANSWERS - a regression parks the get
        # forever, and judge() reports that launch timeout as a hang
        dmdx_departed)  WANT='DMDX-DEPARTED answered' ;;
        group_dmodex)   WANT='COMPLETE' ;;
        resolve)        WANT='Bye\.' ;;
        pub|simple_resolve) WANT='' ;;
    esac
}

# Launch from the staging directory, not from wherever the shell landed.
# examples/resolve.c spawns its child as the RELATIVE path "./resolve", so a
# launch from anywhere else fails on the spawned job with "could not access
# an executable" and then hangs the surviving parent rank at its fence --
# which reads as a library hang and is not one.
launch() {
    local prog="$1"; shift
    sv_geom "$prog"
    OUT="$(RUN "cd /opt/prte/tests-server && prterun --host $HOSTS -np $NP --map-by node --timeout 90 ./$prog $* 2>&1")"
    RC=$?
}

# Judge one launch.  A hang is always a failure.  Otherwise require the
# completion marker (when the example prints one) and the absence of a
# crash.  Deliberately NOT "any line mentioning PMIX_ERR_*": several of
# these examples probe for things the swarm has no provider for and print
# the error they get.  A crash, a lost connection, or a client that never
# reaches PMIx_Finalize is what must never happen.
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

    # The nodes are long-lived containers while the install they read is
    # replaced under them, so a node predating the current image runs a
    # different PMIx than the builder container compiles against.
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

    banner "build the server-path examples"
    docker run --rm \
        -v "$root":/pmix-src:ro \
        -v "$VOLUME":/opt/prte \
        -e EXAMPLES="$SERVER_EXAMPLES" \
        -e PFX="$PMIX_PREFIX" \
        "$IMAGE" bash -euo pipefail -c '
            mkdir -p /opt/prte/tests-server
            for ex in $EXAMPLES; do
                gcc -O0 -g -o "/opt/prte/tests-server/$ex" "/pmix-src/examples/$ex.c" \
                    -I"$PFX/include" -I/pmix-src/examples \
                    -L"$PFX/lib" -lpmix -Wl,-rpath,"$PFX/lib"
            done
        '
    rc=$?
    if [ "$rc" != 0 ]; then
        bad "could not build the server-path examples (rc=$rc)"
        return
    fi
    ok "built: $SERVER_EXAMPLES"

    banner "server-side request handling across separate PMIx servers"
    for ex in $SERVER_EXAMPLES; do
        cleanup_swarm
        if ! RUN "test -x /opt/prte/tests-server/$ex"; then
            skp "$ex (not built)"; continue
        fi
        launch "$ex"
        case "$ex" in
            dmodex)      judge "$ex" "direct modex: a key fetched from another server" ;;
            dmdx_departed)
                         judge "$ex" "direct modex for a terminated proc is answered, not parked" ;;
            modex_twice) judge "$ex" "repeated modex: second request joins an existing tracker" ;;
            group_dmodex) judge "$ex" "modex within a group spanning servers" ;;
            dynamic)     judge "$ex" "spawn + connect: a get across namespaces and servers" ;;
            multi_nspace_group)
                         judge "$ex" "group block/tracker engine across namespaces and nodes" ;;
            resolve)     judge "$ex" "resolve_peers/resolve_node answered by the host" ;;
            simple_resolve)
                         judge "$ex" "resolve against a host that owns the answer" ;;
            pub)         judge "$ex" "publish/lookup/unpublish round-tripped through the host" ;;
            *)           judge "$ex" "completed" ;;
        esac
        [ "$(prted_count 1 2 3 4 5 6 7 8 9 10)" = 0 ] \
            || bad "$ex: stray prted left behind"
    done

    # ------------------------------------------------------------------
    # IOF registration/deregistration on the daemon.
    #
    # prterun attaches to a PERSISTENT DVM as a tool and calls
    # PMIx_IOF_pull; that is what reaches pmix_server_iofreg() in the
    # daemon, and the tool's departure is what reaches
    # pmix_server_iofdereg().  A transient DVM never gets there, because it
    # is created and torn down with the job.
    #
    # Two jobs in a row against ONE DVM is the sequence that matters: the
    # second registration happens after the first has deregistered, so a
    # registration caddy that was freed while the host still owned it shows
    # up as a crashed or silent daemon on the second job rather than the
    # first.
    # ------------------------------------------------------------------
    banner "IOF pull/dereg against a persistent DVM"
    cleanup_swarm
    if ! RUN 'test -x /opt/prte/tests-server/dmodex'; then
        skp "iof-dvm (dmodex not built)"
    else
        OUT="$(RUN 'prte --daemonize --host node1:1,node2:1,node3:1,node4:1 >/tmp/svr-dvm.log 2>&1; sleep 5;
                    echo "--- job 1 ---";
                    prterun --host node1:1,node2:1 -np 2 --map-by node --timeout 60 /opt/prte/tests-server/dmodex 2>&1;
                    echo "--- job 2 ---";
                    prterun --host node3:1,node4:1 -np 2 --map-by node --timeout 60 /opt/prte/tests-server/dmodex 2>&1;
                    echo "--- done ---";
                    pterm >/dev/null 2>&1' )"
        RC=$?
        if ! echo "$OUT" | grep -q -- '--- done ---'; then
            bad "iof-dvm: the DVM sequence did not complete ($(echo "$OUT" | tr '\n' ' ' | tail -c 200))"
        elif echo "$OUT" | grep -qiE 'time limit for job|timed out'; then
            bad "iof-dvm: HUNG"
        elif echo "$OUT" | grep -qiE 'Segmentation|core dumped|: ERROR!'; then
            bad "iof-dvm: $(echo "$OUT" | grep -iE 'Segmentation|core dumped|: ERROR!' | head -1)"
        elif [ "$(echo "$OUT" | grep -c 'PMIx_Finalize successfully completed')" -lt 2 ]; then
            # dmodex announces its finalize from RANK 0 ONLY, so the count to
            # expect is one per job, not one per rank
            bad "iof-dvm: both jobs did not finish ($(echo "$OUT" | grep -c 'PMIx_Finalize successfully completed') of 2)"
        elif [ "$(echo "$OUT" | grep 'PMIx_Finalize successfully completed' | awk '{print $3}' | sort -u | wc -l)" -lt 2 ]; then
            # ... and they must be two DIFFERENT namespaces, or we have one
            # job's output forwarded twice rather than two jobs each forwarded
            bad "iof-dvm: both completions came from one job -- the second tool attachment forwarded nothing"
        else
            ok "iof-dvm: two tool attachments in a row, output forwarded for both"
        fi
        cleanup_swarm
    fi

    # ------------------------------------------------------------------
    # The daemon under valgrind.
    #
    # This is the only stage in the whole harness that leak-checks the
    # SERVER library.  Every other suite valgrinds a client, and a client
    # never runs src/server at all.
    #
    # The DVM's own daemons are started by prte, so the way to get valgrind
    # underneath one is to run the whole DVM under it on a single node and
    # then drive real work through it.  One node is a real limitation --
    # the remote-fetch half of pmix_server_get is not entered -- so this
    # stage is about the request handlers, the caddy accounting, and the
    # collective trackers, not about direct modex.  Do not let it stand in
    # for the multi-node stages above.
    #
    # Only "definitely lost" blocks whose stack names a src/server frame are
    # failed on.  prte itself leaks at exit in ways that are not this
    # suite's business, and holding this stage to a whole-process clean bill
    # would make it permanently red and therefore ignored.
    # ------------------------------------------------------------------
    banner "server library under valgrind"
    cleanup_swarm
    # The image does not carry valgrind (it is a build image, not a debug
    # one, and it is shared with the other swarm so rebuilding it to add a
    # package moves it under that swarm's feet). Install it into the one
    # node we need it on, at run time, and skip cleanly if there is no
    # network to do that with.
    if ! ON 1 'command -v valgrind >/dev/null 2>&1'; then
        ON 1 'apt-get update -qq >/dev/null 2>&1; apt-get install -y -qq valgrind >/dev/null 2>&1' || true
    fi
    if ! ON 1 'command -v valgrind >/dev/null 2>&1'; then
        skp "valgrind-server (valgrind unavailable and could not be installed)"
    elif ! RUN 'test -x /opt/prte/tests-server/dmodex'; then
        skp "valgrind-server (dmodex not built)"
    else
        ON 1 'rm -f /tmp/svr-vg.log'
        # RUN, not ON: prterun refuses to run as root unless
        # PRTE_ALLOW_RUN_AS_ROOT[_CONFIRM] are in the environment, and RUN is
        # the helper that sets them. ON leaves them out and the launch dies
        # with the run-as-root advisory rather than with anything to do with
        # valgrind.
        # --fullpath-after= (empty argument) makes valgrind print the FULL
        # source path in each frame instead of the bare basename. Without it
        # a PMIx server frame reads "pmix_server_get.c:123", which is both
        # unmatchable by a path pattern and ambiguous: PRRTE has its own
        # pmix_server.c, pmix_server_fence.c and pmix_server_gen.c, so a
        # basename match would attribute PRRTE's leaks to us and a path
        # match would find nothing at all. The full path separates them.
        OUT="$(RUN 'valgrind --leak-check=full --show-leak-kinds=definite \
                        --track-origins=yes --error-exitcode=0 \
                        --fullpath-after= \
                        --log-file=/tmp/svr-vg.log \
                        prterun -np 2 --timeout 90 /opt/prte/tests-server/dmodex 2>&1' )"
        RC=$?
        if [ "$RC" != 0 ]; then
            bad "valgrind-server: the run itself failed (exit $RC): $(echo "$OUT" | tr '\n' ' ' | tail -c 200)"
        else
            # a definite leak whose allocation stack passes through
            # src/server is ours; anything else is not this stage's subject
            # --show-leak-kinds=definite means the log holds definite-leak
            # records and the summary, nothing else, so a src/server frame
            # anywhere in it is a frame in a definite leak's allocation stack
            local srvleaks
            srvleaks="$(ON 1 "grep -c 'src/server/pmix_server' /tmp/svr-vg.log" 2>/dev/null | tr -dc '0-9')"
            srvleaks="${srvleaks:-0}"
            if [ "$srvleaks" = 0 ]; then
                ok "valgrind-server: no definite leak attributed to a src/server frame"
            else
                bad "valgrind-server: $srvleaks definite-leak frames in src/server"
                ON 1 'grep -B2 -A12 "definitely lost" /tmp/svr-vg.log | head -60' >&2
            fi
        fi
        cleanup_swarm
    fi
}

########################################################################
# macOS: natively on this host.  One PMIx server, so the remote half of
# pmix_server_get is not reached at all -- but the request handlers, the
# collective trackers and the caddy accounting are still driven by a real
# host, which test/unit cannot do either.
########################################################################

test_macos() {
    local ex prefix="$root/../build/master"

    [ -d "$prefix" ] || prefix="/opt/prte/pmix"

    banner "native single-host server pass"
    echo "  NOTE: one node means one server -- the direct-modex half of"
    echo "        src/server is NOT covered here. Use the linux mode for that."
    if [ ! -x "$prefix/bin/prterun" ] && ! command -v prterun >/dev/null; then
        skp "no prterun available -- run ./build.sh macos first"
        return
    fi

    mac_isolate "$prefix" 2>/dev/null || true

    for ex in $SERVER_EXAMPLES; do
        local bin="/tmp/pmix-server-$ex.$$"
        if ! cc -O0 -g -o "$bin" "$root/examples/$ex.c" \
                -I"$root/include" -I"$root" \
                -L"$root/src/.libs" -lpmix -Wl,-rpath,"$root/src/.libs" \
                >/dev/null 2>&1; then
            skp "$ex (would not compile against the in-tree library)"
            continue
        fi
        sv_geom "$ex"
        OUT="$(prterun -np "$NP" --timeout 90 "$bin" 2>&1)"; RC=$?
        judge "$ex" "completed against a single local server"
        rm -f "$bin"
    done
}

########################################################################

case "$mode" in
    linux) test_linux ;;
    macos) test_macos ;;
    *) echo "usage: $0 [linux|macos]" >&2; exit 2 ;;
esac

banner "summary"
printf '  passed %d, failed %d, skipped %d\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ]
