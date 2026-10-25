#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Exercise src/tool -- the tool-role half of libpmix -- with its servers
# and the ranks they serve spread over several nodes.
#
#   ./run-tool-tests.sh linux    # in the 10-container swarm
#                                #   (requires: ./build.sh && docker compose up -d)
#   ./run-tool-tests.sh macos    # single-host subset natively
#                                #   (requires: ./build.sh macos)
#
# WHY THIS IS A MULTI-NODE TEST
#
# A tool is the only PMIx role that holds connections to SEVERAL servers at
# once and picks which of them is "primary" -- the one that services
# queries, spawns and notifications that are not directed anywhere in
# particular.  That is the whole subject of src/tool:
# pmix_tool_retry_attach, pmix_tool_retry_set, disc and getsrvrs each
# repoint pmix_client_globals.myserver while the peer objects are also held
# in pmix_server_globals.clients, and their reference accounting has to
# balance exactly against PMIx_tool_finalize.
#
# test/simple/tool_server_switch already drives that API in a loop, and it
# is a good bookkeeping test -- but both of its servers are on ONE host.
# Everything the switch is FOR is invisible in that configuration:
#
#   * both connections are loopback, so a server that is unreachable
#     because it is on another machine cannot be produced at all;
#   * a request answered by "the other server" never leaves the host, so
#     nothing distinguishes a correct primary-server choice from a wrong
#     one that happens to reach a server anyway;
#   * the IOF a tool receives is generated on its own node, so
#     tool_iof_handler never sees a payload that was relayed daemon to
#     daemon before it arrived.
#
# A tool reaches only a server on its own node: neither end takes a
# connection from another node. So the stages here put the tool beside its
# server and the work it asks for on OTHER nodes:
#
#   * IOF forwarded to a tool from ranks on several other nodes.  prterun
#     IS a tool; running it against a persistent DVM with ranks on nodes
#     it is not on means every byte it prints arrived through
#     tool_iof_handler after being relayed by a daemon that is not the
#     tool's own.  On one node the output never leaves the machine.
#     (prun, not prterun: prterun brings up a DVM of its own, while prun
#     attaches to the persistent one as a tool, which is the shape we
#     want.)
#   * a server on another node, which a tool must refuse to dial and say
#     so - PMIX_ERR_UNREACH - rather than hang or reach the wrong server.
#
# The switching bookkeeping (toolswitch: attach-as-primary -> get_servers
# -> set_server -> disconnect -> finalize) needs two servers the tool can
# reach, which means two on its own node; the macos mode below runs it.
#
# WHAT THIS DOES NOT COVER:
#
#   * the RELAY half of src/tool/pmix_tool_ops.c -- a tool that has
#     connected to another tool as its primary server sending it a spawn
#     which the receiving tool forwards ON to a real server.  PRRTE's
#     launchers do not arrange that shape, so nothing here reaches it.
#     test/unit/tool_relay covers the other half of the same decision
#     (the receiving tool has NO server, so it fork/execs the job
#     itself), which is what makes the relay's "am I attached?" test
#     observable at all; the forward-to-a-real-server arm remains
#     uncovered anywhere.
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

# The examples this runner drives.
#   tool       - attach by URI, used for the unreachable-server case
TOOL_EXAMPLES="${TOOL_EXAMPLES:-tool}"

# See run-server-tests.sh: these are COMPILED in a throwaway builder
# container and RUN in the long-lived node containers, so they must link the
# PMIx that build.sh installed into the shared volume -- the only tree that
# is the same in both.
PMIX_PREFIX=/opt/prte/pmix
TESTDIR=/opt/prte/tests-tool

# The DVMs report their URIs to files on the NODE's own filesystem: the
# shared volume is mounted read-only in the node containers, and
# --report-uri to a path in it fails silently.  Deliberately not named
# pmix*, which is the glob cleanup_swarm sweeps out of /tmp.
URI_A=/tmp/toolsuite-a.uri
URI_B=/tmp/toolsuite-b.uri

OUT=""
RC=0

# Poll for a --report-uri file rather than guessing how long the DVM takes
# to write it: --daemonize returns as soon as the DVM is forked.
# $1 = node number, $2 = path
wait_for_uri() {
    local i
    for i in 1 2 3 4 5 6 7 8 9 10; do
        ON "$1" "test -s $2" && return 0
        sleep 2
    done
    return 1
}

########################################################################
# Linux: the 10-node swarm.
########################################################################

test_linux() {
    local rc uri_a uri_b n2addr port_b nsp_b nodecount

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

    banner "build the tool examples"
    docker run --rm \
        -v "$root":/pmix-src:ro \
        -v "$VOLUME":/opt/prte \
        -e EXAMPLES="$TOOL_EXAMPLES" \
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
        bad "could not build the tool examples (rc=$rc)"
        return
    fi
    ok "built: $TOOL_EXAMPLES"

    # ------------------------------------------------------------------
    # Two INDEPENDENT DVMs, one headed on node1 and one headed on node2.
    #
    # Two DVMs rather than one, because what we want is two servers that a
    # single tool can hold at the same time and tell apart.  Within one DVM
    # the tool would have to be handed some non-head daemon's URI, and
    # nothing publishes those.  Two heads each report their own.
    # ------------------------------------------------------------------
    banner "bring up two DVMs on different nodes"
    cleanup_swarm
    ON 1 "rm -f $URI_A"; ON 2 "rm -f $URI_B"
    ON 1 "export PRTE_ALLOW_RUN_AS_ROOT=1 PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1;
          . /opt/prte/env.sh; prte --daemonize --report-uri $URI_A \
              --host node1:1,node3:1,node4:1 >/tmp/toolsuite-a.log 2>&1" >/dev/null 2>&1
    ON 2 "export PRTE_ALLOW_RUN_AS_ROOT=1 PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1;
          . /opt/prte/env.sh; prte --daemonize --report-uri $URI_B \
              --host node2:1 >/tmp/toolsuite-b.log 2>&1" >/dev/null 2>&1

    uri_a=""; uri_b=""
    wait_for_uri 1 "$URI_A" && uri_a="$(ON 1 "cat $URI_A" | head -1 | tr -d '\r')"
    wait_for_uri 2 "$URI_B" && uri_b="$(ON 2 "cat $URI_B" | head -1 | tr -d '\r')"
    if [ -z "$uri_a" ] || [ -z "$uri_b" ]; then
        bad "could not bring up two DVMs (A='${uri_a:-none}' B='${uri_b:-none}')"
        cleanup_swarm
        return
    fi
    ok "DVM A on node1 and DVM B on node2 both reported a URI"

    # ------------------------------------------------------------------
    # A server on another node cannot be reached. A server listens only
    # on its own node - its local socket, or a loopback TCP address for
    # peers of earlier releases - and a tool does not dial an address
    # that is not its own node's. DVM B reports its local socket by
    # default, whose path on node1 leads nowhere, so a tool handed that
    # fails for another reason: hand it node2's IPv4 address instead,
    # with DVM B's port when it has a TCP one, and require the refusal
    # to say why.
    # ------------------------------------------------------------------
    banner "a server on another node cannot be reached"
    # examples/tool exits with its PMIx_tool_init status, as a byte
    UNREACH=231         # PMIX_ERR_UNREACH, -25
    n2addr="$(ON 2 "hostname -I 2>/dev/null || hostname -i" | tr ' ' '\n' \
              | grep -E '^[0-9]+(\.[0-9]+){3}$' | grep -v '^127\.' | head -1)"
    case "$uri_b" in
        *";tcp4://"*) port_b="${uri_b##*:}" ;;
        *) port_b=1 ;;
    esac
    nsp_b="${uri_b%%;*}"; nsp_b="${nsp_b%.*}"
    if [ -z "$n2addr" ]; then
        skp "a server on another node (no IPv4 address for node2)"
    else
        OUT="$(ON 1 "timeout 60 $TESTDIR/tool -u '$nsp_b.0;tcp4://$n2addr:$port_b' 2>&1")"
        RC=$?
        if [ "$RC" = "$UNREACH" ] && echo "$OUT" | grep -q "not this host's"; then
            ok "a DVM on another node cannot be reached: PMIX_ERR_UNREACH, not this host's"
        else
            bad "a DVM on another node: exit $RC, not PMIX_ERR_UNREACH saying not this host's ($(echo "$OUT" | tr '\n' ' ' | tail -c 200))"
        fi
    fi

    # ------------------------------------------------------------------
    # IOF delivered to a tool from ranks on other nodes.
    #
    # prun attaches to the persistent DVM as a TOOL, so every byte it
    # prints came in through tool_iof_handler.  Point the job at nodes the
    # tool is NOT on, so each payload was relayed by a daemon other than
    # the tool's own before it arrived.
    # ------------------------------------------------------------------
    banner "IOF forwarded to a tool from ranks on other nodes"
    if [ -z "$uri_a" ]; then
        skp "tool IOF across nodes (no DVM)"
    else
        OUT="$(ON 1 "export PRTE_ALLOW_RUN_AS_ROOT=1 PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1;
                     . /opt/prte/env.sh;
                     timeout 90 prun --dvm-uri '$uri_a' --host node3:1,node4:1 \
                         -np 2 --map-by node hostname 2>&1")"
        RC=$?
        # one distinct hostname per node the ranks landed on: if the tool
        # only ever received its own node's output we would see one, and
        # if it received none we would see zero
        nodecount="$(echo "$OUT" | grep -oE '^[a-z0-9]+' | grep -c 'node[0-9]' || true)"
        if [ "$RC" = 124 ]; then
            bad "tool IOF across nodes: HUNG"
        elif [ "$RC" != 0 ]; then
            bad "tool IOF across nodes: exit $RC ($(echo "$OUT" | tr '\n' ' ' | tail -c 200))"
        elif [ "$(echo "$OUT" | grep -oE 'node[0-9]+' | sort -u | wc -l | tr -dc '0-9')" -lt 2 ]; then
            bad "tool IOF across nodes: output arrived from fewer than two nodes ($(echo "$OUT" | tr '\n' ' ' | tail -c 200))"
        else
            ok "tool IOF across nodes: output relayed to the tool from both remote nodes"
        fi
    fi

    banner "teardown"
    ON 1 ". /opt/prte/env.sh; pterm --dvm-uri '$uri_a' >/dev/null 2>&1" >/dev/null 2>&1
    ON 2 ". /opt/prte/env.sh; pterm --dvm-uri '$uri_b' >/dev/null 2>&1" >/dev/null 2>&1
    cleanup_swarm
    ON 1 "rm -f $URI_A /tmp/toolsuite-a.log"
    ON 2 "rm -f $URI_B /tmp/toolsuite-b.log"
    if [ "$(prted_count 1 2 3 4 5 6 7 8 9 10)" = 0 ]; then
        ok "no stray prted left behind"
    else
        bad "stray prted left behind"
    fi
}

########################################################################
# macOS: natively on this host.
#
# One host means one machine, so the whole point of the suite -- a primary
# server that is somewhere else -- is not reproducible.  What IS worth
# running natively is the bookkeeping: two independent DVMs on this host
# give the tool two distinct servers, so the attach/switch/disconnect
# reference accounting runs for real even though both peers are loopback.
# Say which of the two this is; do not let it stand in for the linux mode.
########################################################################

test_macos() {
    local prefix="$root/../build/master" bin uri_b

    [ -d "$prefix" ] || prefix="/opt/prte/pmix"

    banner "native single-host tool pass"
    echo "  NOTE: one host means the IOF relayed from other nodes is NOT"
    echo "        covered here. Use the linux mode for that."
    if [ ! -x "$prefix/bin/prterun" ] && ! command -v prterun >/dev/null; then
        skp "no prterun available -- run ./build.sh macos first"
        return
    fi

    mac_isolate "$prefix" 2>/dev/null || true

    bin="$MAC_TMP/toolswitch"
    if ! cc -O0 -g -o "$bin" "$root/examples/toolswitch.c" \
            -I"$root/include" -I"$root" -I"$root/examples" \
            -L"$root/src/.libs" -lpmix -Wl,-rpath,"$root/src/.libs" \
            >/dev/null 2>&1; then
        skp "toolswitch (would not compile against the in-tree library)"
        return
    fi

    "$MAC_BIN/prte" --daemonize --report-uri "$MAC_TMP/a.uri" >/dev/null 2>&1
    "$MAC_BIN/prte" --daemonize --report-uri "$MAC_TMP/b.uri" >/dev/null 2>&1
    sleep 5
    uri_b="$(head -1 "$MAC_TMP/b.uri" 2>/dev/null)"
    if [ -z "$uri_b" ]; then
        skp "toolswitch (could not bring up a second DVM)"
        macpk
        return
    fi
    # -q: a second DVM on the same host answers queries out of the same
    # state as the first, so the query stage adds nothing here
    OUT="$("$bin" -u "$uri_b" -n 3 -q 2>&1)"; RC=$?
    if echo "$OUT" | grep -q 'toolswitch: PASS'; then
        ok "toolswitch: attach/switch/disconnect bookkeeping against two DVMs"
    else
        bad "toolswitch: exit $RC ($(echo "$OUT" | tr '\n' ' ' | tail -c 300))"
    fi
    macpk
}

########################################################################

case "$mode" in
    linux) test_linux ;;
    macos) test_macos ;;
    *) echo "usage: $0 [linux|macos]" >&2; exit 2 ;;
esac

printf '\n=== summary: %d passed, %d failed, %d skipped ===\n' "$pass" "$fail" "$skip"
[ "$fail" = 0 ]
