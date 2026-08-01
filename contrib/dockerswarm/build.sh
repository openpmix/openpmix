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
# Two clones on one host: export PMIX_SWARM (see swarm-common.sh) and each
# gets its own containers, volume, and network.  The macOS build is already
# per-clone -- it lives under <repo>/vpath-macos-*.
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

# PMIX_SWARM, IMAGE, VOLUME -- which swarm this build is for.  Set PMIX_SWARM
# in the whole shell and a second clone of openpmix gets its own containers,
# volume, and network instead of fighting over this one.
. "$here/swarm-common.sh"

PRRTE_REF="${PRRTE_REF:-master}"        # baked-image PRRTE branch
PRRTE_REPO="${PRRTE_REPO:-https://github.com/openpmix/prrte.git}"

# the group example programs that exercise the construction methods; built as
# standalone clients against the installed PMIx and launched by prterun/prun.
# run-tests.sh drives the construction methods plus three fault/notification
# exercisers: group_die and connect_die (a participant leaves before
# contributing) drive the lost-connection accounting, and group_leave (a member
# voluntarily departs a live group) drives the group-leave notification path.
#
# The EVENT_EXAMPLES exercise the dynamic, event-driven group operations and
# their fault paths, and are driven by the companion run-group-events.sh:
# group_invite (leader invites, invitees join, the group forms and every member
# is notified of completion), group_invite_timeout (an invitee never responds,
# so the leader's PMIX_TIMEOUT fires, reports the non-responder, and forms the
# group on those that accepted), group_invite_decline (an invitee explicitly
# declines, so the construct resolves immediately -- no timeout -- reporting the
# decliner and forming the group on those that accepted), group_invite_abort (an
# invitee declines an all-or-nothing invite -- no PMIX_GROUP_OPTIONAL -- so the
# whole construct aborts and every participant is notified), and
# group_destruct_die (a member is lost mid PMIx_Group_destruct, so the destruct
# must complete on the survivors -- the destruct analog of group_die).
GROUP_EXAMPLES="group group_bootstrap group_dmodex group_lcl_cid asyncgroup multi_nspace_group"
EVENT_EXAMPLES="group_invite group_invite_timeout group_invite_decline group_invite_abort group_destruct_die group_construct_abort group_daemon_fail"
# The topology/locality exerciser, driven by run-topology.sh: every rank loads
# the topology its LOCAL server handed it (the hwloc shmem segment, or XML as
# the fallback -- neither path exists in a standalone process) and then compares
# PMIX_LOCALITY_STRING with every peer. It has to run across real nodes: a
# single-host run cannot tell a correct answer from one that calls everything
# local, and the producer/consumer mismatch this catches made every process
# report NO shared locality with its own node-mates.
TOPO_EXAMPLES="topology"
BUILD_EXAMPLES="$GROUP_EXAMPLES group_die connect_die group_leave $EVENT_EXAMPLES $TOPO_EXAMPLES"

# The Python bindings are built with PMIx (--enable-python-bindings) and staged,
# together with the maintained test scripts from test/python and the swarm's own
# Python clients, into /opt/prte/tests-python.  Driven by run-python.sh.  This is
# the only place the bindings get exercised on Linux and across real nodes: the
# in-tree `make check` suite is single-host, and several cpuset/topology methods
# cannot run on macOS at all (no CPU-binding API behind hwloc_get_cpubind).
PYTHON_BINDINGS="${PYTHON_BINDINGS:-yes}"

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
        -e PYTHON_BINDINGS="$PYTHON_BINDINGS" \
        "$IMAGE" bash -euo pipefail -c '
            jobs=$(nproc)

            pyopt=""
            [ "$PYTHON_BINDINGS" = yes ] && pyopt="--enable-python-bindings"

            echo ">>>> PMIx from bind-mounted /pmix-src -> /opt/prte/pmix"
            mkdir -p /opt/prte/vpath-pmix && cd /opt/prte/vpath-pmix
            # Reconfigure when the build directory was configured with a
            # different set of options than we want now -- otherwise flipping
            # PYTHON_BINDINGS would be silently ignored, since the guard below
            # only asks whether *a* configure has ever run here.
            if [ -f config.status ] && ! ./config.status --config 2>/dev/null | grep -qF -- "${pyopt:-__none__}"; then
                if [ -n "$pyopt" ] || ./config.status --config 2>/dev/null | grep -q -- --enable-python-bindings; then
                    echo "   (configure options changed -- reconfiguring)"
                    rm -f config.status
                fi
            fi
            # Reconfigure when the *component set* has changed under us.
            # Automake records every configure.m4 it folded into aclocal.m4
            # as a prerequisite of aclocal.m4. Rename or delete a component
            # directory -- gds/shmem2 -> gds/shmem3, say -- and one of those
            # prerequisites no longer exists, so GNU make decides aclocal.m4
            # must be remade, shells out to aclocal-1.<n>, and dies: this
            # image ships no autotools. The build directory is then wedged
            # for good, and the message ("aclocal-1.18 is missing") points
            # nowhere near the cause. A stale prerequisite means the tree
            # has to be configured again from scratch.
            if [ -f config.status ] && [ -f Makefile ]; then
                _ts=$(sed -n "s/^top_srcdir *= *//p" Makefile | head -1)
                _stale=""
                for _m in $(sed -n "/^am__aclocal_m4_deps *=/,/[^\\\\]$/p" Makefile \
                            | tr " " "\n" | grep "\.m4$"); do
                    _m=${_m//\$(top_srcdir)/$_ts}
                    [ -e "$_m" ] || { _stale="$_m"; break; }
                done
                if [ -n "$_stale" ]; then
                    echo "   (component set changed -- $_stale is gone; reconfiguring)"
                    find . -name config.cache -delete 2>/dev/null || true
                    rm -f config.status
                fi
            fi
            [ -f config.status ] || /pmix-src/configure --prefix=/opt/prte/pmix --enable-debug $pyopt
            # NOTE: deliberately two statements, not "make && make install".
            # set -e does NOT fire for a failing command in a non-final
            # position of an && list, so the old form swallowed a build
            # failure and went on to report success with a stale install.
            make -j"$jobs"
            make install

            echo ">>>> PRRTE (baked master) VPATH build -> /opt/prte/prte"
            mkdir -p /opt/prte/vpath-prte && cd /opt/prte/vpath-prte
            # A rebuilt image re-clones PRRTE, so a build directory left by an
            # older image is stale: its Makefiles carry the previous clone`s
            # timestamps and maintainer mode then tries to regenerate the build
            # system with whatever aclocal/automake version the image happens to
            # have, which fails on the unexpanded OAC macros. Start over
            # whenever the baked source is newer than this build directory.
            if [ -f config.status ] && [ /src/prrte/configure -nt config.status ]; then
                echo "   (baked PRRTE source is newer -- reconfiguring from scratch)"
                cd / && rm -rf /opt/prte/vpath-prte && mkdir -p /opt/prte/vpath-prte
                cd /opt/prte/vpath-prte
            fi
            [ -f config.status ] || /src/prrte/configure \
                --prefix=/opt/prte/prte --with-pmix=/opt/prte/pmix --enable-debug
            make -j"$jobs"
            make install

            echo ">>>> group example clients -> /opt/prte/tests"
            mkdir -p /opt/prte/tests
            for ex in $BUILD_EXAMPLES; do
                [ -f "/pmix-src/examples/$ex.c" ] || { echo "   (skip $ex: no source)"; continue; }
                # -rpath, not just -L: these binaries are launched by prted,
                # which does NOT give its children a login shell, so the
                # LD_LIBRARY_PATH that env.sh sets is not in scope for them.
                # With PMIx installed outside the default loader directories
                # the result is "libpmix.so.2: cannot open shared object file"
                # on every rank -- which surfaces as a whole suite of group
                # tests failing with exit code 127 and nothing pointing at the
                # link line.  (No apostrophes in here: this block is the body
                # of a single-quoted bash -c argument.)
                gcc -O0 -g -o "/opt/prte/tests/$ex" "/pmix-src/examples/$ex.c" \
                    -I/opt/prte/pmix/include -I/pmix-src/examples \
                    -L/opt/prte/pmix/lib -lpmix -Wl,-rpath,/opt/prte/pmix/lib \
                && echo "   built $ex" || echo "   FAILED to build $ex"
            done

            # Python bindings + their test scripts -> /opt/prte/tests-python
            #
            # The built extension is staged by copying rather than by pointing
            # PYTHONPATH at the VPATH build directory: the nodes mount the
            # volume read-only, and a single directory holding both pmix*.so and
            # the scripts keeps the run-time environment to one PYTHONPATH entry.
            # We do NOT use `make install` for this -- its install-exec-local
            # runs `setup.py install`, which newer setuptools have removed.
            if [ "$PYTHON_BINDINGS" = yes ]; then
                echo ">>>> Python bindings + test scripts -> /opt/prte/tests-python"
                rm -rf /opt/prte/tests-python
                mkdir -p /opt/prte/tests-python
                so=$(ls -d /opt/prte/vpath-pmix/bindings/python/build/lib.*/pmix*.so 2>/dev/null | head -1)
                if [ -n "$so" ]; then
                    cp "$so" /opt/prte/tests-python/
                    # the maintained scripts, plus the swarm-specific clients
                    cp /pmix-src/test/python/*.py /opt/prte/tests-python/ 2>/dev/null || true
                    cp /pmix-src/contrib/dockerswarm/python/*.py /opt/prte/tests-python/ 2>/dev/null || true
                    chmod +x /opt/prte/tests-python/*.py 2>/dev/null || true
                    echo "   staged $(basename "$so") + $(ls /opt/prte/tests-python/*.py | wc -l) scripts"
                    # The Python ports of the C examples get their own
                    # directory: examples/python and test/python both carry a
                    # client.py and a server.py, so they cannot share one.
                    rm -rf /opt/prte/tests-examples
                    mkdir -p /opt/prte/tests-examples
                    cp /pmix-src/examples/python/*.py /opt/prte/tests-examples/ 2>/dev/null || true
                    # the extension goes here too, so a script in this
                    # directory imports pmix by Python`s script-directory rule
                    # with no PYTHONPATH at all. That matters for the examples
                    # that PMIx_Spawn a child job: the child is a new job and
                    # does not inherit prterun`s -x forwarding.
                    cp "$so" /opt/prte/tests-examples/
                    chmod +x /opt/prte/tests-examples/*.py 2>/dev/null || true
                    echo "   staged $(ls /opt/prte/tests-examples/*.py | wc -l) example ports"
                else
                    echo "   FAILED: no pmix*.so was built -- check the configure summary above"
                fi
            fi

            # runtime env for login shells (node-entrypoint handles ld.so)
            printf "export PATH=/opt/prte/prte/bin:/opt/prte/tests:\$PATH\nexport LD_LIBRARY_PATH=/opt/prte/prte/lib:/opt/prte/pmix/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}\nexport PYTHONPATH=/opt/prte/tests-python:/opt/prte/tests-examples\${PYTHONPATH:+:\$PYTHONPATH}\n" \
                > /opt/prte/env.sh
            echo ">>>> done: PMIx in /opt/prte/pmix, PRRTE in /opt/prte/prte, tests in /opt/prte/tests"
        '
    echo ">>> Linux build complete."
    if [ -z "$SWARM_ENV" ]; then
        echo ">>> next: docker compose up -d && ./run-tests.sh linux"
    else
        # Say it with the variable: compose reads PMIX_SWARM from the
        # environment of the compose command, and a plain `docker compose
        # up -d` brings up the DEFAULT swarm against the default volume,
        # leaving this build sitting in $VOLUME unused.
        echo ">>> next: ${SWARM_ENV}docker compose up -d && ${SWARM_ENV}./run-tests.sh linux"
        echo ">>>       (swarm '$PMIX_SWARM': containers ${NODE}1..10, volume $VOLUME)"
    fi
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
        # see the Linux build above for why this needs -rpath and not just -L
        gcc -O0 -g -o "$root/vpath-macos-prte/install/bin/$ex" "$root/examples/$ex.c" \
            -I"$pfx/include" -I"$root/examples" -L"$pfx/lib" -lpmix \
            -Wl,-rpath,"$pfx/lib" \
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
