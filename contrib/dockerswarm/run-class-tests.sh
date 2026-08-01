#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Run the src/class unit suite (test/unit/class) against your live openpmix
# tree, in the two configurations the in-tree `make check` on a developer's
# machine does not give you.
#
#   ./run-class-tests.sh linux    # in the swarm's containers
#   ./run-class-tests.sh macos    # natively, both configurations
#
# WHY THIS IS NOT A MULTI-NODE TEST
#
# Everything in src/class -- the object model, the list, the hash table, the
# pointer array, the hotel, the ring buffer, the value array, the bitmap --
# is a single-process, in-memory data structure.  Nothing in it talks to
# another node, so spreading it over ten containers would tell you nothing
# that running it once does not.  The distributed dimension of these classes
# is *cross-process, same node*: pmix_hash_table_t and pmix_pointer_array_t
# are TMA-aware precisely so gds/shmem3 can build them inside an mmap'd
# segment a server shares with its local clients.  That path is already
# covered by run-tests.sh, whose group cases drive a real datastore on each
# node -- not by anything a class unit test could do on its own.
#
# WHAT THE SWARM DOES BUY
#
# Two kinds of coverage the maintainer's own `make check` cannot produce:
#
#  1. Linux.  The primary development host here is macOS.  This code has
#     platform-sensitive corners -- the width of unsigned long under a shift,
#     C11 atomic codegen, what calloc(0, n) and realloc(p, 0) return, pthread
#     scheduling -- and the container is the nearest Linux userspace.
#
#  2. An OPTIMIZED build.  This is the bigger one.  Several guards in
#     src/class used to be compiled out unless --enable-debug, so the bugs
#     they now catch were only reachable in a build nobody tests: an
#     un-init'd hash table divided by zero, value_array's remove_item
#     memmove'd a wrapped size_t length.  Every tree in this workflow --
#     yours, and build.sh's -- configures --enable-debug.  So this script
#     configures a second, --disable-debug PMIx and runs the suite against
#     that too.  That second build is the entire reason this script exists.
#
# Prints PASS/FAIL per configuration and exits non-zero if anything failed.

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

# The program list comes out of the Makefile.am rather than being repeated
# here, so adding a class_* test to the suite does not silently leave it out
# of this run.
class_programs() {
    sed -n '/^check_PROGRAMS[[:space:]]*=/,/[^\\]$/p' \
        "$root/test/unit/class/Makefile.am" \
        | sed 's/^check_PROGRAMS[[:space:]]*=//; s/\\$//' \
        | tr -s ' \t\n' ' '
}

########################################################################
# Linux: build both configurations in a builder container, then run the
# optimized binaries on every node of the swarm.
########################################################################

test_linux() {
    local progs rc

    progs="$(class_programs)"
    if [ -z "${progs// /}" ]; then
        bad "could not read check_PROGRAMS from test/unit/class/Makefile.am"
        return
    fi
    echo "    suite: $progs"

    swarm_up_or_die

    if ! docker volume inspect "$VOLUME" >/dev/null 2>&1; then
        bad "build volume $VOLUME missing -- run ./build.sh first"
        return
    fi

    banner "build + run: --enable-debug (asserts and spot-checks live)"
    # Reuse the tree build.sh already configured; if it is not there, say so
    # rather than configuring a third one behind the user's back.
    docker run --rm \
        -v "$root":/pmix-src:ro \
        -v "$VOLUME":/opt/prte \
        -e PROGS="$progs" \
        "$IMAGE" bash -euo pipefail -c '
            cd /opt/prte/vpath-pmix 2>/dev/null || {
                echo "no /opt/prte/vpath-pmix -- run ./build.sh first" >&2; exit 2; }
            make -j"$(nproc)" -C test/unit/class check
            # Stage them so the nodes can run the same binaries.
            mkdir -p /opt/prte/tests-class/debug
            for p in $PROGS; do
                cp "test/unit/class/$p" /opt/prte/tests-class/debug/ 2>/dev/null || true
            done
        '
    rc=$?
    [ "$rc" = 0 ] && ok "debug build: suite green in the container" \
                  || bad "debug build: suite failed (rc=$rc)"

    banner "build + run: --disable-debug (the configuration nobody tests)"
    # A separate prefix and a separate VPATH directory: this library must not
    # displace the one the rest of the harness runs against.
    docker run --rm \
        -v "$root":/pmix-src:ro \
        -v "$VOLUME":/opt/prte \
        -e PROGS="$progs" \
        "$IMAGE" bash -euo pipefail -c '
            mkdir -p /opt/prte/vpath-pmix-opt && cd /opt/prte/vpath-pmix-opt
            [ -f config.status ] || /pmix-src/configure \
                --prefix=/opt/prte/pmix-opt --disable-debug --disable-devel-check
            # Two statements, not "make && make install": set -e does not fire
            # for a failing command in a non-final position of an && list, so
            # the joined form reports success over a stale install.
            make -j"$(nproc)"
            make install
            make -j"$(nproc)" -C test/unit/class check
            mkdir -p /opt/prte/tests-class/opt
            for p in $PROGS; do
                cp "test/unit/class/$p" /opt/prte/tests-class/opt/ 2>/dev/null || true
            done
        '
    rc=$?
    [ "$rc" = 0 ] && ok "optimized build: suite green in the container" \
                  || bad "optimized build: suite failed (rc=$rc)"

    # Running the staged binaries on the nodes is a load/contention pass, not
    # a distributed one: ten containers on one host share a kernel and a set
    # of CPUs.  It is worth the minute it costs for class_refcount, whose
    # whole subject is what several runnable threads do to one atomic, and it
    # confirms the binaries work under the same runtime the rest of the
    # harness uses.  It is not claimed to be more than that.
    banner "concurrent run across all ten nodes (contention pass)"
    for cfg in debug opt; do
        if ! ON 1 "test -d /opt/prte/tests-class/$cfg"; then
            skp "$cfg binaries not staged"
            continue
        fi
        local lib="/opt/prte/pmix/lib"
        [ "$cfg" = opt ] && lib="/opt/prte/pmix-opt/lib"

        local pids="" n bad_nodes=""
        rm -f /tmp/swarm-class-$cfg-*.out
        for n in $(seq 1 10); do
            ( docker exec "$NODE$n" bash -lc \
                "export LD_LIBRARY_PATH=$lib; \
                 cd /opt/prte/tests-class/$cfg && \
                 for p in $progs; do ./\$p >/dev/null 2>&1 || exit 1; done" \
              >/dev/null 2>&1; echo "$?" > "/tmp/swarm-class-$cfg-$n.out" ) &
            pids="$pids $!"
        done
        for p in $pids; do wait "$p"; done
        for n in $(seq 1 10); do
            [ "$(cat "/tmp/swarm-class-$cfg-$n.out" 2>/dev/null)" = 0 ] \
                || bad_nodes="$bad_nodes node$n"
        done
        rm -f /tmp/swarm-class-$cfg-*.out
        [ -z "$bad_nodes" ] && ok "$cfg: suite green on all ten nodes at once" \
                            || bad "$cfg: failed on$bad_nodes"
    done
}

########################################################################
# macOS: both configurations natively.  No containers involved -- the
# point here is still the optimized build, which the developer's own
# tree does not provide either.
########################################################################

test_macos() {
    local progs rc jobs opt="$root/vpath-macos-pmix-opt"

    progs="$(class_programs)"
    echo "    suite: $progs"
    jobs="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

    banner "in-tree build (whatever it is configured as)"
    if [ ! -f "$root/config.status" ]; then
        skp "no configured tree at $root -- run ./configure there first"
    else
        ( cd "$root" && make -j"$jobs" && make -C test/unit/class check ) \
            >/tmp/class-intree.$$ 2>&1
        rc=$?
        [ "$rc" = 0 ] && ok "in-tree: suite green" \
                      || { bad "in-tree: suite failed (rc=$rc)"; tail -30 /tmp/class-intree.$$; }
        rm -f /tmp/class-intree.$$
    fi

    banner "--disable-debug VPATH build (the configuration nobody tests)"
    # Autoconf refuses a VPATH build while the source directory itself holds
    # a config.status ("source directory already configured").  build.sh
    # resolves that by running "make distclean" on your tree; a test script
    # has no business doing that to a working build, so say what is in the
    # way instead of demolishing it.
    if [ -f "$root/config.status" ]; then
        skp "srcdir is configured in place -- an out-of-tree build cannot coexist"
        echo "     This half needs a source tree with no config.status in it," >&2
        echo "     which is the state ./build.sh macos leaves behind (it runs" >&2
        echo "     make distclean first).  Either run that, or:" >&2
        echo "         make -C $root distclean && $0 macos" >&2
        echo "     The Linux half has no such restriction -- ./run-class-tests.sh" >&2
        echo "     linux builds both configurations in the container." >&2
        return
    fi

    mkdir -p "$opt"
    ( cd "$opt" \
        && { [ -f config.status ] || "$root/configure" --prefix="$opt/install" \
                 --disable-debug --disable-devel-check; } \
        && make -j"$jobs" \
        && make -C test/unit/class check ) >/tmp/class-opt.$$ 2>&1
    rc=$?
    [ "$rc" = 0 ] && ok "optimized: suite green" \
                  || { bad "optimized: suite failed (rc=$rc)"; tail -30 /tmp/class-opt.$$; }
    rm -f /tmp/class-opt.$$
}

########################################################################

case "$mode" in
    linux) test_linux ;;
    macos) test_macos ;;
    *) echo "usage: $0 [linux|macos]" >&2; exit 2 ;;
esac

printf '\n=== summary: %d passed, %d failed, %d skipped ===\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ]
