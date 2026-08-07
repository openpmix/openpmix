#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Run the src/runtime suite -- library bring-up and tear-down, the
# progress-thread engine, the MCA parameter registry, the pmix_info support
# library -- against your live openpmix tree.
#
#   ./run-runtime-tests.sh linux    # in the swarm's containers
#   ./run-runtime-tests.sh macos    # natively, optimized configuration
#
# WHY THIS EXISTS
#
# Most of src/runtime is single-process and `make check` on the developer's
# own machine covers it.  Three things it does not:
#
#  1. LINUX-ONLY CODE THAT NEVER COMPILES AT HOME.  The progress thread's
#     CPU-affinity path -- start_progress_engine's parse_cpu_range and the
#     pthread_setaffinity_np call it feeds -- lives inside
#     #ifdef HAVE_PTHREAD_SETAFFINITY_NP.  macOS has no such function, so on
#     the primary development host that code is not merely untested, it is
#     not compiled.  test/unit/progress_threads reports its whole cpulist:
#     group as SKIP there.  Here it runs.
#
#     That path takes user input straight from an MCA parameter or the
#     PMIX_BIND_PROGRESS_THREAD attribute, and used to hand it to strtoul and
#     CPU_SET unchecked: strtoul reports 0 for a token with no digits in it,
#     so "cpu0" quietly became "bind to cpu 0", and a number past the end of
#     the mask is dropped by glibc but written past the end of it by BSD.
#     Stage 3 below drives that through the real entry point -- the shared
#     progress thread, from PMIx_server_init -- rather than the named threads
#     the unit test can reach.
#
#  2. AN OPTIMIZED BUILD.  Every tree in this workflow configures
#     --enable-debug.  start_progress_engine opens with assert(!trk->ev_active),
#     and pmix_info_close_components with an assert on its refcount; both
#     vanish under NDEBUG, which is the build users get.  A guard that only
#     exists in a configuration nobody ships is not a guard.
#
#  3. RE-ENTRANCY UNDER A REAL RUNTIME.  This directory's standing
#     requirement is that a second PMIx_Init starts from a clean slate, and
#     nearly all of its recent history is about that.  client_cycle and
#     tool_cycle cycle init/finalize a thousand times; running them on Linux
#     and optimized is where a leak or a stale latch that macOS/debug hides
#     shows up.
#
# WHAT THE MULTI-NODE STAGE IS, AND IS NOT
#
# Stage 4 is a guard, not a discovery test.  src/runtime is where every
# process's own node identity is settled: pmix_rte_init resolves the hostname
# (from PMIX_HOSTNAME, the environment, or the OS) and pmix_set_aliases
# records the form of it that the FQDN policy did not keep, so that
# pmix_check_local matches this node under either name.  That call used to be
# skipped whenever the host supplied the name -- which every resource manager
# does -- and the alias list came out empty.  Stage 4 runs a real job across
# real servers and asserts that every rank resolves its own node's peers,
# which is the cross-node consumer of that matching.
#
# It cannot exercise the FQDN half.  The swarm's containers are named node1..
# node10 with no domain part, so pmix_set_aliases has no second form to
# record and the interesting branch is never taken.  Engineering dotted
# hostnames into docker-compose.yml would ripple through every other runner
# here for one branch that test/unit/runtime_init already pins down directly
# (it hands PMIx_server_init an FQDN and checks both forms resolve).  So the
# FQDN case is deliberately left to the unit test; this stage guards the
# ordinary short-name path across nodes against the same change.
#
# Prints PASS/FAIL per stage and exits non-zero if anything failed.

set -uo pipefail

mode="${1:-linux}"
pass=0 fail=0 skip=0
ok()   { pass=$((pass+1)); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  \033[31mFAIL\033[0m %s\n' "$1"; }
skp()  { skip=$((skip+1)); printf '  \033[33mSKIP\033[0m %s\n' "$1"; }
banner() { printf '\n=== %s ===\n' "$1"; }

. "$(dirname "$0")/swarm-common.sh"

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"

# The programs in test/unit that belong to src/runtime.  Spelled out rather
# than read from Makefile.am: that file's check_PROGRAMS is the whole unit
# suite, and only these five are about this directory.
RUNTIME_PROGS="runtime_init progress_threads info_support client_cycle tool_cycle"

########################################################################
# Stages 1 and 2: build and run the suite in the container, in both
# configurations.
########################################################################

# This builds from a COPY of the source rather than a VPATH tree against the
# read-only mount, which is what the older runners here do.  The reason is
# worth stating, because the VPATH form looks cheaper and is not usable:
# autoconf refuses a VPATH configure outright while the source directory
# holds a config.status ("source directory already configured; run make
# distclean there first"), and a developer's own tree is configured in place.
# Worse, an already-configured VPATH tree does not escape it either -- touch
# any Makefile.am on the host and maintainer mode re-runs config.status in
# the container, which re-runs configure, which hits the same wall.  So the
# runner would work until the moment you edited something, which is the
# moment you want to run it.  Copying costs a full build per stage and is
# immune to whatever state the host tree is in.
#
# $1 = label, $2 = configure args, $3 = staging subdir
build_and_run() {
    local label="$1" cfgargs="$2" stage="$3" rc

    docker run --rm \
        -v "$root":/pmix-src:ro \
        -v "$VOLUME":/opt/prte \
        -e PROGS="$RUNTIME_PROGS" \
        -e CFGARGS="$cfgargs" \
        -e STAGE="$stage" \
        "$IMAGE" bash -euo pipefail -c '
            cp -a /pmix-src /work
            cd /work
            # The copy carries the host'"'"'s configure output and objects --
            # macOS ones, most likely.  Clear them before configuring here.
            make distclean >/dev/null 2>&1 || true
            rm -f config.status config.log
            ./autogen.pl > /tmp/autogen.log 2>&1 || { tail -20 /tmp/autogen.log; exit 1; }
            ./configure $CFGARGS > /tmp/configure.log 2>&1 \
                || { tail -30 /tmp/configure.log; exit 1; }
            make -j"$(nproc)" > /tmp/make.log 2>&1 || { tail -30 /tmp/make.log; exit 1; }
            # A fresh $TMPDIR: these programs stand up real PMIx servers, and
            # a leftover session directory from an earlier run makes them fail
            # for reasons that have nothing to do with the library.
            TMPDIR="$(mktemp -d)"; export TMPDIR
            for p in $PROGS; do
                make -C test/unit "$p" >> /tmp/make.log 2>&1 \
                    || { tail -30 /tmp/make.log; exit 1; }
            done
            ( cd test/unit && . ../pmix_test_env.sh
              for p in $PROGS; do
                  printf "    ---- %s ----\n" "$p"
                  ./"$p" 2>&1 | tail -3 || exit 1
              done )
            mkdir -p "$STAGE" "$STAGE/lib"
            for p in $PROGS; do
                # Stage the real ELF binary, not the libtool wrapper: an
                # uninstalled libtool target leaves a /bin/sh wrapper at
                # test/unit/$p and puts the executable under .libs/.
                cp "test/unit/.libs/$p" "$STAGE/" 2>/dev/null \
                    || cp "test/unit/$p" "$STAGE/" 2>/dev/null || true
            done
            # and the library they were linked against, so a node can run
            # them without depending on which PMIx the volume has installed
            cp -a src/.libs/libpmix.so* "$STAGE/lib/" 2>/dev/null || true
            cp -a src/mca/*/*/.libs/*.so "$STAGE/lib/" 2>/dev/null || true
        '
    rc=$?
    [ "$rc" = 0 ] && ok "$label: suite green in the container" \
                  || bad "$label: suite failed (rc=$rc)"
    return $rc
}

########################################################################
# Stage 3: the CPU-affinity path, through the real entry point.
#
# The unit test drives named progress threads, because those are the ones it
# can create and destroy at will.  The list is also read for the *shared*
# thread, straight out of pmix_rte_init, and that is the path a user actually
# takes -- so drive it here with the MCA parameter, against a program that
# stands up a real server.
#
# Both directions are asserted, and the second is the one that matters: with
# binding declared required, a list from which nothing usable can be
# extracted must make init fail rather than leave the thread silently bound
# to cpu 0, which is what an unchecked strtoul produced for any typo.
########################################################################

test_cpu_binding() {
    local stage="$1" out rc

    if ! ON 1 "test -x $stage/client_cycle"; then
        skp "cpu binding: binaries not staged"
        return
    fi

    # Two cycles is all that is needed: the binding happens during PMIx_Init,
    # and a second cycle proves the refusal did not wedge anything.
    local base="export LD_LIBRARY_PATH=$stage/lib; \
                export PMIX_MCA_mca_base_component_path=$stage/lib; \
                export TMPDIR=\$(mktemp -d); cd $stage;"

    # cpu 0 exists on every machine this can run on
    out="$(ON 1 "$base PMIX_MCA_pmix_progress_thread_cpus=0 \
                 PMIX_MCA_pmix_bind_progress_thread_reqd=1 ./client_cycle 2 2>&1")"
    rc=$?
    [ "$rc" = 0 ] && ok "cpu binding: a valid list is accepted" \
                  || { bad "cpu binding: valid list rejected (rc=$rc)"; echo "$out" | tail -5; }

    # a token with no digits in it must not be read as cpu 0
    out="$(ON 1 "$base PMIX_MCA_pmix_progress_thread_cpus=cpu0 \
                 PMIX_MCA_pmix_bind_progress_thread_reqd=1 ./client_cycle 2 2>&1")"
    rc=$?
    if [ "$rc" != 0 ]; then
        ok "cpu binding: a non-numeric list is refused when binding is required"
    else
        bad "cpu binding: 'cpu0' was accepted -- it is being read as cpu 0"
        echo "$out" | tail -5
    fi

    # and the diagnostic has to be the one about the list, not a bare
    # "failed to bind": a missing help topic used to turn every one of these
    # into "I couldn't find that help reference"
    case "$out" in
        *"could not be used"*|*"comma-delimited series"*)
            ok "cpu binding: the rejected entry is explained" ;;
        *"couldn't find that help reference"*)
            bad "cpu binding: help topic missing for the diagnostic" ;;
        *)
            skp "cpu binding: no diagnostic captured (init may have failed earlier)" ;;
    esac

    # an out-of-range cpu, likewise
    out="$(ON 1 "$base PMIX_MCA_pmix_progress_thread_cpus=99999999 \
                 PMIX_MCA_pmix_bind_progress_thread_reqd=1 ./client_cycle 2 2>&1")"
    [ $? != 0 ] && ok "cpu binding: an out-of-range cpu is refused" \
                || bad "cpu binding: cpu 99999999 was accepted"

    # not required, so a bad list must be a warning and not a failure -- the
    # thread runs unbound
    out="$(ON 1 "$base PMIX_MCA_pmix_progress_thread_cpus=cpu0 ./client_cycle 2 2>&1")"
    [ $? = 0 ] && ok "cpu binding: a bad list is survivable when not required" \
               || { bad "cpu binding: bad list aborted init without being required"
                    echo "$out" | tail -5; }
}

########################################################################

test_linux() {
    swarm_up_or_die

    if ! docker volume inspect "$VOLUME" >/dev/null 2>&1; then
        bad "build volume $VOLUME missing -- run ./build.sh first"
        return
    fi

    echo "    suite: $RUNTIME_PROGS"

    banner "stage 1: --enable-debug (asserts live; the affinity path compiles)"
    build_and_run "debug build" \
        "--prefix=/opt/prte/pmix-rt-dbg --enable-debug --enable-devel-check" \
        /opt/prte/tests-runtime/debug

    banner "stage 2: --disable-debug (the configuration users actually get)"
    build_and_run "optimized build" \
        "--prefix=/opt/prte/pmix-rt-opt --disable-debug --disable-devel-check" \
        /opt/prte/tests-runtime/opt

    banner "stage 3: progress-thread CPU binding through PMIx_Init"
    test_cpu_binding /opt/prte/tests-runtime/debug

    banner "stage 4: node identity across real servers"
    # NOTE: unlike stages 1-3, this one runs against the PMIx that ./build.sh
    # installed into the volume, because that is the library prted -- the
    # actual PMIx *server* here -- is linked against. Run ./build.sh against
    # your tree first or this stage reports on whatever was there before.
    # Every runner in this directory that drives prterun has the same
    # contract.
    #
    # simple_resolve asks the library to resolve the peers on its own node and
    # then the nodes of its job.  Run it spread over four nodes: each rank
    # must get an answer, which it only does if its server's idea of "this
    # node" matches the name the job was described with.
    local out rc
    # Build it in a docker run, not on a node: the nodes mount the build
    # volume READ-ONLY, so a compile there fails at the link step with
    # "cannot open output file ... Read-only file system".
    #
    # -rpath, not just -L: prted does not give its children a login shell, so
    # the LD_LIBRARY_PATH env.sh sets is not in scope for them and every rank
    # dies with "libpmix.so.2: cannot open shared object file".
    if ! docker run --rm \
            -v "$root":/pmix-src:ro \
            -v "$VOLUME":/opt/prte \
            "$IMAGE" bash -euo pipefail -c '
                mkdir -p /opt/prte/tests-runtime
                gcc -O0 -g -o /opt/prte/tests-runtime/simple_resolve \
                    /pmix-src/examples/simple_resolve.c \
                    -I/opt/prte/pmix/include -I/pmix-src/examples \
                    -L/opt/prte/pmix/lib -lpmix -Wl,-rpath,/opt/prte/pmix/lib
            '; then
        skp "resolve: could not build simple_resolve against the volume's PMIx"
        return
    fi

    out="$(RUN 'prterun --host node1:2,node2:2,node3:2,node4:2 -np 8 --map-by node \
                        --timeout 120 /opt/prte/tests-runtime/simple_resolve 2>&1')"
    rc=$?
    if [ "$rc" != 0 ]; then
        bad "resolve across four nodes: prterun failed (rc=$rc)"
        printf '%s\n' "$out" | tail -20
        return
    fi

    # One "ResPeers returned:" and one "ResNodes returned:" per rank - eight
    # of each. A rank whose server could not match its own node name against
    # the job description is the one that goes missing.
    local npeers nnodes bad_lines
    npeers="$(printf '%s\n' "$out" | grep -c 'ResPeers returned:')"
    nnodes="$(printf '%s\n' "$out" | grep -c 'ResNodes returned:')"
    [ "$npeers" = 8 ] && [ "$nnodes" = 8 ] \
        && ok "resolve: all eight ranks reported" \
        || bad "resolve: $npeers ResPeers / $nnodes ResNodes lines, expected 8 each"

    # Note the string is PMIx_Error_string's, which is "PMIX_SUCCESS" - not
    # "SUCCESS". Matching the short form silently passes on any output at all.
    bad_lines="$(printf '%s\n' "$out" | grep -E 'Res(Peers|Nodes)( global)? returned:' \
                 | grep -cv 'PMIX_SUCCESS')"
    [ "$bad_lines" = 0 ] \
        && ok "resolve: no rank failed to resolve its peers or nodes" \
        || { bad "resolve: $bad_lines resolve calls failed"
             printf '%s\n' "$out" | grep -E 'Res(Peers|Nodes)( global)? returned:' \
                 | grep -v 'PMIX_SUCCESS' | head -5; }

    # Every node named in the job has to come back in the node list. A node
    # whose own name did not match what it was told would drop out here.
    #
    # Pick the list out by its shape rather than by position. Eight ranks
    # are writing to the same stderr, so "the line after ResNodes" is
    # whatever happened to be flushed next -- which is how this read a
    # fragment of another rank's banner.
    local nodelist missing="" n
    nodelist="$(printf '%s\n' "$out" | tr -d ' \t' \
                | grep -Ex 'node[0-9]+(,node[0-9]+)*' | sort -u | head -1)"
    if [ -z "$nodelist" ]; then
        bad "resolve: no node list found in the output"
        printf '%s\n' "$out" | grep -A1 'ResNodes returned' | head -6
        return
    fi
    for n in node1 node2 node3 node4; do
        case ",$nodelist," in *",$n,"*) ;; *) missing="$missing $n" ;; esac
    done
    if [ -z "$missing" ]; then
        ok "resolve: all four nodes appear in the node list ($nodelist)"
    else
        bad "resolve: missing from node list:$missing (got '$nodelist')"
    fi
}

########################################################################
# macOS: the optimized configuration natively.  The affinity path still
# does not exist here -- that is stage 1/2's job in the container -- but
# the rest of the suite gains the build users get.
########################################################################

test_macos() {
    local rc jobs opt="$root/vpath-macos-pmix-opt" p

    jobs="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
    echo "    suite: $RUNTIME_PROGS"
    echo "    note: the progress-thread affinity path is absent on macOS;"
    echo "          run '$0 linux' for it."

    banner "in-tree build (whatever it is configured as)"
    if [ ! -f "$root/config.status" ]; then
        skp "no configured tree at $root -- run ./configure there first"
    else
        ( cd "$root" && make -j"$jobs" \
          && for p in $RUNTIME_PROGS; do make -C test/unit "$p" || exit 1; done \
          && cd test/unit && . ../pmix_test_env.sh \
          && export TMPDIR="$(mktemp -d)" \
          && for p in $RUNTIME_PROGS; do ./"$p" || exit 1; done ) \
            >/tmp/runtime-intree.$$ 2>&1
        rc=$?
        [ "$rc" = 0 ] && ok "in-tree: suite green" \
                      || { bad "in-tree: suite failed (rc=$rc)"; tail -30 /tmp/runtime-intree.$$; }
        rm -f /tmp/runtime-intree.$$
    fi

    banner "--disable-debug VPATH build (the configuration nobody tests)"
    # Autoconf refuses a VPATH build while the source directory itself holds
    # a config.status ("source directory already configured").  A test script
    # has no business running distclean on a working tree, so say what is in
    # the way instead.
    if [ -f "$root/config.status" ]; then
        skp "srcdir is configured in place -- an out-of-tree build cannot coexist"
        echo "     Either run './build.sh macos' (which distcleans first), or:" >&2
        echo "         make -C $root distclean && $0 macos" >&2
        echo "     The Linux half has no such restriction." >&2
        return
    fi

    mkdir -p "$opt"
    ( cd "$opt" \
        && { [ -f config.status ] || "$root/configure" --prefix="$opt/install" \
                 --disable-debug --disable-devel-check; } \
        && make -j"$jobs" \
        && for p in $RUNTIME_PROGS; do make -C test/unit "$p" || exit 1; done \
        && cd test/unit && . ../pmix_test_env.sh \
        && export TMPDIR="$(mktemp -d)" \
        && for p in $RUNTIME_PROGS; do ./"$p" || exit 1; done ) >/tmp/runtime-opt.$$ 2>&1
    rc=$?
    [ "$rc" = 0 ] && ok "optimized: suite green" \
                  || { bad "optimized: suite failed (rc=$rc)"; tail -30 /tmp/runtime-opt.$$; }
    rm -f /tmp/runtime-opt.$$
}

########################################################################

case "$mode" in
    linux) test_linux ;;
    macos) test_macos ;;
    *) echo "usage: $0 [linux|macos]" >&2; exit 2 ;;
esac

printf '\n=== summary: %d passed, %d failed, %d skipped ===\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ]
