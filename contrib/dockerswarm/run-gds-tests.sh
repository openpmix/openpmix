#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Exercise src/mca/gds -- the datastore -- in the ways a developer's own
# `make check` cannot.
#
#   ./run-gds-tests.sh linux    # in the 10-container swarm
#                               #   (requires: ./build.sh && docker compose up -d)
#   ./run-gds-tests.sh macos    # the single-host subset, natively
#
# WHY THIS SUITE EXISTS
#
# Two distinct gaps, and they are not the same gap.
#
#   1. gds/shmem3 IS NOT BUILT ON macOS AT ALL.  Its configure.m4 gates on
#      a 64-bit, non-Apple host, and unlike the other environment-specific
#      components it carries no "|| test $pmix_testbuild = 1" escape, so
#      --enable-test-build does not reach it either.  A developer working
#      on a Mac gets no compiler coverage of that component -- not a
#      weaker test, none -- and the first thing this runner does is
#      compile it.  Everything downstream of that is a bonus.
#
#   2. THE DATASTORE'S JOB IS TO ANSWER FOR PROCESSES THAT ARE NOT HERE.
#      A rank asking for a value its own node already holds is served out
#      of the local hash tables and never touches the modex machinery at
#      all.  So a single-node run of any of these programs is
#      self-consistent under a datastore that drops every remote proc, and
#      that is not a hypothetical failure mode: shmem3's store_modex
#      callback returned the unpack "end of buffer" code where the base
#      envelope walker expects PMIX_SUCCESS, so it stored the *first* proc
#      of each server's contribution and silently discarded the rest --
#      while reporting success.  Nothing in test/unit could see it.
#
# Hence one rank per node (--map-by node with one slot each) everywhere
# below: with two ranks on a node, half the gets are answered locally and
# the run can be green with the remote half broken.
#
# WHAT EACH STAGE COVERS
#
#   compile        shmem3 actually builds, warning-free, and is wired into
#                  static-components.h
#   unit           test/unit's gds programs on Linux, in the tree build.sh
#                  configured and again with --enable-mca-dso where the gds
#                  components are real dlopen'd plugins
#   collective     examples/datatypes.c across N separate PMIx servers:
#                  put, commit, fence with PMIX_COLLECT_DATA, then verify
#                  every peer's values.  This is the modex path -- the one
#                  the truncation bug above sat in.  Run three times over
#                  the same geometry, under a different gds configuration
#                  each time (below).
#   hash           the same run with the clients forced onto gds/hash, so
#                  both shipped components are covered and a divergence
#                  between them shows up as one passing and one failing.
#   direct         examples/dmodex.c: commit without a collective fence, so
#                  each get is answered by the owning server on demand.
#                  That is the assemb_kvs_req / accept_kvs_resp path -- the
#                  slots shmem3 leaves NULL and routes to the local module.
#   fallback       the collective run again with
#                  gds_shmem3_force_client_attach_failure=1, so every
#                  client's fixed-address attach fails and it must switch
#                  to the next module and re-request its job data.  A
#                  regression here is a failed PMIx_Init, not a wrong
#                  answer.
#
# WHAT IT DELIBERATELY DOES NOT COVER, AND ONE OVERLAP
#
# examples/datatypes is also driven cross-node by run-bfrops-tests.sh.
# That is the same program with a different subject: there it is asked
# whether every data type survives the encoders, here whether the
# datastore keeps and returns what it was given, which is why this runner
# repeats the geometry under three gds configurations and that one does
# not.  If you are changing datatypes.c, both suites are downstream.
#
# examples/client.c is NOT used, and should not be added.  It asks for
# PMIX_LOCAL_PROCS, which no part of PMIx stores -- a host is expected to
# supply it and PRRTE does not -- and on the resulting PMIX_ERR_NOT_FOUND
# it jumps over the entire put/fence/get section it exists for and returns
# 0.  It reports success having tested nothing.
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

# The test/unit programs that are about gds. Named here rather than read
# out of Makefile.am because that file holds forty programs and only these
# three are ours.
GDS_PROGRAMS="gds_datastore gds_fallback proc_array_id"

# Geometries: one slot per host so every get crosses a server boundary.
GEOMETRIES="node1:1,node2:1|2 node1:1,node2:1,node3:1,node4:1|4 node1:1,node2:1,node3:1,node4:1,node5:1,node6:1,node7:1,node8:1|8"

# The build stages below copy the source into the container and build it
# there, rather than configuring a VPATH tree against the read-only mount
# the way run-bfrops-tests.sh does.  That costs one autogen per run, and
# buys not caring whether the developer's srcdir has been configured in
# place -- which most have, and which blocks a VPATH build two different
# ways (autoconf refuses to configure alongside a config.status, and stale
# objects in the srcdir shadow the build tree through VPATH).  Since the
# headline stage here is "does shmem3 compile at all", skipping it on the
# ordinary developer configuration would defeat the runner.

# Run one client program across $np separate servers and grade the result.
# $4 is a space-separated list of VAR=VALUE to place in both the launch
# environment and the app environment; $5 is a label for the message.
#
# Both environments are needed and they are not the same thing: an MCA
# parameter read by the *client* library has to reach the app process
# (prterun -x), while one that has to reach the PMIx server inside each
# prted has to be in the environment prterun itself was started with,
# because that is what PRRTE forwards to the daemons it launches.
#
# Grading differs per program because the two report differently:
#
#   datatypes prints "datatypes: rank N: PASS" per rank and exits non-zero
#   on a mismatch, so we require exactly $np of those lines.
#
#   dmodex has no per-rank success line and returns 0 even when it jumps
#   over its checks, so the only usable signal is the absence of its
#   failure lines together with rank 0's finalize line. Grade it that way
#   rather than on the exit status, which proves nothing here.
run_across_nodes() {
    local prog="$1" hosts="$2" np="$3" envs="${4:-}" label="${5:-}"
    local out rc nok xargs exports v

    xargs=""; exports=""
    for v in $envs; do
        xargs="$xargs -x ${v%%=*}"
        exports="$exports export $v;"
    done

    cleanup_swarm
    out="$(RUN "$exports prterun --host $hosts -np $np --map-by node $xargs \
                --timeout 120 /opt/prte/tests-gds/$prog 2>&1")"
    rc=$?

    if echo "$out" | grep -qiE 'time limit for job|timed out|DVM timeout'; then
        bad "$prog $label, $np servers: HUNG (hit the launch timeout)"
        return 1
    fi
    if echo "$out" | grep -qiE 'Segmentation|core dumped|Bus error'; then
        bad "$prog $label, $np servers: $(echo "$out" | grep -iE 'Segmentation|core dumped|Bus error' | head -1)"
        return 1
    fi

    if [ "$prog" = datatypes ]; then
        nok=$(echo "$out" | grep -c 'datatypes: rank .*: PASS')
        if [ "$nok" != "$np" ]; then
            bad "$prog $label, $np servers: only $nok/$np ranks reported PASS"
            echo "$out" | grep -iE 'failed|wrong|mismatch|MISMATCH' | head -3 | sed 's/^/       /'
            return 1
        fi
        if [ "$rc" != 0 ]; then
            bad "$prog $label, $np servers: every rank passed but exit was $rc"
            return 1
        fi
    else
        if echo "$out" | grep -qiE 'PMIx_Get .* failed|returned wrong'; then
            bad "$prog $label, $np servers: $(echo "$out" | grep -iE 'PMIx_Get .* failed|returned wrong' | head -1)"
            return 1
        fi
        if ! echo "$out" | grep -q 'PMIx_Finalize successfully completed'; then
            bad "$prog $label, $np servers: no rank reached finalize"
            return 1
        fi
    fi
    return 0
}

########################################################################
# Linux
########################################################################

test_linux() {
    local rc out hosts np geom imgid cimg imgn stalenodes

    swarm_up_or_die

    if ! docker volume inspect "$VOLUME" >/dev/null 2>&1; then
        bad "build volume $VOLUME missing -- run ./build.sh first"
        return
    fi

    banner "preflight: containers are running the current image"
    stalenodes=""
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

    # ------------------------------------------------------------------
    # 1. The coverage a Mac developer does not have at all.
    #
    # One container, two trees, so autogen.pl runs once: a default build
    # (the configuration CI and most developers use) and an --enable-mca-dso
    # build (where the gds components are real dlopen'd plugins rather than
    # being linked into libpmix).  Both are configured --enable-devel-check,
    # so a warning anywhere in the tree is an error and this stage fails on
    # it rather than having to grep for one.
    # ------------------------------------------------------------------
    banner "shmem3 builds on Linux, and the gds unit programs pass there"
    out=$(docker run --rm \
        -v "$root":/pmix-src:ro \
        -e PROGS="$GDS_PROGRAMS" \
        "$IMAGE" bash -euo pipefail -c '
            mkdir -p /src
            (cd /pmix-src && tar cf - --exclude=.git .) | (cd /src && tar xf -)
            cd /src
            # Scrub whatever the developer built in place. Most trees have
            # been built in the srcdir, and the leftovers are not merely
            # untidy: make resolves a missing prerequisite through VPATH,
            # finds the srcdir object, and hands libtool a .lo built for a
            # different tree. The error is
            #   libtool: error: 'pmix_globals.lo' is not a valid libtool object
            # which names neither VPATH nor the srcdir. autoconf also
            # refuses outright to configure a VPATH tree while the srcdir
            # holds a config.status, so that goes too.
            find /src \( -name '*.o'  -o -name '*.lo' -o -name '*.la' \
                       -o -name '*.Plo' -o -name '*.Po' -o -name '*.a' \
                       -o -name 'Makefile' \) -delete
            find /src \( -name .libs -o -name .deps \) -type d -prune -exec rm -rf {} +
            rm -f /src/config.status /src/config.log /src/libtool
            ./autogen.pl >/tmp/autogen.log 2>&1 || { tail -30 /tmp/autogen.log; exit 3; }

            for cfg in "default:--enable-devel-check" \
                       "mca-dso:--enable-devel-check --enable-mca-dso"; do
                name="${cfg%%:*}"; opts="${cfg#*:}"
                mkdir -p /build-$name && cd /build-$name
                /src/configure --prefix=/inst-$name $opts >/tmp/conf-$name.log 2>&1 \
                    || { tail -40 /tmp/conf-$name.log; exit 4; }
                make -j"$(nproc)" >/tmp/make-$name.log 2>&1 \
                    || { echo "BUILD-FAILED ($name)"; tail -40 /tmp/make-$name.log; exit 6; }
                test -s src/mca/gds/shmem3/gds_shmem3.lo \
                    || { echo "NOT-BUILT: no shmem3 objects ($name)"; exit 7; }
                # A component that compiles but is not wired in is
                # invisible at run time, and the two configurations wire
                # them differently: a default build lists each component in
                # the generated static-components.h, while --enable-mca-dso
                # produces a loadable plugin per component and an EMPTY
                # static list. Checking static-components.h in the dso build
                # therefore fails for the one reason that is not a defect.
                if [ "$name" = mca-dso ]; then
                    ls src/mca/gds/shmem3/.libs/*gds_shmem3*.so >/dev/null 2>&1 \
                        || { echo "NOT-WIRED: no shmem3 plugin built ($name)"; exit 5; }
                else
                    grep -q shmem3 src/mca/gds/base/static-components.h \
                        || { echo "NOT-WIRED: shmem3 missing from static-components.h ($name)"; exit 5; }
                fi
                make -j"$(nproc)" -C test/unit $PROGS >/dev/null 2>&1 \
                    || { echo "TESTBUILD-FAILED ($name)"; exit 8; }
                # Source the generated test environment before running
                # anything. The MCA base searches the *install* prefix for
                # components, so a program run straight out of the build
                # tree finds none -- fatal in the --enable-mca-dso build,
                # where every component is a plugin, and it fails as "no
                # usable plugins for the BFROPS framework", which names
                # neither the build tree nor the missing search path. This
                # is the same file "make check" pulls in through
                # AM_TESTS_ENVIRONMENT.
                ( . ./test/pmix_test_env.sh
                  cd test/unit && for prog in $PROGS; do
                      TMPDIR=$(mktemp -d) ./$prog >/tmp/$prog-$name.out 2>&1 \
                          || { echo "UNIT-FAILED: $prog ($name)"; cat /tmp/$prog-$name.out; exit 9; }
                  done )
                echo "STAGE-OK $name"
                cd /
            done
        ' 2>&1)
    rc=$?
    if [ "$rc" != 0 ]; then
        bad "Linux build stage failed (rc=$rc)"
        echo "$out" | tail -25 | sed 's/^/       /'
    else
        if [ "$(echo "$out" | grep -c '^STAGE-OK ')" = 2 ]; then
            ok "shmem3 builds warning-free and is wired in (default and mca-dso)"
            ok "gds unit programs pass on Linux in both configurations"
        else
            bad "not every configuration completed"
            echo "$out" | tail -25 | sed 's/^/       /'
        fi
    fi

    # ------------------------------------------------------------------
    # 2. The part that needs more than one node.
    # ------------------------------------------------------------------
    banner "preflight: PMIx installed in the shared volume"
    if ON 1 "test -f $PMIX_PREFIX/include/pmix_common.h"; then
        ok "PMIx headers/libs under $PMIX_PREFIX"
    else
        bad "no $PMIX_PREFIX in the volume -- run ./build.sh first"
        return
    fi

    banner "build the cross-node clients"
    docker run --rm \
        -v "$root":/pmix-src:ro \
        -v "$VOLUME":/opt/prte \
        -e PFX="$PMIX_PREFIX" \
        "$IMAGE" bash -euo pipefail -c '
            mkdir -p /opt/prte/tests-gds
            for p in datatypes dmodex; do
                gcc -O0 -g -o /opt/prte/tests-gds/$p /pmix-src/examples/$p.c \
                    -I"$PFX/include" -I/pmix-src/examples \
                    -L"$PFX/lib" -lpmix -Wl,-rpath,"$PFX/lib"
            done
        '
    rc=$?
    if [ "$rc" != 0 ]; then
        bad "could not build examples/datatypes and examples/dmodex (rc=$rc)"
        return
    fi
    ok "built examples/datatypes and examples/dmodex"

    if ! RUN 'command -v prterun >/dev/null'; then
        skp "no prterun in the containers -- run ./build.sh first"
        return
    fi

    banner "collective modex across separate PMIx servers (default gds)"
    for geom in $GEOMETRIES; do
        hosts="${geom%|*}"; np="${geom#*|}"
        run_across_nodes datatypes "$hosts" "$np" "" "(default)" \
            && ok "$np servers: every rank read every peer's values"
    done

    banner "the same, with everything forced onto gds/hash"
    for geom in $GEOMETRIES; do
        hosts="${geom%|*}"; np="${geom#*|}"
        run_across_nodes datatypes "$hosts" "$np" "PMIX_MCA_gds=hash" "(hash)" \
            && ok "$np servers on hash: every rank read every peer's values"
    done

    banner "direct modex (assemb_kvs_req / accept_kvs_resp)"
    # dmodex commits without a collective fence, so each get is satisfied
    # by the owning proc's server on demand rather than out of an
    # already-distributed blob. shmem3 leaves both of those module slots
    # NULL and relies on the framework macros routing them to the local
    # module -- this is the stage that proves that fallback works.
    for geom in $GEOMETRIES; do
        hosts="${geom%|*}"; np="${geom#*|}"
        run_across_nodes dmodex "$hosts" "$np" "" "" \
            && ok "$np servers: direct modex answered every request"
    done

    banner "the GDS fallback path (forced attach failure)"
    # Every client's fixed-address segment attach is made to fail, so each
    # one has to notice, switch to the next active module, and re-request
    # its job data through PMIX_GDS_FALLBACK_CMD. The answers still have to
    # be right afterwards. Where shmem3 is not the assigned module this is
    # simply the default run again, which is harmless.
    for geom in $GEOMETRIES; do
        hosts="${geom%|*}"; np="${geom#*|}"
        run_across_nodes datatypes "$hosts" "$np" \
            "PMIX_MCA_gds_shmem3_force_client_attach_failure=1" "(fallback)" \
            && ok "$np servers: clients fell back and still read every peer"
    done
}

########################################################################
# macOS: the single-process half only. shmem3 does not exist here.
########################################################################

test_macos() {
    local p rc out

    banner "note on coverage"
    echo "  gds/shmem3 is not compiled on this platform (configure.m4 gates"
    echo "  on a 64-bit non-Apple host, with no --enable-test-build escape),"
    echo "  so nothing below touches it. Run './run-gds-tests.sh linux'."

    banner "gds unit programs in the local tree"
    if [ ! -f "$root/config.status" ]; then
        skp "no configured tree at $root"
        return
    fi
    for p in $GDS_PROGRAMS; do
        make -C "$root/test/unit" "$p" >/dev/null 2>&1
        if [ ! -x "$root/test/unit/$p" ]; then
            bad "$p did not build"
            continue
        fi
        ( cd "$root/test/unit" && TMPDIR="$(mktemp -d)" "./$p" >/dev/null 2>&1 )
        rc=$?
        [ "$rc" = 0 ] && ok "$p" || bad "$p (rc=$rc)"
    done

    banner "collective modex through a single simptest server"
    # Not a substitute for the swarm stage -- one server means every get is
    # answered out of the local datastore, which is precisely the case the
    # multi-node stages exist to get past -- but it does drive the store and
    # fetch paths end to end and catches an outright break cheaply.
    make -C "$root/examples" datatypes >/dev/null 2>&1
    make -C "$root/test/simple" >/dev/null 2>&1
    if [ ! -x "$root/test/simple/run_simptest.sh" ] || [ ! -x "$root/examples/datatypes" ]; then
        skp "simptest or examples/datatypes not built"
        return
    fi
    out="$( cd "$root/test/simple" \
            && TMPDIR="$(mktemp -d)" ./run_simptest.sh -n 4 -e ../../examples/datatypes 2>&1 )"
    if [ "$(echo "$out" | grep -c 'datatypes: rank .*: PASS')" = 4 ]; then
        ok "4 ranks, one server: every rank read every peer's values"
    else
        bad "simptest run: $(echo "$out" | grep -iE 'failed|wrong|mismatch' | head -1)"
    fi

    echo
    echo "  NOTE: shmem3, the cross-server modex, and the GDS fallback all"
    echo "        need the swarm. Run './run-gds-tests.sh linux' for those."
}

########################################################################

banner "src/mca/gds: the datastore across builds and across nodes"
case "$mode" in
    linux) test_linux ;;
    macos) test_macos ;;
    *) echo "usage: $0 [linux|macos]" >&2; exit 2 ;;
esac

printf '\n=== summary: %d passed, %d failed, %d skipped ===\n' "$pass" "$fail" "$skip"
[ "$fail" = 0 ]
