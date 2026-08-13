#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Where does the output of a job spawned by an ordinary CLIENT go?
#
#   ./run-spawn-iof.sh linux   # in the 10-container swarm
#                              #   (requires: ./build.sh && docker compose up -d)
#
# WHY THIS IS A MULTI-NODE TEST -- AND WHY THE GEOMETRY IS THE WHOLE POINT
#
# A job spawned through PMIx_Spawn should be treated the way its parent is
# being treated: whoever is receiving the parent job's output should receive
# the child's.  docs/how-things-work/iof_inheritance.rst is the design; the
# short version of the mechanism is that PMIx decides who gets a copy by
# walking the subscriptions (pmix_iof_req_t) held by ONE PMIx server -- the
# server of the daemon the requesting tool attached to -- while PRRTE decides
# which daemon a job's output is relayed to.
#
# So there are two daemons in this story and they need not be the same one:
#
#   * the daemon the TOOL attached to, which holds the subscription covering
#     the parent job, and
#   * the daemon hosting the RANK THAT CALLS PMIx_Spawn, which is where the
#     spawn command lands and therefore where PMIx performs the inheritance.
#
# Put the spawning rank on the tool's own daemon and the two coincide: the
# subscription to clone is right there, and the case passes.  That is a real
# configuration -- a single-node DVM, or prterun on one node -- and it is
# also the configuration in which this test proves almost nothing, because a
# library that routes nothing at all still passes it.  The cases that carry
# the information are the ones where the spawning rank sits on a DIFFERENT
# daemon, and they are run after the co-located one because that one is what
# says the rig itself works when a later case fails.
#
# The far end of the same question is where the child's output ARRIVES.  A
# child placed on the tool's own node reaches the tool's server from a
# process that server hosts, which a delivery-time decision could answer out
# of local state; case 3 pushes tool, spawner and child onto three different
# daemons so that nothing about the answer can be local.
#
# Nothing in `make check` reaches this: test/unit/iof_inherit.c drives the
# parser and the registration directly and reads the subscription list back,
# which establishes WHICH subscriptions get created and for whom, but it
# cannot say which daemon holds them or where the bytes arrive.  Neither can
# test/simple/simptest, which cannot host a spawn at all.
#
# READING A FAILURE.  If the child's lines are missing, that alone does not
# say whether the forwarding dropped them or the child never ran.  So the
# example is run with --markers, which makes every process drop a file of the
# same content on the node it ran on, by a channel that has nothing to do
# with IOF.  Each case reports both: what came back through the tool, and
# what actually ran where.
#
# THE TOOL'S OWN DAEMON IS THE THIRD VARIABLE.  Cases 1-4 all attach their
# tool to the HNP, which is what prun and prterun do, and the HNP is special:
# every prted forwards to it, and it hands everything it receives to its OWN
# PMIx server.  So those cases never ask whether output can reach a tool
# whose subscription is held somewhere else.  Case 5 does -- it puts the tool
# on a non-HNP daemon and has it pull a job that was ALREADY RUNNING, which
# is the only case here that depends on the runtime recording an interest
# after the fact rather than at launch.
#
# WHAT THIS DOES NOT COVER.  Output forwarding to a tool that is watching a
# job through something other than PMIx_IOF_pull or a spawn it issued -- no
# such path exists today -- and the stdin direction, which is not what any
# of this is about.
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

# See run-server-tests.sh: the example is COMPILED in a throwaway builder
# container and RUN in the long-lived node containers, so it must link the
# PMIx that build.sh installed into the shared volume -- the only tree that
# is the same in both.
PMIX_PREFIX=/opt/prte/pmix
STAGE=/opt/prte/tests-iof
PROG="$STAGE/spawn_iof"
WATCHER="$STAGE/iof_watcher"

# The DVM spans six nodes; a parent job of one rank leaves five nodes' worth
# of slots free.  --host IS the allocation, so a parent that fills it leaves
# the spawn nowhere to land and the job blocks until the launch timeout --
# which reads exactly like a library hang and is not one.
DVM_HOSTS="node1:1,node2:1,node3:1,node4:1,node5:1,node6:1"

# Markers go to each node's own /tmp: the shared volume is mounted READ-ONLY
# in the node containers.  The name deliberately does not match pmix*, which
# is the glob cleanup_swarm sweeps out of /tmp between cases -- we want to
# clear these ourselves, at a moment we choose.
MARKERS=/tmp
clear_markers() {
    local n
    for n in $(seq 1 10); do
        ON "$n" "rm -f $MARKERS/spawn_iof.* 2>/dev/null; true" >/dev/null 2>&1
    done
}
# What actually ran, and where -- gathered from every node, IOF uninvolved.
collect_markers() {
    local n
    for n in $(seq 1 10); do
        ON "$n" "cat $MARKERS/spawn_iof.* 2>/dev/null" 2>/dev/null
    done
}

OUT=""
RAN=""

# Judge one case.  $1 = label, $2 = what a pass means.
#
# The parent's own lines are checked first and separately.  If the tool is
# not receiving the PARENT's output then the premise of the case is gone and
# the child's absence says nothing about inheritance -- report that, rather
# than a misleading verdict about the child.
judge() {
    local name="$1" what="$2"

    if echo "$OUT" | grep -qiE 'time limit for job|timed out|DVM timeout'; then
        bad "$name: HUNG (hit the launch timeout)"
        return
    fi
    if echo "$OUT" | grep -qiE 'Segmentation|core dumped|: ERROR!'; then
        bad "$name: $(echo "$OUT" | grep -iE 'Segmentation|core dumped|: ERROR!' | head -1)"
        return
    fi
    if ! echo "$OUT" | grep -q 'SPAWN_IOF parent '; then
        bad "$name: the tool never saw the PARENT's output -- the case cannot judge the child"
        echo "     $(echo "$OUT" | tr '\n' ' ' | tail -c 200)" >&2
        return
    fi
    if ! echo "$OUT" | grep -q 'SPAWN_IOF spawned '; then
        bad "$name: PMIx_Spawn did not report a child namespace"
        echo "     $(echo "$OUT" | tr '\n' ' ' | tail -c 200)" >&2
        return
    fi
    if ! echo "$RAN" | grep -q 'SPAWN_IOF child '; then
        bad "$name: the child job never ran (no marker on any node) -- not an IOF result"
        return
    fi

    # The premise holds and the child ran.  Now the actual question.
    if echo "$OUT" | grep -q 'SPAWN_IOF child ' && echo "$OUT" | grep -q 'SPAWN_IOF childerr '; then
        ok "$name: $what"
    elif echo "$OUT" | grep -qE 'SPAWN_IOF child|SPAWN_IOF childerr'; then
        bad "$name: only one of the child's two channels came back"
    else
        bad "$name: the child ran ($(echo "$RAN" | grep 'SPAWN_IOF child ' | head -1 |
             sed 's/.*host /on /')) but NONE of its output reached the tool"
    fi
}

# Where each process ran, printed under every case: the whole subject is
# which daemon is which, so a result nobody can place is not worth much.
show_layout() {
    echo "$RAN" | sed 's/^/     /'
}

########################################################################

test_linux() {
    local rc

    swarm_up_or_die

    banner "preflight"
    if ! RUN 'command -v prterun prte prun pterm >/dev/null'; then
        bad "prterun/prte/prun/pterm missing -- did ./build.sh run?"; return
    fi
    ok "prte tools on PATH"

    if ! docker volume inspect "$VOLUME" >/dev/null 2>&1; then
        bad "build volume $VOLUME missing -- run ./build.sh first"; return
    fi

    # The nodes are long-lived containers while the install they read is
    # replaced under them, so a node predating the current image runs a
    # different PMIx than the builder container compiles against.
    local imgid cimg n stalenodes=""
    imgid=$(docker images --no-trunc --format '{{.ID}}' "$IMAGE" 2>/dev/null | head -1)
    for n in $(seq 1 10); do
        cimg=$(docker inspect "$NODE$n" --format '{{.Image}}' 2>/dev/null)
        [ "$cimg" = "$imgid" ] || stalenodes="$stalenodes node$n"
    done
    if [ -n "$stalenodes" ]; then
        bad "containers predate $IMAGE:$stalenodes"
        echo "     Recreate them: ${SWARM_ENV}docker compose up -d --force-recreate" >&2
        return
    fi
    ok "all 10 containers are on the current $IMAGE"

    # Which PRRTE this is matters more here than in most of these runners:
    # the routing half of the behavior under test is PRRTE's.
    banner "the launcher under test"
    RUN 'prte_info --version 2>/dev/null | head -2' | sed 's/^/     /'

    banner "build the example"
    docker run --rm \
        -v "$root":/pmix-src:ro \
        -v "$VOLUME":/opt/prte \
        -e PFX="$PMIX_PREFIX" \
        -e STAGE="$STAGE" \
        "$IMAGE" bash -euo pipefail -c '
            mkdir -p "$STAGE"
            for ex in spawn_iof iof_watcher; do
                gcc -O0 -g -o "$STAGE/$ex" "/pmix-src/examples/$ex.c" \
                    -I"$PFX/include" -I/pmix-src/examples \
                    -L"$PFX/lib" -lpmix -Wl,-rpath,"$PFX/lib" || exit 1
            done
        '
    rc=$?
    if [ "$rc" != 0 ]; then
        bad "could not build examples/spawn_iof.c (rc=$rc)"; return
    fi
    ok "built spawn_iof and iof_watcher"

    # ------------------------------------------------------------------
    # Case 1: the spawning rank is on the tool's own daemon.
    #
    # prun runs on node1 and attaches to the DVM through node1's rendezvous
    # file, so the subscription covering the parent job is held by the PMIx
    # server inside node1's daemon -- and the parent's single rank is placed
    # on node1 as well.  The spawn command therefore lands on the very
    # server that holds the subscription, which is the co-located case the
    # spawn-time inheritance already answers.
    #
    # This is the control.  It says the example, the launcher and the
    # capture all work, so that a failure in case 2 is about routing.
    # ------------------------------------------------------------------
    banner "case 1: spawner on the tool's daemon (control)"
    cleanup_swarm
    clear_markers
    OUT="$(RUN "prte --daemonize --host $DVM_HOSTS >/tmp/iof-dvm.log 2>&1; sleep 5;
                prun --host node1:1 -np 1 --timeout 90 $PROG --markers $MARKERS 2>&1;
                echo '--- done ---';
                pterm >/dev/null 2>&1")"
    RAN="$(collect_markers)"
    show_layout
    judge "co-located" "the child's output came back to the tool on both channels"
    cleanup_swarm

    # ------------------------------------------------------------------
    # Case 2: the spawning rank is on a different daemon.  THE REPRODUCER.
    #
    # Identical to case 1 but for one thing: the parent's rank is placed on
    # node2, so the spawn command lands on node2's PMIx server while the
    # tool's subscription is still held by node1's.  Nothing else moves --
    # same DVM, same tool, same tool attachment, same example.
    #
    # The parent's own output still comes back (every prted forwards to the
    # HNP, and the HNP hands everything it receives to its own PMIx server,
    # where prun's subscription for the parent namespace matches).  The
    # child's is the question.
    # ------------------------------------------------------------------
    banner "case 2: spawner on another daemon (the reproducer)"
    clear_markers
    OUT="$(RUN "prte --daemonize --host $DVM_HOSTS >/tmp/iof-dvm.log 2>&1; sleep 5;
                prun --host node2:1 -np 1 --timeout 90 $PROG --markers $MARKERS 2>&1;
                echo '--- done ---';
                pterm >/dev/null 2>&1")"
    RAN="$(collect_markers)"
    show_layout
    judge "cross-daemon" "the child's output came back to the tool on both channels"
    cleanup_swarm

    # ------------------------------------------------------------------
    # Case 3: three different daemons -- tool, spawner, child.
    #
    # Case 2 leaves the child's placement to the mapper, which fills from
    # the first free slot -- node1, the tool's own node. So the output
    # arrives at the tool's server from a process that server hosts, and
    # a delivery-time decision made there could be reading local state
    # rather than routed output. Listing node2 FIRST puts rank 0 (the
    # spawner) there and rank 1 on node1, which consumes the free slot
    # the child would have taken and pushes it out to node3.
    #
    # Tool on node1, spawner on node2, child on node3: no two of the
    # three are the same daemon, and the child's bytes reach the tool
    # only by being relayed to the HNP and matched there.
    # ------------------------------------------------------------------
    banner "case 3: tool, spawner and child on three different daemons"
    clear_markers
    OUT="$(RUN "prte --daemonize --host $DVM_HOSTS >/tmp/iof-dvm.log 2>&1; sleep 5;
                prun --host node2:1,node1:1 -np 2 --timeout 90 $PROG --markers $MARKERS 2>&1;
                echo '--- done ---';
                pterm >/dev/null 2>&1")"
    RAN="$(collect_markers)"
    show_layout
    if [ "$(echo "$RAN" | sed -n 's/.*host //p' | sort -u | wc -l)" -lt 3 ]; then
        # The mapper placed things differently than the geometry above
        # assumes, so whatever this run shows is not the case described.
        skp "three-daemon: the ranks did not land on three distinct nodes"
    else
        judge "three-daemon" "the child's output crossed two daemons to reach the tool"
    fi
    cleanup_swarm

    # ------------------------------------------------------------------
    # Case 4: the same, under prterun rather than a persistent DVM.
    #
    # prterun brings up a DVM of its own and is the tool, so "the tool's
    # daemon" is the HNP by construction -- and the HNP is prterun itself,
    # here on node1.  Leaving node1 OUT of the host list is what puts the
    # spawning rank somewhere else: --host is the allocation, so with no
    # slot on node1 the single rank lands on node2 while the tool stays
    # where it is.
    #
    # This is reported separately because prterun ALSO writes output
    # locally, which can carry a child's bytes to the terminal by a route
    # that has nothing to do with the subscription being inherited.  A pass
    # here is therefore weaker evidence than a pass in case 2 -- it is here
    # to show what a user sees, not to stand in for it.
    # ------------------------------------------------------------------
    banner "case 4: the same under prterun"
    clear_markers
    OUT="$(RUN "prterun --host node2:1,node3:1,node4:1,node5:1,node6:1 \
                    -np 1 --timeout 90 $PROG --markers $MARKERS 2>&1;
                echo '--- done ---'")"
    RAN="$(collect_markers)"
    show_layout
    judge "prterun" "the child's output reached the terminal"
    clear_markers
    cleanup_swarm

    # ------------------------------------------------------------------
    # Case 5: a tool attaches to a NON-HNP daemon, pulls a running job's
    # output, and then sees the output of a job that job spawns.
    #
    # This is the case none of the others reach, and the only one that
    # exercises the runtime recording an interest AFTER a job is already
    # running. The tool is started on node3 and attaches to node3's own
    # daemon - not the HNP - so its subscription is held by a PMIx server
    # that hosts none of the job's processes. Nothing it wants is local:
    # the parent rank is on node2 and the child lands elsewhere again, so
    # every byte it sees had to be relayed to node3 on purpose.
    #
    # ORDERING IS THE POINT. The watcher has to be subscribed BEFORE the
    # child exists, or a pass would only show cached output being
    # replayed. --delay holds the spawn open long enough for that, and
    # the two lines asserted below are both written AFTER the watcher
    # subscribes: "spawned" by the parent when the delay expires, and the
    # child's own output after that. Nothing here depends on the cache.
    # ------------------------------------------------------------------
    banner "case 5: a tool on a non-HNP daemon pulls a running job, then sees its child"
    clear_markers
    RUN "prte --daemonize --host $DVM_HOSTS >/tmp/iof-dvm.log 2>&1" >/dev/null 2>&1
    sleep 5
    # the parent announces itself at once, then waits before spawning
    RUN "nohup prun --host node2:1 -np 1 --timeout 150 \
             $PROG --markers $MARKERS --delay 30 > /tmp/iof-parent.log 2>&1 &" >/dev/null 2>&1
    sleep 10
    pns="$(RUN "sed -n 's/^SPAWN_IOF parent \\([^:]*\\):.*/\\1/p' /tmp/iof-parent.log | head -1" | tr -d '\r\n ')"
    if [ -z "$pns" ]; then
        bad "watcher: the parent job never announced a namespace to attach to"
        RUN "cat /tmp/iof-parent.log 2>/dev/null | tail -5" >&2
    else
        # 45s covers the parent's remaining delay plus the child's run
        WOUT="$(ON 3 "timeout 90 $WATCHER $pns 45 2>&1")"
        RAN="$(collect_markers)"
        show_layout
        if ! echo "$WOUT" | grep -q 'WATCHER watching'; then
            bad "watcher: could not attach to node3's daemon or register the pull"
            echo "     $(echo "$WOUT" | tr '\n' ' ' | tail -c 200)" >&2
        elif ! echo "$RAN" | grep -q 'SPAWN_IOF child '; then
            bad "watcher: the child job never ran -- not an IOF result"
        elif ! echo "$WOUT" | grep -q 'SPAWN_IOF spawned '; then
            bad "watcher: the running PARENT job's output never reached the tool"
            echo "     $(echo "$WOUT" | tr '\n' ' ' | tail -c 300)" >&2
        elif ! echo "$WOUT" | grep -q 'SPAWN_IOF child '; then
            bad "watcher: saw the parent but NOT the job it spawned"
            echo "     $(echo "$WOUT" | tr '\n' ' ' | tail -c 300)" >&2
        else
            ok "watcher: a tool on a non-HNP daemon saw the running job AND its child"
        fi
    fi
    RUN "pterm >/dev/null 2>&1; rm -f /tmp/iof-parent.log" >/dev/null 2>&1
    clear_markers
    cleanup_swarm
}

########################################################################

case "$mode" in
    linux) test_linux ;;
    macos)
        # One host means one daemon, so the two daemons in the story above
        # are the same daemon and the only case that can be built is the
        # control.  Saying so is more useful than running it.
        echo "This runner has no macOS mode: the behavior it tests is about"
        echo "WHICH daemon holds a subscription, and one host has only one."
        exit 2 ;;
    *) echo "usage: $0 [linux]" >&2; exit 2 ;;
esac

banner "summary"
printf '  passed %d, failed %d, skipped %d\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ]
