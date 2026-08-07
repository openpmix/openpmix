#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Exercise src/mca/ptl -- the PMIx transport layer -- over real sockets
# between real hosts.
#
#   ./run-ptl-tests.sh linux    # in the 10-container swarm
#                               #   (requires: ./build.sh && docker compose up -d)
#   ./run-ptl-tests.sh macos    # single-host subset natively
#                               #   (requires: ./build.sh macos)
#
# WHY THIS IS A MULTI-NODE TEST
#
# Almost everything a developer's own "make check" does to the PTL happens
# over loopback, on one host, between processes that were started by the
# same server.  That reaches the framing code but not much else, and the
# parts it misses are exactly the parts that are hard to reason about:
#
#   * INTERFACE SELECTION.  pmix_ptl_base_setup_listener defaults to a
#     loopback device and only falls back to a public one when remote or
#     tool connections were asked for.  On one host the loopback branch is
#     always taken and the public branch -- along with every "no
#     interfaces" diagnostic guarding it -- is dead code.  A tool on
#     another node can only reach a server that took the other branch.
#
#   * DISCOVERY IS NODE-LOCAL.  The rendezvous files a server drops
#     (pmix.<host>.tool, pmix.<host>.tool.<pid>, pmix.<host>.tool.<nspace>)
#     live in that node's tmpdir and name that node's host.  On one host
#     "search the tmpdir" always succeeds, so nothing ever exercises what
#     happens when it does not -- which used to be a NULL dereference on
#     the optional-rendezvous-file path.
#
#   * PARTIAL WRITES.  send_msg tracks a cursor across writev() because a
#     socket buffer can fill mid-message; read_bytes resumes a payload
#     across EAGAIN for the same reason.  A loopback socket between two
#     processes on an idle machine rarely makes either happen.  A real TCP
#     connection carrying a modex between containers does.
#
#   * THE MESSAGE-SIZE CEILING.  pmix_ptl_base.max_msg_size is checked
#     against every inbound header before the payload is allocated.  The
#     "0 means no practical limit" case used to leave that ceiling at zero,
#     which rejects every message carrying a payload -- invisible until
#     someone sets the parameter, and then total.
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

# client  - an ordinary PMIx client: connect, put/get, fence, finalize.
#           The vehicle for every "does traffic cross the wire" case.
# dmodex  - forces a direct modex, i.e. a real request/reply round trip to
#           a peer behind a different server, with a payload attached.
# tool    - PMIx_tool_init with -u <uri> or -nspace <nspace>, which is the
#           only way to drive the tool half of connect_to_peer.
PTL_EXAMPLES="${PTL_EXAMPLES:-client dmodex tool}"

# See the note in run-client-tests.sh: the PMIx these link against must be
# the one build.sh installed into the shared volume, because they are
# COMPILED in a throwaway builder container and RUN in the long-lived node
# containers, and only the volume is the same in both.
PMIX_PREFIX=/opt/prte/pmix
TESTDIR=/opt/prte/tests-ptl

OUT=""
RC=0
WANT='PMIx_Finalize successfully completed'

# Judge one launch.  A hang is always a failure; otherwise require the
# completion marker and the absence of a crash.
judge() {
    local name="$1" what="$2"
    if echo "$OUT" | grep -qiE 'time limit for job|timed out|DVM timeout'; then
        bad "$name: HUNG (hit the launch timeout)"
        return 1
    fi
    if [ -n "$WANT" ] && ! echo "$OUT" | grep -qiE "$WANT"; then
        bad "$name: no completion (output: $(echo "$OUT" | tr '\n' ' ' | tail -c 240))"
        return 1
    fi
    if [ "$RC" != 0 ]; then
        bad "$name: exit $RC (output: $(echo "$OUT" | tr '\n' ' ' | tail -c 240))"
        return 1
    fi
    if echo "$OUT" | grep -qiE 'Segmentation|core dumped|connection refused|: ERROR!'; then
        bad "$name: $(echo "$OUT" | grep -iE 'Segmentation|core dumped|connection refused|: ERROR!' | head -1)"
        return 1
    fi
    ok "$name: $what"
    return 0
}

########################################################################
# Linux: the 10-node swarm.
########################################################################

test_linux() {
    local rc ex imgid cimg imgn stalenodes="" uri host addr nsp

    swarm_up_or_die

    banner "preflight: install present in shared volume"
    if RUN 'command -v prterun prte pterm >/dev/null'; then
        ok "prterun/prte/pterm on PATH"
    else
        bad "tools missing -- did ./build.sh run?"; return
    fi

    if ! docker volume inspect "$VOLUME" >/dev/null 2>&1; then
        bad "build volume $VOLUME missing -- run ./build.sh first"; return
    fi

    # Same check the other runners make, for the same reason: the nodes are
    # long-lived containers while the install they read is replaced under
    # them, so a node that predates the current image runs a different PMIx
    # than the builder container compiles against.
    banner "preflight: containers are running the current image"
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
        bad "no $PMIX_PREFIX in the volume -- run ./build.sh first"; return
    fi

    banner "build the transport examples"
    docker run --rm \
        -v "$root":/pmix-src:ro \
        -v "$VOLUME":/opt/prte \
        -e EXAMPLES="$PTL_EXAMPLES" \
        -e PFX="$PMIX_PREFIX" \
        -e TDIR="$TESTDIR" \
        "$IMAGE" bash -euo pipefail -c '
            mkdir -p "$TDIR"
            for ex in $EXAMPLES; do
                gcc -O0 -g -o "$TDIR/$ex" "/pmix-src/examples/$ex.c" \
                    -I"$PFX/include" -I/pmix-src/examples \
                    -L"$PFX/lib" -lpmix -Wl,-rpath,"$PFX/lib"
            done
        '
    rc=$?
    if [ "$rc" != 0 ]; then
        bad "could not build the transport examples (rc=$rc)"; return
    fi
    ok "built: $PTL_EXAMPLES"

    ####################################################################
    banner "a job whose ranks are behind different servers"
    ####################################################################
    # The baseline. Everything below assumes this works, so when several
    # cases fail at once this is the one to read first.
    cleanup_swarm
    OUT="$(RUN "prterun --host node1:2,node2:2 -np 4 --map-by node --timeout 90 $TESTDIR/client 2>&1")"
    RC=$?
    judge "cross-node client" "put/get/fence carried between two servers"

    ####################################################################
    banner "a direct modex across the wire"
    ####################################################################
    # dmodex asks for data the local server does not hold, so the request
    # and its reply are a real PMIX_PTL_SEND_RECV round trip to a peer
    # behind another server -- posted recv on a dynamic tag, header, and a
    # payload large enough to be worth framing.
    cleanup_swarm
    OUT="$(RUN "prterun --host node1:2,node2:2 -np 4 --map-by node --timeout 90 $TESTDIR/dmodex 2>&1")"
    RC=$?
    judge "direct modex" "request/reply round trip to a remote peer"

    ####################################################################
    banner "the inbound message-size ceiling"
    ####################################################################
    # max_msg_size is documented as "0 means no limit". The zero branch used
    # to leave the working ceiling at its static zero instead of raising it,
    # and since that value is compared against every inbound header before
    # the payload is allocated, the server then refused every message
    # carrying a payload. Nothing on one host caught it because nothing sets
    # the parameter; here it is set explicitly.
    cleanup_swarm
    OUT="$(RUN "PMIX_MCA_pmix_ptl_base_max_msg_size=0 prterun --host node1:2,node2:2 -np 4 --map-by node --timeout 90 $TESTDIR/dmodex 2>&1")"
    RC=$?
    judge "max_msg_size=0" "zero is treated as no practical limit, not as a zero-byte cap"

    # An explicit ceiling must still be honored for traffic under it.
    cleanup_swarm
    OUT="$(RUN "PMIX_MCA_pmix_ptl_base_max_msg_size=32 prterun --host node1:2,node2:2 -np 4 --map-by node --timeout 90 $TESTDIR/dmodex 2>&1")"
    RC=$?
    judge "max_msg_size=32" "an explicit ceiling passes traffic under it"

    ####################################################################
    banner "a tool attaching to a server on its own node"
    ####################################################################
    # The rendezvous-file discovery path: the tool is given a namespace and
    # has to find pmix.<host>.tool.<nspace> in the session tmpdir. This is
    # the case that must keep working; the next one is the case that must
    # fail cleanly.
    cleanup_swarm
    nsp=""
    OUT="$(RUN "prte --daemonize --report-uri $TESTDIR/dvm.uri 2>&1")"
    sleep 3
    uri="$(RUN "cat $TESTDIR/dvm.uri 2>/dev/null" | head -1)"
    if [ -z "$uri" ]; then
        skp "tool by nspace (DVM did not report a URI)"
        skp "tool discovery is node-local (no DVM)"
        skp "tool by URI from another node (no DVM)"
    else
        # "<nspace>.<rank>;tcp4://<addr>:<port>" - split it the way
        # pmix_ptl_base_parse_uri does, from the back
        nsp="${uri%%;*}"; nsp="${nsp%.*}"
        addr="${uri#*//}"; addr="${addr%%:*}"
        ok "DVM reported a URI (nspace $nsp at $addr)"

        OUT="$(RUN "timeout 60 $TESTDIR/tool -nspace $nsp 2>&1")"
        RC=$?
        WANT='Tool ns .* rank .*: Running|PMIx_tool_init'
        if [ "$RC" = 124 ]; then
            bad "tool by nspace: HUNG"
        elif [ "$RC" != 0 ]; then
            bad "tool by nspace: exit $RC ($(echo "$OUT" | tr '\n' ' ' | tail -c 240))"
        else
            ok "tool by nspace: found its server through the rendezvous file"
        fi

        ################################################################
        banner "tool discovery is node-local, and says so"
        ################################################################
        # The same namespace from a different node. The rendezvous file
        # lives in node1's tmpdir and names node1's host, so node2 cannot
        # find it -- and the requirement is that it fails promptly and
        # says why, rather than hanging or dereferencing the URI it never
        # got. (That NULL dereference was a real defect on the
        # optional-rendezvous-file path.)
        OUT="$(ON 2 "timeout 60 $TESTDIR/tool -nspace $nsp 2>&1")"
        RC=$?
        if [ "$RC" = 124 ]; then
            bad "remote nspace lookup: HUNG instead of failing"
        elif echo "$OUT" | grep -qiE 'Segmentation|core dumped'; then
            bad "remote nspace lookup: crashed ($(echo "$OUT" | tr '\n' ' ' | tail -c 200))"
        elif [ "$RC" = 0 ]; then
            bad "remote nspace lookup: reported success for a server on another node"
        else
            ok "remote nspace lookup: refused cleanly (exit $RC)"
        fi

        ################################################################
        banner "a tool attaching to a server on another node"
        ################################################################
        # This is the case only a swarm can run. It needs the DVM's PMIx
        # listener to have bound a PUBLIC interface: the default is
        # loopback, and a loopback URI is unreachable from node2 by
        # construction. Whether PRRTE asked for remote connections is
        # PRRTE's business, so a loopback URI is a skip with the reason
        # named, not a failure of the transport.
        case "$addr" in
            127.*|::1|localhost)
                skp "tool by URI from another node (DVM listener is on loopback: $addr)" ;;
            "")
                skp "tool by URI from another node (could not parse an address out of $uri)" ;;
            *)
                OUT="$(ON 2 "timeout 60 $TESTDIR/tool -u '$uri' 2>&1")"
                RC=$?
                if [ "$RC" = 124 ]; then
                    bad "tool by URI across nodes: HUNG"
                elif [ "$RC" != 0 ]; then
                    bad "tool by URI across nodes: exit $RC ($(echo "$OUT" | tr '\n' ' ' | tail -c 240))"
                elif echo "$OUT" | grep -qiE 'Segmentation|core dumped'; then
                    bad "tool by URI across nodes: crashed"
                else
                    ok "tool by URI across nodes: handshake completed over a public interface"
                fi ;;
        esac

        ################################################################
        banner "a tool whose server goes away"
        ################################################################
        # lost_connection's client-side half: when the server drops, every
        # in-flight sendrecv has to be completed with an empty buffer so a
        # blocked caller unwinds, and the persistent recvs (notification,
        # IOF) must NOT be -- nobody is waiting on those, and handing them
        # that buffer only makes them fail to unpack a message nobody
        # sent. What this case can observe is the part that matters to a
        # user: the tool must notice and exit rather than hang.
        RUN "( sleep 5; pterm --dvm-uri '$uri' >/dev/null 2>&1 ) &" >/dev/null 2>&1
        OUT="$(RUN "timeout 60 $TESTDIR/tool -nspace $nsp 2>&1")"
        RC=$?
        if [ "$RC" = 124 ]; then
            bad "tool loses its server: HUNG (never noticed the drop)"
        elif echo "$OUT" | grep -qiE 'Segmentation|core dumped'; then
            bad "tool loses its server: crashed ($(echo "$OUT" | tr '\n' ' ' | tail -c 200))"
        else
            ok "tool loses its server: unwound and exited (exit $RC)"
        fi
    fi
    cleanup_swarm

    ####################################################################
    banner "an exhausted listener port range is reported"
    ####################################################################
    # Told to use one specific port, and that port is already taken, the
    # listener has nothing it may bind. It used to run off the end of its
    # port loop and carry on: getsockname and listen on an unbound socket
    # both succeed, the kernel picks a port of its own, and the server
    # came up advertising an address that had nothing to do with what it
    # was told to use. It must fail instead.
    cleanup_swarm
    ON 1 "nohup python3 -c \"
import socket,time
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
s.bind(('',48765)); s.listen(1); time.sleep(120)
\" >/dev/null 2>&1 &" >/dev/null 2>&1
    sleep 2
    if ON 1 "python3 -c \"
import socket,sys
s=socket.socket()
try:
    s.bind(('',48765)); sys.exit(1)
except OSError:
    sys.exit(0)
\""; then
        OUT="$(RUN "PMIX_MCA_pmix_ptl_base_ipv4_ports=48765 timeout 60 prterun --host node1:1 -np 1 $TESTDIR/client 2>&1")"
        RC=$?
        if [ "$RC" = 0 ] && echo "$OUT" | grep -qiE "$WANT"; then
            bad "exhausted port range: server came up anyway on a port it was not given"
        elif [ "$RC" = 124 ]; then
            bad "exhausted port range: HUNG"
        else
            ok "exhausted port range: refused to start (exit $RC)"
        fi
    else
        skp "exhausted port range (could not occupy the test port on node1)"
    fi
    ON 1 'pkill -9 -f "import socket,time" 2>/dev/null; true' >/dev/null 2>&1
    cleanup_swarm

    [ "$(prted_count 1 2 3 4 5 6 7 8 9 10)" = 0 ] \
        || bad "stray prted left behind at the end of the run"
}

########################################################################
# macOS: natively on this host.  One node, so nothing here is a
# *multi-node* test -- but the message-size ceiling and the local
# rendezvous-file discovery are both real, and both are cheap enough to
# be worth having without a swarm.
########################################################################

test_macos() {
    local prefix="$root/../build/master" bin uri nsp

    [ -d "$prefix" ] || prefix="/opt/prte/pmix"

    banner "native single-host transport pass"
    if [ ! -x "$prefix/bin/prterun" ] && ! command -v prterun >/dev/null; then
        skp "no prterun available -- run ./build.sh macos first"
        return
    fi

    mac_isolate "$prefix" 2>/dev/null || true

    bin="$MAC_TMP/ptl-client"
    if ! cc -O0 -g -o "$bin" "$root/examples/client.c" \
            -I"$root/include" -I"$root" \
            -L"$root/src/.libs" -lpmix -Wl,-rpath,"$root/src/.libs" \
            >/dev/null 2>&1; then
        skp "client (would not compile against the in-tree library)"
        mac_done 2>/dev/null || true
        return
    fi

    OUT="$("$MAC_BIN/prterun" -np 2 --timeout 60 "$bin" 2>&1)"; RC=$?
    judge "local client" "baseline traffic over loopback"

    # The ceiling cases do not need more than one node to be meaningful:
    # the parameter is read once, at framework registration, and the
    # defect it guards against made every payload-bearing message fail.
    OUT="$(PMIX_MCA_pmix_ptl_base_max_msg_size=0 "$MAC_BIN/prterun" -np 2 --timeout 60 "$bin" 2>&1)"
    RC=$?
    judge "max_msg_size=0" "zero is treated as no practical limit"

    OUT="$(PMIX_MCA_pmix_ptl_base_max_msg_size=32 "$MAC_BIN/prterun" -np 2 --timeout 60 "$bin" 2>&1)"
    RC=$?
    judge "max_msg_size=32" "an explicit ceiling passes traffic under it"

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
