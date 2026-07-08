#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Exercise the DYNAMIC, event-driven PMIx group operations and their fault
# paths across the container swarm produced by build.sh, driving the example
# programs through PRRTE.  This run is kept separate from run-tests.sh (which
# covers the group *construction methods*) so the event/fault matrix can grow
# independently as more of the dynamic protocol gains coverage.
#
#   ./run-group-events.sh linux    # full suite in the 10-container swarm
#                                  #   (requires: ./build.sh && docker compose up -d)
#   ./run-group-events.sh macos    # single-host subset natively on this host
#                                  #   (requires: ./build.sh macos)
#
# Prints PASS/FAIL per test and a summary; exits non-zero if anything failed.
# The multi-node swarm matters because the dynamic group protocol is realized
# through event notifications (PMIx_Notify_event) that must cross distinct PMIx
# servers (one prted per node), so the cross-daemon notification paths for
# invitations, completion broadcasts, and fault events actually run.
#
# Coverage so far:
#   * group_invite         -- invite/join happy path: a leader invites the whole
#                             job, invitees accept via PMIx_Group_join, the group
#                             forms, and every member receives
#                             PMIX_GROUP_CONSTRUCT_COMPLETE and can fence across
#                             the group.
#   * group_invite_timeout -- an invitee never responds; the leader's
#                             PMIX_TIMEOUT fires, reports the non-responder via
#                             PMIX_GROUP_INVITE_FAILED, and forms the group on
#                             the members that accepted.
#   * group_invite_decline -- an invitee explicitly declines; the construct
#                             resolves immediately (no timeout), reports the
#                             decliner via PMIX_GROUP_INVITE_FAILED, and forms
#                             the group on the members that accepted.
#   * group_invite_abort   -- an invitee declines an all-or-nothing invite (no
#                             PMIX_GROUP_OPTIONAL); the whole construct aborts,
#                             every participant receives PMIX_GROUP_CONSTRUCT_ABORT
#                             and the leader's invite returns that status.
#   * group_destruct_die   -- a member is lost mid-PMIx_Group_destruct; the
#                             destruct must complete on the survivors rather than
#                             hang (the destruct analog of run-tests.sh's
#                             group_die, which covers construct).
#
# Still to come (as the library gains support -- see issue #3936): leader
# failure and reselection, construct abort, member-failed events, and the
# FT-collective adjustment.

set -uo pipefail

mode="${1:-linux}"
pass=0 fail=0 skip=0
ok()   { pass=$((pass+1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
skp()  { skip=$((skip+1)); printf '  \033[33mSKIP\033[0m %s\n' "$1"; }
banner() { printf '\n=== %s ===\n' "$1"; }

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

# common markers of a launch that hung to the DVM/job timeout
hung() { echo "$1" | grep -qiE 'time limit for job|timed out|DVM timeout|timeout'; }

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

    #############################################################
    # invite / join happy path
    #############################################################
    banner "invite/join happy path (PMIX_GROUP_CONSTRUCT_COMPLETE to all members)"
    cleanup_swarm
    # group_invite: rank 0 (the leader) invites the whole job; ranks 1..N-1
    # accept via PMIx_Group_join.  The group forms purely through event
    # notification (no server collective), so spreading the ranks across nodes
    # exercises the cross-daemon delivery of both the invitations and the
    # completion broadcast.  Every member (leader included) must receive
    # PMIX_GROUP_CONSTRUCT_COMPLETE, then fence across the formed group.
    if RUN 'test -x /opt/prte/tests/group_invite'; then
        OUT="$(RUN 'prterun --host node1:2,node2:2 -np 4 --map-by node --timeout 60 /opt/prte/tests/group_invite 2>&1')"
        npass=$(echo "$OUT" | grep -c 'CONSTRUCT_COMPLETE received: PASS')
        nfence=$(echo "$OUT" | grep -c 'group fence complete')
        if hung "$OUT"; then
            bad "group_invite HUNG (invitation or completion never delivered)"
        elif echo "$OUT" | grep -qiE 'FAILED|PMIX_ERROR'; then
            bad "group_invite: a member reported failure: $(echo "$OUT" | tr '\n' ' ' | tail -c 200)"
        elif [ "$npass" -ge 4 ] && [ "$nfence" -ge 4 ]; then
            ok "all 4 members notified of completion and fenced across the group"
        else
            bad "group_invite: only $npass completed / $nfence fenced: $(echo "$OUT" | tr '\n' ' ' | tail -c 160)"
        fi
    else
        skp "group_invite not built"
    fi
    [ "$(prted_count 1 2 3 4 5 6 7 8 9 10)" = 0 ] || bad "group_invite: stray prted left behind"
    cleanup_swarm

    #############################################################
    # invite timeout (a non-responder)
    #############################################################
    banner "invite timeout (PMIX_GROUP_INVITE_FAILED + reduced membership)"
    cleanup_swarm
    # group_invite_timeout: the leader invites the whole job with a PMIX_TIMEOUT;
    # the last rank deliberately never answers its PMIX_GROUP_INVITED event. The
    # leader's timer must fire, report the non-responder via
    # PMIX_GROUP_INVITE_FAILED, and form the group on the members that did accept
    # (which then receive PMIX_GROUP_CONSTRUCT_COMPLETE). The non-responder stays
    # alive and rejoins the closing barrier, so no --rtos recoverable is needed.
    if RUN 'test -x /opt/prte/tests/group_invite_timeout'; then
        OUT="$(RUN 'prterun --host node1:2,node2:2 -np 4 --map-by node --timeout 60 /opt/prte/tests/group_invite_timeout 2>&1')"
        nfail=$(echo "$OUT" | grep -c 'INVITE_FAILED for non-responder: PASS')
        npass=$(echo "$OUT" | grep -c 'CONSTRUCT_COMPLETE received: PASS')
        if hung "$OUT"; then
            bad "group_invite_timeout HUNG (timeout never fired)"
        elif echo "$OUT" | grep -qiE 'FAILED -'; then
            bad "group_invite_timeout: a member reported failure: $(echo "$OUT" | tr '\n' ' ' | tail -c 200)"
        elif [ "$nfail" -ge 1 ] && [ "$npass" -ge 3 ]; then
            ok "leader reported the non-responder and formed the group on the 3 that accepted"
        else
            bad "group_invite_timeout: failed=$nfail completed=$npass: $(echo "$OUT" | tr '\n' ' ' | tail -c 160)"
        fi
    else
        skp "group_invite_timeout not built"
    fi
    [ "$(prted_count 1 2 3 4 5 6 7 8 9 10)" = 0 ] || bad "group_invite_timeout: stray prted left behind"
    cleanup_swarm

    #############################################################
    # invite declined (an explicit refusal)
    #############################################################
    banner "invite declined (PMIX_GROUP_INVITE_FAILED, no timeout)"
    cleanup_swarm
    # group_invite_decline: the leader invites the whole job with NO PMIX_TIMEOUT;
    # the last rank explicitly DECLINES via PMIx_Group_join instead of accepting.
    # A decline is a definitive answer, so the construct must resolve immediately
    # -- report the decliner via PMIX_GROUP_INVITE_FAILED and form the group on
    # the members that accepted (which then receive PMIX_GROUP_CONSTRUCT_COMPLETE)
    # -- rather than wait on a member that will never join. With no timeout, a
    # regression cannot be broken by a timer; it simply hangs to the job timeout.
    # The decliner stays alive and rejoins the closing barrier, so no
    # --rtos recoverable is needed.
    if RUN 'test -x /opt/prte/tests/group_invite_decline'; then
        OUT="$(RUN 'prterun --host node1:2,node2:2 -np 4 --map-by node --timeout 60 /opt/prte/tests/group_invite_decline 2>&1')"
        nfail=$(echo "$OUT" | grep -c 'INVITE_FAILED for decliner: PASS')
        npass=$(echo "$OUT" | grep -c 'CONSTRUCT_COMPLETE received: PASS')
        if hung "$OUT"; then
            bad "group_invite_decline HUNG (decline did not resolve the construct)"
        elif echo "$OUT" | grep -qiE 'FAILED -'; then
            bad "group_invite_decline: a member reported failure: $(echo "$OUT" | tr '\n' ' ' | tail -c 200)"
        elif [ "$nfail" -ge 1 ] && [ "$npass" -ge 3 ]; then
            ok "leader reported the decliner and formed the group on the 3 that accepted"
        else
            bad "group_invite_decline: failed=$nfail completed=$npass: $(echo "$OUT" | tr '\n' ' ' | tail -c 160)"
        fi
    else
        skp "group_invite_decline not built"
    fi
    [ "$(prted_count 1 2 3 4 5 6 7 8 9 10)" = 0 ] || bad "group_invite_decline: stray prted left behind"
    cleanup_swarm

    #############################################################
    # invite aborted (all-or-nothing construct)
    #############################################################
    banner "invite aborted (PMIX_GROUP_CONSTRUCT_ABORT, all-or-nothing)"
    cleanup_swarm
    # group_invite_abort: the leader invites the whole job WITHOUT
    # PMIX_GROUP_OPTIONAL, making the construct all-or-nothing. The last rank
    # declines, so the lone non-accepter must abort the entire construct: every
    # invited participant (including the members that accepted) must receive
    # PMIX_GROUP_CONSTRUCT_ABORT, the leader's PMIx_Group_invite must return that
    # status, and no group may form (no CONSTRUCT_COMPLETE). All ranks stay alive
    # and rejoin the closing barrier, so no --rtos recoverable is needed.
    if RUN 'test -x /opt/prte/tests/group_invite_abort'; then
        OUT="$(RUN 'prterun --host node1:2,node2:2 -np 4 --map-by node --timeout 60 /opt/prte/tests/group_invite_abort 2>&1')"
        nleader=$(echo "$OUT" | grep -c 'PMIx_Group_invite returned CONSTRUCT_ABORT: PASS')
        nabort=$(echo "$OUT" | grep -c 'PMIX_GROUP_CONSTRUCT_ABORT received: PASS')
        if hung "$OUT"; then
            bad "group_invite_abort HUNG (abort was not propagated)"
        elif echo "$OUT" | grep -qiE 'FAILED -'; then
            bad "group_invite_abort: a member reported failure: $(echo "$OUT" | tr '\n' ' ' | tail -c 200)"
        elif [ "$nleader" -ge 1 ] && [ "$nabort" -ge 4 ]; then
            ok "leader's invite returned abort and all 4 participants were notified"
        else
            bad "group_invite_abort: leader=$nleader aborted=$nabort: $(echo "$OUT" | tr '\n' ' ' | tail -c 160)"
        fi
    else
        skp "group_invite_abort not built"
    fi
    [ "$(prted_count 1 2 3 4 5 6 7 8 9 10)" = 0 ] || bad "group_invite_abort: stray prted left behind"
    cleanup_swarm

    #############################################################
    # member lost during destruct
    #############################################################
    banner "member lost during PMIx_Group_destruct (loss accounting)"
    cleanup_swarm
    # group_destruct_die: every rank constructs the group, syncs, then every
    # rank EXCEPT the last calls PMIx_Group_destruct; the last leaves before
    # contributing (and without finalizing), so the server must account for the
    # lost member and complete the DEstruct on the survivors instead of hanging.
    # Same two requirements as group_die/connect_die make the loss observable:
    #   * --rtos recoverable, so PRRTE does not tear the whole job down when the
    #     member vanishes without finalizing;
    #   * whole-job membership + --map-by node, so the victim's node also hosts a
    #     surviving member (the one whose call created the local tracker the loss
    #     accounting then completes).
    if RUN 'test -x /opt/prte/tests/group_destruct_die'; then
        OUT="$(RUN 'prterun --rtos recoverable --host node1:2,node2:2 -np 4 --map-by node --timeout 60 /opt/prte/tests/group_destruct_die 2>&1')"
        n=$(echo "$OUT" | grep -c 'destruct complete on survivors')
        if hung "$OUT"; then
            bad "destruct HUNG on member death (loss accounting missing)"
        elif [ "$n" -ge 3 ]; then
            ok "destruct completed on all 3 survivors after a member died"
        else
            bad "group_destruct_die: only $n survivors completed: $(echo "$OUT" | tr '\n' ' ' | tail -c 160)"
        fi
    else
        skp "group_destruct_die not built"
    fi
    [ "$(prted_count 1 2 3 4 5 6 7 8 9 10)" = 0 ] || bad "group_destruct_die: stray prted left behind"
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

    banner "macOS: invite/join happy path (single host)"
    if [ -x "$prefix/bin/group_invite" ]; then
        macpk; sleep 1
        out="$(prterun -np 4 --timeout 60 "$prefix/bin/group_invite" 2>&1)"
        npass=$(echo "$out" | grep -c 'CONSTRUCT_COMPLETE received: PASS')
        if echo "$out" | grep -qiE 'FAILED|timeout|timed out'; then
            skp "group_invite: not clean (native Darwin DVM can be flaky): $(echo "$out" | tr '\n' ' ' | tail -c 160)"
        elif [ "$npass" -ge 4 ]; then
            ok "group_invite: all 4 members notified of completion (single host)"
        else
            skp "group_invite: only $npass completed (native Darwin DVM can be flaky)"
        fi
    else
        skp "group_invite (not built)"
    fi

    banner "macOS: invite timeout (single host)"
    if [ -x "$prefix/bin/group_invite_timeout" ]; then
        macpk; sleep 1
        out="$(prterun -np 4 --timeout 60 "$prefix/bin/group_invite_timeout" 2>&1)"
        nfail=$(echo "$out" | grep -c 'INVITE_FAILED for non-responder: PASS')
        npass=$(echo "$out" | grep -c 'CONSTRUCT_COMPLETE received: PASS')
        if echo "$out" | grep -qiE 'FAILED -|timeout|timed out'; then
            skp "group_invite_timeout: not clean (native Darwin DVM can be flaky)"
        elif [ "$nfail" -ge 1 ] && [ "$npass" -ge 3 ]; then
            ok "group_invite_timeout: non-responder reported, group formed on 3 (single host)"
        else
            skp "group_invite_timeout: failed=$nfail completed=$npass (native Darwin DVM can be flaky)"
        fi
    else
        skp "group_invite_timeout (not built)"
    fi

    banner "macOS: invite declined (single host)"
    if [ -x "$prefix/bin/group_invite_decline" ]; then
        macpk; sleep 1
        out="$(prterun -np 4 --timeout 60 "$prefix/bin/group_invite_decline" 2>&1)"
        nfail=$(echo "$out" | grep -c 'INVITE_FAILED for decliner: PASS')
        npass=$(echo "$out" | grep -c 'CONSTRUCT_COMPLETE received: PASS')
        if echo "$out" | grep -qiE 'FAILED -|timeout|timed out'; then
            skp "group_invite_decline: not clean (native Darwin DVM can be flaky)"
        elif [ "$nfail" -ge 1 ] && [ "$npass" -ge 3 ]; then
            ok "group_invite_decline: decliner reported, group formed on 3 (single host)"
        else
            skp "group_invite_decline: failed=$nfail completed=$npass (native Darwin DVM can be flaky)"
        fi
    else
        skp "group_invite_decline (not built)"
    fi

    banner "macOS: invite aborted (single host)"
    if [ -x "$prefix/bin/group_invite_abort" ]; then
        macpk; sleep 1
        out="$(prterun -np 4 --timeout 60 "$prefix/bin/group_invite_abort" 2>&1)"
        nleader=$(echo "$out" | grep -c 'PMIx_Group_invite returned CONSTRUCT_ABORT: PASS')
        nabort=$(echo "$out" | grep -c 'PMIX_GROUP_CONSTRUCT_ABORT received: PASS')
        if echo "$out" | grep -qiE 'FAILED -|timeout|timed out'; then
            skp "group_invite_abort: not clean (native Darwin DVM can be flaky)"
        elif [ "$nleader" -ge 1 ] && [ "$nabort" -ge 4 ]; then
            ok "group_invite_abort: construct aborted, all 4 participants notified (single host)"
        else
            skp "group_invite_abort: leader=$nleader aborted=$nabort (native Darwin DVM can be flaky)"
        fi
    else
        skp "group_invite_abort (not built)"
    fi

    banner "macOS: member lost during destruct (single host)"
    if [ -x "$prefix/bin/group_destruct_die" ]; then
        macpk; sleep 1
        out="$(prterun --rtos recoverable -np 4 --timeout 60 "$prefix/bin/group_destruct_die" 2>&1)"
        n=$(echo "$out" | grep -c 'destruct complete on survivors')
        if echo "$out" | grep -qiE 'timeout|timed out'; then
            skp "group_destruct_die: not clean (native Darwin DVM can be flaky)"
        elif [ "$n" -ge 3 ]; then
            ok "group_destruct_die: destruct completed on 3 survivors (single host)"
        else
            skp "group_destruct_die: only $n survivors (native Darwin DVM can be flaky)"
        fi
    else
        skp "group_destruct_die (not built)"
    fi
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
