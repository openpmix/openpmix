#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Shared by build.sh, run-tests.sh, run-group-events.sh, run-topology.sh and
# run-python.sh.
# SOURCED, never executed:  . "$(dirname "$0")/swarm-common.sh"
#
# Two things live here because all four scripts need them to agree.
#
# 1. WHICH SWARM.  Every global name this harness claims -- the compose
#    project, the ten container names, the build volume, and (by way of the
#    project name) the docker network -- is derived from $PMIX_SWARM, so a
#    second clone of openpmix on the same host can drive its own swarm instead
#    of fighting over this one.  Unset, it is "pmix" and every name is exactly
#    what it has always been.
#
# 2. HOW A NODE IS CLEANED.  The runners each used to carry their own
#    copy of cleanup_swarm, and the copies had already drifted.  One copy is
#    one place to fix, which is the whole point: an incomplete sweep does not
#    fail the case that caused it, it fails the next several with a message
#    about "multiple possible servers".
#

# --- which swarm ------------------------------------------------------------
PMIX_SWARM="${PMIX_SWARM:-pmix}"
# Reject what docker would reject, rather than leaving it to surface as a
# confusing compose error.  The filter runs through LC_ALL=C tr and not a
# shell [a-z] range because in a UTF-8 locale that range follows collation
# order and matches 'B' quite happily -- so the obvious form of this test
# accepts "Bad_Name".  The leading-character check uses a literal set for the
# same reason.
case "$PMIX_SWARM" in [_-]*) PMIX_SWARM="" ;; esac
if [ -z "$PMIX_SWARM" ] || \
   [ "$PMIX_SWARM" != "$(printf '%s' "$PMIX_SWARM" | LC_ALL=C tr -cd 'a-z0-9_-')" ]; then
    echo "PMIX_SWARM must be lowercase [a-z0-9_-] and start with a letter or digit" >&2
    exit 2
fi

NODE="$PMIX_SWARM-node"                 # container names: ${NODE}1 .. ${NODE}10
VOLUME="${VOLUME:-$PMIX_SWARM-build}"
# NOT per-swarm: the image is read-only to a running swarm and expensive to
# build (it clones and autogens PRRTE), so two swarms share one.  The cost is
# that rebuilding it moves it under the other swarm's feet, and that swarm's
# containers then fail the "running the current image" preflight until they
# are recreated.
IMAGE="${IMAGE:-pmix-swarm:latest}"
# Prefix for the compose commands the scripts suggest in diagnostics: empty
# for the default swarm, "PMIX_SWARM=<name> " otherwise, because compose reads
# the variable from the environment of the compose command itself.
SWARM_ENV=""
[ "$PMIX_SWARM" = pmix ] || SWARM_ENV="PMIX_SWARM=$PMIX_SWARM "

# --- driving a node ---------------------------------------------------------
# run a command on the head node (login env so PATH/LD_LIBRARY_PATH are set)
RUN() { docker exec -e PRTE_ALLOW_RUN_AS_ROOT=1 -e PRTE_ALLOW_RUN_AS_ROOT_CONFIRM=1 \
            "${NODE}1" bash -lc ". /opt/prte/env.sh; $*"; }
ON()  { docker exec "$NODE$1" bash -lc ". /opt/prte/env.sh 2>/dev/null; ${*:2}"; }
prted_count() { local c=0 n; for n in "$@"; do ON "$n" 'pgrep -x prted' >/dev/null 2>&1 && c=$((c+1)); done; echo "$c"; }

# Refuse to run against a swarm that is not up, and say which one we looked
# for: forgetting PMIX_SWARM on the compose command (compose interpolates
# docker-compose.yml, so it has to be in THAT command's environment) brings up
# the default swarm instead, and a bare "swarm not up" sends you looking for a
# docker problem you do not have.
swarm_up_or_die() {
    docker ps --format '{{.Names}}' | grep -qx "${NODE}1" && return 0
    echo "swarm '$PMIX_SWARM' not up -- no container named ${NODE}1" >&2
    echo "run: ${SWARM_ENV}docker compose up -d" >&2
    [ -z "$SWARM_ENV" ] || \
        echo "     (and ${SWARM_ENV}./build.sh first, so volume $VOLUME exists)" >&2
    exit 2
}

# --- cleaning a node --------------------------------------------------------
# What must not survive into the next test, on one node.  Each tool has its
# OWN session-dir prefix -- prte.<pid> for the HNP, prtrn.<pid> for prterun,
# prted.<pid> for a daemon standing on its own, prun.<pid> for prun itself,
# and ompi.<pid> for anything run under the ompi personality -- and every one
# of them holds a pmix.* server rendezvous file.  A system-level server drops
# one straight into the tmpdir as well.  Leaving any behind is what makes the
# next tool report "multiple possible servers ... connection handles have been
# read from files named pmix.*" and fail to find the DVM, so clear them all:
# the prefixes we know by name, and then whatever pmix* is still standing in
# the tmpdir or one level below it, which catches the session dir of a tool
# this list has not heard of.  The nodes leave TMPDIR unset, so that tmpdir is
# /tmp -- as every other path in this harness assumes.
#
# The tools are killed too, not just the daemons: a live prun or pterm is
# holding a rendezvous file of its own, and reaping it is the point of a
# teardown.
SWARM_CLEAN='
    for t in prted prte prterun prun pterm; do pkill -9 -x $t 2>/dev/null; done
    rm -rf /tmp/prte.* /tmp/prted.* /tmp/prtrn.* /tmp/prun.* /tmp/ompi.* \
           /tmp/pmix* 2>/dev/null
    find /tmp -maxdepth 2 -name "pmix*" -prune -exec rm -rf {} + 2>/dev/null
    true'
cleanup_swarm() {
    for n in $(seq 1 10); do
        docker exec "$NODE$n" sh -c "$SWARM_CLEAN"
    done
}

# --- macOS: keep this run inside its own clone ------------------------------
# The native subsets run on the developer's own machine, where another clone
# of openpmix may be running this very same script.  Nothing may reach a
# process or a file that clone owns, which takes both of:
#
#  - every tool and example is started by ABSOLUTE path out of THIS clone's
#    install ($MAC_BIN, never PATH), so "is this process mine" is a question
#    about the argv -- which is what mypgrep/mypkill ask.  A bare
#    `pkill -9 prterun` kills the other clone's launcher; a bare
#    `pgrep -x prte` is worse still, because it reports the other clone's DVM
#    as ours and the case then passes on a process this run never started.
#  - PRRTE gets a PRIVATE TMPDIR, so the session dirs -- and the pmix.*
#    rendezvous files inside them, which a -9'd tool never removes -- land
#    somewhere only this run will ever delete.  It is made under /tmp rather
#    than beside the build because the rendezvous socket path has to fit in
#    sun_path (104 bytes on Darwin) and the per-user $TMPDIR a Mac hands out
#    is already half of that.
#
# MAC_BIN/MAC_BRE/MAC_TMP are globals on purpose: the traps fire after the
# calling function has returned, and a local would be out of scope by then --
# which would silently leave the private directory behind on every run.
MAC_BIN=""
MAC_BRE=""
MAC_TMP=""
mac_isolate() {                 # $1 = install prefix whose bin/ we will drive
    MAC_BIN="$1/bin"
    MAC_BRE="$(printf '%s' "$MAC_BIN" | sed 's/[].[^$*+?(){}|\\]/\\&/g')"
    MAC_TMP="$(mktemp -d /tmp/pmixsuite.XXXXXX)" || {
        echo "cannot create a private TMPDIR under /tmp" >&2; exit 2; }
    export TMPDIR="$MAC_TMP"
    trap mac_done EXIT
    trap 'mac_done; exit 130' INT TERM
}
mypgrep() { pgrep -f "^$MAC_BRE/$1( |\$)" >/dev/null 2>&1; }
mypkill() { pkill -9 -f "^$MAC_BRE/$1( |\$)" 2>/dev/null; true; }
# Reap every process this run started -- the launcher and the example clients
# alike, since both were exec'd out of $MAC_BIN -- and the session dirs they
# left.  Session dirs only, not the whole private directory: a caller may keep
# a capture file there (mktemp honors TMPDIR) and read it after calling this.
# Everything named is unambiguously ours, pmix* included, because the
# directory it sits in is.
macpk() {
    pkill -9 -f "^$MAC_BRE/" 2>/dev/null
    rm -rf "${TMPDIR:?}"/prte.* "${TMPDIR:?}"/prted.* "${TMPDIR:?}"/prtrn.* \
           "${TMPDIR:?}"/prun.* "${TMPDIR:?}"/ompi.* "${TMPDIR:?}"/pmix* 2>/dev/null
    true
}
# However we leave, ^C included: the private dir holds only this run's session
# dirs, so there is nothing in it worth keeping.
mac_done() {
    [ -z "$MAC_BRE" ] || pkill -9 -f "^$MAC_BRE/" 2>/dev/null
    [ -z "$MAC_TMP" ] || rm -rf "$MAC_TMP"
    true
}
