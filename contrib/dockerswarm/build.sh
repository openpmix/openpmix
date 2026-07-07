#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Build PMIx (your *live* working tree) plus the PRRTE launcher for the group
# test swarm (README.md) -- no commit required, never stale.
#
# This harness is PMIx-centric: the code under test is the openpmix tree this
# script lives in.  PRRTE master is baked (as source) into the image and built
# against your PMIx here, so the launcher's PMIx *server* library is the one you
# are testing -- which is what makes it exercise server-side collective code.
#
# The PMIx tree is built OUT OF TREE (VPATH) so the source stays pristine:
#
#   ./build.sh          # or 'linux': build (in a container) into the shared
#                       #   /opt/prte volume that the swarm nodes mount
#   ./build.sh macos    # build natively on this host (single-host smoke)
#   ./build.sh image    # (re)build just the base container image
#
# Because a VPATH configure refuses to run while the source tree still has an
# in-tree build, this script runs `make distclean` (once) at the repo root and
# then `./autogen.pl`.  After that, ALL builds are out-of-tree and your
# top-level source dir stays clean.
#
# Requires: docker (for linux/image), git, and a working autotools toolchain.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(git -C "$here" rev-parse --show-toplevel)"     # the openpmix tree

IMAGE="${IMAGE:-pmix-swarm:latest}"
VOLUME="${VOLUME:-pmix-build}"
PRRTE_REF="${PRRTE_REF:-master}"        # baked-image PRRTE branch
PRRTE_REPO="${PRRTE_REPO:-https://github.com/openpmix/prrte.git}"

# the group example programs that exercise the construction methods; built as
# standalone clients against the installed PMIx and launched by prterun/prun.
# group_die and connect_die (a participant leaves before contributing) drive
# the lost-connection accounting, and group_leave (a member voluntarily departs
# a live group) drives the group-leave notification path; all three are
# exercised separately by run-tests.sh.
GROUP_EXAMPLES="group group_bootstrap group_dmodex group_lcl_cid asyncgroup multi_nspace_group"
BUILD_EXAMPLES="$GROUP_EXAMPLES group_die connect_die group_leave"

mode="${1:-linux}"

# --- make the source tree VPATH-ready (idempotent) --------------------------
prep_srcdir() {
    if [ -f "$root/config.status" ] || [ -f "$root/Makefile" ]; then
        echo ">>> make distclean (source tree had an in-tree build)"
        make -C "$root" distclean >/dev/null 2>&1 || true
    fi
    if [ ! -x "$root/configure" ] || [ "$root/configure.ac" -nt "$root/configure" ]; then
        echo ">>> autogen.pl"
        ( cd "$root" && ./autogen.pl )
    fi
}

# --- (re)build the base image if needed -------------------------------------
build_image() {
    if [ "${1:-}" = force ] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
        echo ">>> docker build $IMAGE (baked PRRTE $PRRTE_REF)"
        docker build -t "$IMAGE" \
            --build-arg PRRTE_REPO="$PRRTE_REPO" \
            --build-arg PRRTE_REF="$PRRTE_REF" \
            "$here"
    else
        echo ">>> using existing image $IMAGE (./build.sh image to rebuild)"
    fi
}

# --- Linux build (in a builder container, into the shared volume) -----------
build_linux() {
    prep_srcdir
    build_image
    docker volume create "$VOLUME" >/dev/null

    echo ">>> building PMIx (your tree) + PRRTE (baked master) into volume $VOLUME"
    docker run --rm \
        -v "$root":/pmix-src:ro \
        -v "$VOLUME":/opt/prte \
        -e BUILD_EXAMPLES="$BUILD_EXAMPLES" \
        "$IMAGE" bash -euo pipefail -c '
            jobs=$(nproc)

            echo ">>>> PMIx from bind-mounted /pmix-src -> /opt/prte/pmix"
            mkdir -p /opt/prte/vpath-pmix && cd /opt/prte/vpath-pmix
            [ -f config.status ] || /pmix-src/configure --prefix=/opt/prte/pmix --enable-debug
            make -j"$jobs" && make install

            echo ">>>> PRRTE (baked master) VPATH build -> /opt/prte/prte"
            mkdir -p /opt/prte/vpath-prte && cd /opt/prte/vpath-prte
            [ -f config.status ] || /src/prrte/configure \
                --prefix=/opt/prte/prte --with-pmix=/opt/prte/pmix --enable-debug
            make -j"$jobs" && make install

            echo ">>>> group example clients -> /opt/prte/tests"
            mkdir -p /opt/prte/tests
            for ex in $BUILD_EXAMPLES; do
                [ -f "/pmix-src/examples/$ex.c" ] || { echo "   (skip $ex: no source)"; continue; }
                gcc -O0 -g -o "/opt/prte/tests/$ex" "/pmix-src/examples/$ex.c" \
                    -I/opt/prte/pmix/include -I/pmix-src/examples \
                    -L/opt/prte/pmix/lib -lpmix \
                && echo "   built $ex" || echo "   FAILED to build $ex"
            done

            # runtime env for login shells (node-entrypoint handles ld.so)
            printf "export PATH=/opt/prte/prte/bin:/opt/prte/tests:\$PATH\nexport LD_LIBRARY_PATH=/opt/prte/prte/lib:/opt/prte/pmix/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}\n" \
                > /opt/prte/env.sh
            echo ">>>> done: PMIx in /opt/prte/pmix, PRRTE in /opt/prte/prte, tests in /opt/prte/tests"
        '
    echo ">>> Linux build complete."
    echo ">>> next: docker compose up -d && ./run-tests.sh linux"
}

# --- macOS build (native, on this host; single-host smoke) ------------------
build_macos() {
    prep_srcdir
    local pfx="$root/vpath-macos-pmix/install"
    echo ">>> PMIx native VPATH build -> $pfx"
    mkdir -p "$root/vpath-macos-pmix" && cd "$root/vpath-macos-pmix"
    [ -f config.status ] || "$root/configure" --prefix="$pfx" --enable-debug ${EXTRA_CONFIGURE_ARGS:-}
    make -j"$(sysctl -n hw.ncpu)" && make install

    local psrc="$root/vpath-macos-prte/src"
    if [ ! -d "$psrc" ]; then
        echo ">>> cloning PRRTE $PRRTE_REF -> $psrc"
        git clone --recursive -b "$PRRTE_REF" "$PRRTE_REPO" "$psrc"
        ( cd "$psrc" && ./autogen.pl )
    fi
    echo ">>> PRRTE native build -> $root/vpath-macos-prte/install"
    mkdir -p "$root/vpath-macos-prte/build" && cd "$root/vpath-macos-prte/build"
    [ -f config.status ] || "$psrc/configure" \
        --prefix="$root/vpath-macos-prte/install" --with-pmix="$pfx" --enable-debug \
        ${EXTRA_CONFIGURE_ARGS:-}
    make -j"$(sysctl -n hw.ncpu)" && make install

    echo ">>> group example clients -> $root/vpath-macos-prte/install/bin"
    for ex in $BUILD_EXAMPLES; do
        [ -f "$root/examples/$ex.c" ] || continue
        gcc -O0 -g -o "$root/vpath-macos-prte/install/bin/$ex" "$root/examples/$ex.c" \
            -I"$pfx/include" -I"$root/examples" -L"$pfx/lib" -lpmix \
        && echo "   built $ex" || echo "   FAILED to build $ex"
    done
    echo ">>> macOS build complete."
    echo ">>> next: ./run-tests.sh macos"
}

case "$mode" in
    linux) build_linux ;;
    macos) build_macos ;;
    image) prep_srcdir; build_image force ;;
    *) echo "usage: $0 [linux|macos|image]" >&2; exit 2 ;;
esac
