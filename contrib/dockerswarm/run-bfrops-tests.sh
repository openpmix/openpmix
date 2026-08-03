#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Exercise src/mca/bfrops -- the serialization engine -- in the two ways a
# developer's own `make check` cannot.
#
#   ./run-bfrops-tests.sh linux    # in the 10-container swarm
#                                  #   (requires: ./build.sh && docker compose up -d)
#   ./run-bfrops-tests.sh macos    # the single-host subset, natively
#
# WHY THIS ONE IS PARTLY A MULTI-NODE TEST
#
# bfrops is the only framework in the tree whose whole reason for existing
# is that two processes have to agree.  Half of it can be checked in one
# process -- test/unit/bfrops_darray.c and test/unit/bfrops_malformed.c do
# that, and the first two stages below just run them where a Mac developer
# does not: on Linux, in an optimized build, and with --enable-mca-dso so
# every component is a real dlopen'd plugin.
#
# The other half cannot.  Two things about a pack are decided by the *peer*,
# not by the process doing the packing:
#
#   * WHICH MODULE.  PMIX_BFROPS_PACK dereferences peer->nptr->compat.bfrops,
#     the module pmix_bfrops_base_assign_module() picked from the version
#     string the ptl handshake exchanged.  In a single process that is
#     always this build's newest component, so the assignment path is never
#     really taken.
#   * WHICH BUFFER TYPE.  Described vs. non-described is negotiated at
#     connection time, and the pack and unpack drivers branch on it to
#     decide whether each item carries a type tag.  A round trip inside one
#     process agrees with itself under either, so it cannot fail on this.
#
# A round trip inside one process is self-consistent under both and so
# proves neither.  The `datatypes` stage spreads ranks over separate nodes,
# hence separate prted/PMIx servers: a value rank N asks for is not in its
# local datastore, so the get takes the full pack -> send -> unpack -> store
# path across two daemons.  examples/datatypes.c publishes one value of
# every interesting type -- including nested data arrays and the typeless
# array descriptor whose marker pack and unpack must spell identically --
# and every rank verifies every other rank's copy.
#
# Prints PASS/FAIL per stage and exits non-zero if anything failed.

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

PMIX_PREFIX=/opt/prte/pmix

# The single-process programs this runner stages and runs.  Named here
# rather than read out of the Makefile.am because test/unit holds forty
# programs and only these two are about bfrops.
BFROPS_PROGRAMS="bfrops_darray bfrops_malformed bfrops_get_number bfrops_null_object bfrops_helpers bfrops_regex2 bfrops_alloc_inherit nested_darray"

# An out-of-tree build cannot coexist with a config.status in the srcdir --
# autoconf refuses with "source directory already configured".  Say so once,
# plainly, rather than letting the VPATH stages fail with an autoconf error
# that names no way out.
srcdir_blocks_vpath() {
    [ -f "$root/config.status" ] || return 1
    echo "     $root holds a config.status, so autoconf will refuse the" >&2
    echo "     VPATH build this script needs.  Either:" >&2
    echo "         make -C $root distclean && $0 $mode" >&2
    echo "     or run ./build.sh, which distcleans the tree for you." >&2
    return 0
}

########################################################################
# Linux
########################################################################

test_linux() {
    local rc out np hosts

    swarm_up_or_die

    if ! docker volume inspect "$VOLUME" >/dev/null 2>&1; then
        bad "build volume $VOLUME missing -- run ./build.sh first"
        return
    fi

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

    if srcdir_blocks_vpath; then
        skp "srcdir is configured in place -- no VPATH build can be made from it"
    else
        banner "unit suite in the tree build.sh configured (static components)"
        docker run --rm \
            -v "$root":/pmix-src:ro \
            -v "$VOLUME":/opt/prte \
            -e PROGS="$BFROPS_PROGRAMS" \
            "$IMAGE" bash -euo pipefail -c '
                cd /opt/prte/vpath-pmix 2>/dev/null || {
                    echo "no /opt/prte/vpath-pmix -- run ./build.sh first" >&2; exit 2; }
                make -j"$(nproc)" -C test/unit $PROGS
                mkdir -p /opt/prte/tests-bfrops/static
                for p in $PROGS; do
                    # Stage the real ELF binary, not the libtool wrapper: an
                    # uninstalled libtool target leaves a /bin/sh wrapper in
                    # test/unit and the executable under .libs/.
                    cp "test/unit/.libs/$p" /opt/prte/tests-bfrops/static/ 2>/dev/null \
                        || cp "test/unit/$p" /opt/prte/tests-bfrops/static/ 2>/dev/null || true
                done
                cd test/unit && for p in $PROGS; do ./$p >/dev/null; done
            '
        rc=$?
        [ "$rc" = 0 ] && ok "static build: bfrops unit programs green in the container" \
                      || bad "static build: bfrops unit programs failed (rc=$rc)"

        banner "--enable-mca-dso build (every component a real plugin)"
        # A separate prefix and VPATH directory: this library must not
        # displace the one the rest of the harness runs against.
        docker run --rm \
            -v "$root":/pmix-src:ro \
            -v "$VOLUME":/opt/prte \
            -e PROGS="$BFROPS_PROGRAMS" \
            "$IMAGE" bash -euo pipefail -c '
                mkdir -p /opt/prte/vpath-pmix-bfrops-dso
                cd /opt/prte/vpath-pmix-bfrops-dso
                [ -f config.status ] || /pmix-src/configure \
                    --prefix=/opt/prte/pmix-bfrops-dso --enable-mca-dso \
                    --disable-debug --disable-devel-check
                make -j"$(nproc)"
                make install
                make -j"$(nproc)" -C test/unit $PROGS
                mkdir -p /opt/prte/tests-bfrops/dso
                for p in $PROGS; do
                    cp "test/unit/.libs/$p" /opt/prte/tests-bfrops/dso/ 2>/dev/null \
                        || cp "test/unit/$p" /opt/prte/tests-bfrops/dso/ 2>/dev/null || true
                done
                cd test/unit && for p in $PROGS; do ./$p >/dev/null; done
            '
        rc=$?
        [ "$rc" = 0 ] && ok "mca-dso build: bfrops unit programs green in the container" \
                      || bad "mca-dso build: bfrops unit programs failed (rc=$rc)"
    fi

    # ------------------------------------------------------------------
    # The part that needs more than one node.
    # ------------------------------------------------------------------
    banner "preflight: PMIx installed in the shared volume"
    if ON 1 "test -f $PMIX_PREFIX/include/pmix_common.h"; then
        ok "PMIx headers/libs under $PMIX_PREFIX"
    else
        bad "no $PMIX_PREFIX in the volume -- run ./build.sh first"
        return
    fi

    banner "build the cross-node data-type client"
    docker run --rm \
        -v "$root":/pmix-src:ro \
        -v "$VOLUME":/opt/prte \
        -e PFX="$PMIX_PREFIX" \
        "$IMAGE" bash -euo pipefail -c '
            mkdir -p /opt/prte/tests-bfrops
            gcc -O0 -g -o /opt/prte/tests-bfrops/datatypes \
                /pmix-src/examples/datatypes.c \
                -I"$PFX/include" -I/pmix-src/examples \
                -L"$PFX/lib" -lpmix -Wl,-rpath,"$PFX/lib"
        '
    rc=$?
    if [ "$rc" != 0 ]; then
        bad "could not build examples/datatypes (rc=$rc)"
        return
    fi
    ok "built examples/datatypes"

    if ! RUN 'command -v prterun >/dev/null'; then
        skp "no prterun in the containers -- run ./build.sh first"
        return
    fi

    # One rank per node, so *every* get crosses a server boundary. With two
    # ranks on a node the local pair would be answered out of the local
    # datastore and the cross-node half could pass while the wire half was
    # broken; --map-by node with one slot each removes that hiding place.
    for geom in "node1:1,node2:1 2" "node1:1,node2:1,node3:1,node4:1 4" \
                "node1:1,node2:1,node3:1,node4:1,node5:1,node6:1,node7:1,node8:1 8"; do
        hosts="${geom% *}"
        np="${geom##* }"
        cleanup_swarm
        banner "every data type across $np separate PMIx servers"
        out="$(RUN "prterun --host $hosts -np $np --map-by node --timeout 120 \
                    /opt/prte/tests-bfrops/datatypes 2>&1")"
        rc=$?
        if echo "$out" | grep -qiE 'time limit for job|timed out|DVM timeout'; then
            bad "$np nodes: HUNG (hit the launch timeout)"
        elif echo "$out" | grep -qiE 'MISMATCH|Segmentation|core dumped'; then
            bad "$np nodes: $(echo "$out" | grep -iE 'MISMATCH|Segmentation|core dumped' | head -1)"
        elif [ "$(echo "$out" | grep -c 'datatypes: rank .*: PASS')" != "$np" ]; then
            # Every rank has to say so. Counting them is what stops a run in
            # which some ranks never got that far from reading as a pass.
            bad "$np nodes: only $(echo "$out" | grep -c 'datatypes: rank .*: PASS')/$np ranks reported PASS"
        elif [ "$rc" != 0 ]; then
            bad "$np nodes: exit $rc"
        else
            ok "$np nodes: every type survived the wire on all $np ranks"
        fi
    done
}

########################################################################
# macOS: the single-process half only. There is one node, so there is no
# cross-server half to run.
########################################################################

test_macos() {
    local p rc

    banner "bfrops unit programs in the local tree"
    if [ ! -f "$root/config.status" ]; then
        skp "no configured tree at $root"
        return
    fi
    for p in $BFROPS_PROGRAMS; do
        make -C "$root/test/unit" "$p" >/dev/null 2>&1
        if [ ! -x "$root/test/unit/$p" ]; then
            bad "$p did not build"
            continue
        fi
        ( cd "$root/test/unit" && TMPDIR="$(mktemp -d)" "./$p" >/dev/null 2>&1 )
        rc=$?
        [ "$rc" = 0 ] && ok "$p" || bad "$p (rc=$rc)"
    done

    banner "every data type through a single simptest server"
    # Not a substitute for the swarm stage -- one server means one peer
    # module and one negotiated buffer type -- but it does drive the same
    # client end to end, which catches an outright break before you spend
    # ten containers on it.
    make -C "$root/examples" datatypes >/dev/null 2>&1
    make -C "$root/test/simple" >/dev/null 2>&1
    if [ ! -x "$root/test/simple/run_simptest.sh" ] || [ ! -x "$root/examples/datatypes" ]; then
        skp "simptest or examples/datatypes not built"
        return
    fi
    local out
    out="$( cd "$root/test/simple" \
            && TMPDIR="$(mktemp -d)" ./run_simptest.sh -n 4 -e ../../examples/datatypes 2>&1 )"
    if [ "$(echo "$out" | grep -c 'datatypes: rank .*: PASS')" = 4 ]; then
        ok "4 ranks, one server: every type survived"
    else
        bad "simptest run: $(echo "$out" | grep -iE 'MISMATCH|FAIL' | head -1)"
    fi

    echo
    echo "  NOTE: the cross-server half of this suite needs the swarm."
    echo "        Run './run-bfrops-tests.sh linux' for that."
}

########################################################################

banner "src/mca/bfrops: serialization across builds and across nodes"
case "$mode" in
    linux) test_linux ;;
    macos) test_macos ;;
    *) echo "usage: $0 [linux|macos]" >&2; exit 2 ;;
esac

printf '\n=== summary: %d passed, %d failed, %d skipped ===\n' "$pass" "$fail" "$skip"
[ "$fail" = 0 ]
