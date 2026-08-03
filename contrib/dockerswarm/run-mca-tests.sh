#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Run the src/mca/base unit suite (test/unit/mca) against your live openpmix
# tree, in the configuration a developer's own `make check` does not give
# them: a Linux build with --enable-mca-dso, where every MCA component is a
# real dlopen'd plugin.
#
#   ./run-mca-tests.sh linux    # in the swarm's containers
#   ./run-mca-tests.sh macos    # natively, in-tree plus a --enable-mca-dso build
#
# WHY THIS IS NOT A MULTI-NODE TEST
#
# src/mca/base is process-local by construction.  It registers MCA variables,
# reads parameter files and the environment, finds and loads components, and
# runs the framework register/open/select/close lifecycle.  None of that
# crosses a node boundary, so spreading it over ten containers would tell you
# nothing that running it once does not.  The ten-node stage below is a
# contention and load pass -- ten independent copies against one kernel and
# one CPU set -- and is not claimed to be more.
#
# WHAT THE SWARM DOES BUY -- AND IT IS THE WHOLE POINT
#
#  1. --enable-mca-dso.  Roughly half of this directory is compiled but never
#     executed in an ordinary build.  pmix_mca_base_component_repository.c is
#     entirely #if PMIX_HAVE_PDL_SUPPORT, and find_dyn_components() in
#     pmix_mca_base_component_find.c only does anything when there are DSOs to
#     find.  A static build walks framework_static_components and stops.  So
#     the component-path parser, the repository hash, the dlopen/dlsym/version
#     checks, the retain/release refcount on a repository item and the
#     show_load_errors reporting around them all get zero coverage from
#     `make check` on a normal tree.  Two of the defects the August 2026
#     review of this directory found were in exactly that code.
#
#  2. Linux.  The primary development host here is macOS.  This directory
#     reaches getcwd, geteuid, the home directory, syslog and dlopen, and the
#     container is the nearest Linux userspace.
#
#  3. An OPTIMIZED build.  register_variable() and several other routines
#     guard developer errors behind assert() and #if PMIX_ENABLE_DEBUG.  A
#     --disable-debug tree is the one users get and the one nobody tests.
#
# Beyond the unit suite, the stages below drive the parameter forms that only
# reach live code once components are loadable, plus the three malformed-input
# cases that used to be memory errors: an mca_base_component_path entry with
# no "@" delimiter, an over-long project name in one, and
# mca_base_verbose=syslogid:<str>.  Those assert survival, not output.
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

# The program list comes out of the Makefile.am rather than being repeated
# here, so adding an mca_base_* test to the suite does not silently leave it
# out of this run.
mca_programs() {
    sed -n '/^check_PROGRAMS[[:space:]]*=/,/[^\\]$/p' \
        "$root/test/unit/mca/Makefile.am" \
        | sed 's/^check_PROGRAMS[[:space:]]*=//; s/\\$//' \
        | tr -s ' \t\n' ' '
}

# The hostile-input cases, as one shell fragment so the linux and macos halves
# cannot drift.  $1 is the directory holding an installed pmix_info.  Every
# case asserts that the process SURVIVES -- what it prints is not the subject,
# and a malformed MCA parameter is an ordinary user mistake, not something
# that may take the library down.  A crash shows up as a signal exit status
# (>128), which is why each case tests for that rather than for zero.
#
# Note the deliberate use of pmix_info: it is the one installed program that
# opens every framework, so it exercises the repository across all of them.
BAD_INPUT_CASES='
# Refuse to run at all if the binary is not there.  Without this the cases
# below "pass" against a missing program -- env(1) exits 127, which is not a
# signal, so every case reports ok and the whole stage proves nothing.  That
# is precisely how a build failure two stages earlier turned into fourteen
# green lines the first time this script was run.
if [ ! -x "$INFO/pmix_info" ]; then
    echo "PREFLIGHT no-pmix_info-at-$INFO"
    exit 0
fi
survives() {   # $1 = label, rest = env assignments; prints "label verdict"
    local label="$1"; shift
    local rc
    env "$@" "$INFO/pmix_info" --all >/dev/null 2>&1
    rc=$?
    # What is being asserted is "not a MEMORY error", not "exit zero".
    #
    # Rejecting a malformed value is legitimate, and PMIx rejects some of
    # these loudly: an unparseable mca_base_component_show_load_errors makes
    # pmix_mca_base_open() fail, which pmix_info reports through show_help
    # and then abort()s on.  That is a diagnosed refusal, and SIGABRT is how
    # it arrives -- so treating every signal as failure would flag correct
    # behavior.  SIGSEGV/SIGBUS/SIGILL/SIGFPE have no such reading: they are
    # the library walking off the end of something, which is the entire
    # subject of this stage.
    #
    # 127 means the program never ran, which is a broken harness rather than
    # a passing case.
    case "$rc" in
        139|138|132|136) echo "$label CRASH(signal $((rc-128)))" ;;
        127)             echo "$label NOT-RUN" ;;
        *)               echo "$label ok" ;;
    esac
}
# Rejecting bad input is fine; accepting it silently is not.  This asserts
# that the one value the parser is documented to refuse actually is refused,
# and says so -- otherwise "survives" above would be satisfied by a library
# that quietly ignored it.
rejects() {    # $1 = label, rest = env assignments
    local label="$1"; shift
    if env "$@" "$INFO/pmix_info" --all 2>&1 >/dev/null \
         | grep -q "more than one"; then
        echo "$label ok"
    else
        echo "$label NOT-DIAGNOSED"
    fi
}
survives no-at-delimiter        PMIX_MCA_mca_base_component_path=/no/at/sign/here
survives overlong-project       PMIX_MCA_mca_base_component_path=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa@/tmp
survives empty-component-path   PMIX_MCA_mca_base_component_path=
survives syslogid               PMIX_MCA_mca_base_verbose=syslogid:pmixtest,stderr
survives sle-bare-framework     PMIX_MCA_mca_base_component_show_load_errors=ptl
survives sle-negated-framework  PMIX_MCA_mca_base_component_show_load_errors=^ptl
survives sle-fw-component       PMIX_MCA_mca_base_component_show_load_errors=ptl/tcp
survives sle-mixed              PMIX_MCA_mca_base_component_show_load_errors=ptl/tcp,gds,psec
survives sle-empty-entries      PMIX_MCA_mca_base_component_show_load_errors=ptl,,gds,
survives sle-too-many-slashes   PMIX_MCA_mca_base_component_show_load_errors=ptl/tcp/extra
rejects  sle-too-many-slashes-diagnosed \
                                PMIX_MCA_mca_base_component_show_load_errors=ptl/tcp/extra
survives sle-none               PMIX_MCA_mca_base_component_show_load_errors=none
survives paramfiles-none        PMIX_MCA_mca_base_param_files=none
survives missing-param-file     PMIX_MCA_mca_base_param_files=/nonexistent/mca-params.conf
survives deprecated-synonym     PMIX_MCA_mca_param_files=none
'

# Did the repository actually load anything?  With --enable-mca-dso the only
# way pmix_info can name a component is by having dlopen'd it, so a low count
# means the dynamic path silently found nothing -- which is exactly the
# failure mode a static build hides.
COUNT_COMPONENTS='
n=$("$INFO/pmix_info" --all 2>/dev/null \
      | grep -cE "^ *MCA [a-z0-9_]+: .+ \(MCA v")
echo "components:$n"
'

# An out-of-tree build cannot coexist with a config.status in the srcdir --
# autoconf refuses with "source directory already configured".  Every VPATH
# build below reads this clone through a read-only mount, so a configured-in-
# place tree stops them all.  Say so once, plainly, instead of letting three
# stages fail with an autoconf error that names no way out.  ./build.sh runs
# "make distclean" for exactly this reason; a test script has no business
# doing that to a working tree.
srcdir_blocks_vpath() {
    [ -f "$root/config.status" ] || return 1
    echo "     $root holds a config.status, so autoconf will refuse every" >&2
    echo "     VPATH build this script needs.  Either:" >&2
    echo "         make -C $root distclean && $0 $mode" >&2
    echo "     or run ./build.sh, which distcleans the tree for you." >&2
    return 0
}

########################################################################
# Linux: build the DSO configuration in a builder container, run the
# suite and the hostile-input cases against it, then a contention pass.
########################################################################

test_linux() {
    local progs rc out

    progs="$(mca_programs)"
    if [ -z "${progs// /}" ]; then
        bad "could not read check_PROGRAMS from test/unit/mca/Makefile.am"
        return
    fi
    echo "    suite: $progs"

    swarm_up_or_die

    if ! docker volume inspect "$VOLUME" >/dev/null 2>&1; then
        bad "build volume $VOLUME missing -- run ./build.sh first"
        return
    fi

    if srcdir_blocks_vpath; then
        skp "srcdir is configured in place -- no VPATH build can be made from it"
        return
    fi

    banner "unit suite in the tree build.sh configured (static components)"
    # Reuse the tree build.sh already configured; if it is not there, say so
    # rather than configuring a third one behind the user's back.
    docker run --rm \
        -v "$root":/pmix-src:ro \
        -v "$VOLUME":/opt/prte \
        -e PROGS="$progs" \
        "$IMAGE" bash -euo pipefail -c '
            cd /opt/prte/vpath-pmix 2>/dev/null || {
                echo "no /opt/prte/vpath-pmix -- run ./build.sh first" >&2; exit 2; }
            # The build volume outlives your branch, so this tree may have
            # been configured before test/unit/mca existed -- its
            # config.status has never heard of that Makefile and make dies
            # with "test/unit/mca: No such file or directory".  Re-running
            # config.status --recheck re-runs configure (with the original
            # arguments) against the current source, and the plain
            # config.status then instantiates the new Makefile.
            if [ ! -f test/unit/mca/Makefile ]; then
                echo "reconfiguring: this tree predates test/unit/mca"
                ./config.status --recheck
                ./config.status
                make -j"$(nproc)"
            fi
            make -j"$(nproc)" -C test/unit/mca check
            mkdir -p /opt/prte/tests-mca/static
            for p in $PROGS; do
                # Stage the real ELF binary, not the libtool wrapper.  An
                # uninstalled libtool target leaves a /bin/sh wrapper at
                # test/unit/mca/$p and puts the executable under .libs/;
                # copying the wrapper gives a script that says .libs/$p does
                # not exist and exits 1.
                cp "test/unit/mca/.libs/$p" /opt/prte/tests-mca/static/ 2>/dev/null \
                    || cp "test/unit/mca/$p" /opt/prte/tests-mca/static/ 2>/dev/null || true
            done
        '
    rc=$?
    [ "$rc" = 0 ] && ok "static build: unit suite green in the container" \
                  || bad "static build: unit suite failed (rc=$rc)"

    banner "--enable-mca-dso build (the configuration that exercises this code)"
    # A separate prefix and a separate VPATH directory: this library must not
    # displace the one the rest of the harness runs against.
    docker run --rm \
        -v "$root":/pmix-src:ro \
        -v "$VOLUME":/opt/prte \
        -e PROGS="$progs" \
        "$IMAGE" bash -euo pipefail -c '
            mkdir -p /opt/prte/vpath-pmix-dso && cd /opt/prte/vpath-pmix-dso
            [ -f config.status ] || /pmix-src/configure \
                --prefix=/opt/prte/pmix-dso --enable-mca-dso \
                --disable-debug --disable-devel-check
            # Two statements, not "make && make install": set -e does not fire
            # for a failing command in a non-final position of an && list, so
            # the joined form reports success over a stale install.
            make -j"$(nproc)"
            make install
            make -j"$(nproc)" -C test/unit/mca check
            mkdir -p /opt/prte/tests-mca/dso
            for p in $PROGS; do
                cp "test/unit/mca/.libs/$p" /opt/prte/tests-mca/dso/ 2>/dev/null \
                    || cp "test/unit/mca/$p" /opt/prte/tests-mca/dso/ 2>/dev/null || true
            done
        '
    rc=$?
    [ "$rc" = 0 ] && ok "mca-dso build: unit suite green in the container" \
                  || bad "mca-dso build: unit suite failed (rc=$rc)"

    banner "the repository actually loaded components"
    out=$(docker run --rm -v "$VOLUME":/opt/prte \
              -e INFO=/opt/prte/pmix-dso/bin \
              "$IMAGE" bash -c "export LD_LIBRARY_PATH=/opt/prte/pmix-dso/lib; $COUNT_COMPONENTS" \
              2>/dev/null | tr -d '\r')
    case "$out" in
        components:0|components:|"")
            bad "pmix_info named no components -- the dynamic path found nothing ($out)" ;;
        components:*)
            # A real tree has dozens; anything above a handful means dlopen,
            # dlsym and the version check all worked for many frameworks.
            if [ "${out#components:}" -ge 10 ] 2>/dev/null; then
                ok "pmix_info named ${out#components:} components out of the repository"
            else
                bad "pmix_info named only ${out#components:} components -- suspiciously few"
            fi ;;
        *) bad "unexpected output from the component count: $out" ;;
    esac

    banner "malformed MCA parameters must not be memory errors (dso build)"
    out=$(docker run --rm -v "$VOLUME":/opt/prte \
              -e INFO=/opt/prte/pmix-dso/bin \
              "$IMAGE" bash -c "export LD_LIBRARY_PATH=/opt/prte/pmix-dso/lib; $BAD_INPUT_CASES" \
              2>/dev/null | tr -d '\r')
    case "$out" in
        ""|*PREFLIGHT*)
            bad "the hostile-input stage never ran: ${out:-no output at all}" ;;
        *)
            while read -r label verdict; do
                [ -n "$label" ] || continue
                [ "$verdict" = ok ] && ok "survived: $label" \
                                    || bad "survived: $label -- $verdict"
            done <<< "$out" ;;
    esac

    # Ten containers on one host share a kernel and a set of CPUs, so this is
    # a load and contention pass, not a distributed one.  It is worth the
    # minute it costs because it confirms the staged binaries work under the
    # same runtime the rest of the harness uses.  It is not claimed to be
    # more than that.
    banner "concurrent run across all ten nodes (contention pass)"
    for cfg in static dso; do
        if ! ON 1 "test -d /opt/prte/tests-mca/$cfg"; then
            skp "$cfg binaries not staged"
            continue
        fi
        local lib="/opt/prte/pmix/lib"
        [ "$cfg" = dso ] && lib="/opt/prte/pmix-dso/lib"

        local pids="" n bad_nodes=""
        rm -f /tmp/swarm-mca-$cfg-*.out
        for n in $(seq 1 10); do
            ( docker exec "$NODE$n" bash -lc \
                "export LD_LIBRARY_PATH=$lib; \
                 cd /opt/prte/tests-mca/$cfg && \
                 for p in $progs; do ./\$p >/dev/null 2>&1 || exit 1; done" \
              >/dev/null 2>&1; echo "$?" > "/tmp/swarm-mca-$cfg-$n.out" ) &
            pids="$pids $!"
        done
        for p in $pids; do wait "$p"; done
        for n in $(seq 1 10); do
            [ "$(cat "/tmp/swarm-mca-$cfg-$n.out" 2>/dev/null)" = 0 ] \
                || bad_nodes="$bad_nodes node$n"
        done
        rm -f /tmp/swarm-mca-$cfg-*.out
        [ -z "$bad_nodes" ] && ok "$cfg: suite green on all ten nodes at once" \
                            || bad "$cfg: failed on$bad_nodes"
    done
}

########################################################################
# macOS: the in-tree build, plus an --enable-mca-dso VPATH build.  No
# containers -- the point here is still the DSO configuration, which the
# developer's own tree does not provide either.
########################################################################

test_macos() {
    local progs rc jobs out dso="$root/vpath-macos-pmix-dso"

    progs="$(mca_programs)"
    echo "    suite: $progs"
    jobs="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

    banner "in-tree build (whatever it is configured as)"
    if [ ! -f "$root/config.status" ]; then
        skp "no configured tree at $root -- run ./configure there first"
    else
        ( cd "$root" && make -j"$jobs" && make -C test/unit/mca check ) \
            >/tmp/mca-intree.$$ 2>&1
        rc=$?
        [ "$rc" = 0 ] && ok "in-tree: unit suite green" \
                      || { bad "in-tree: unit suite failed (rc=$rc)"; tail -30 /tmp/mca-intree.$$; }
        rm -f /tmp/mca-intree.$$
    fi

    banner "--enable-mca-dso VPATH build"
    if srcdir_blocks_vpath; then
        skp "srcdir is configured in place -- an out-of-tree build cannot coexist"
        return
    fi

    mkdir -p "$dso"
    ( cd "$dso" \
        && { [ -f config.status ] || "$root/configure" --prefix="$dso/install" \
                 --enable-mca-dso --disable-debug --disable-devel-check; } \
        && make -j"$jobs" \
        && make install \
        && make -C test/unit/mca check ) >/tmp/mca-dso.$$ 2>&1
    rc=$?
    if [ "$rc" != 0 ]; then
        bad "mca-dso: build or unit suite failed (rc=$rc)"
        tail -30 /tmp/mca-dso.$$
        rm -f /tmp/mca-dso.$$
        return
    fi
    ok "mca-dso: unit suite green"
    rm -f /tmp/mca-dso.$$

    banner "the repository actually loaded components"
    out=$(INFO="$dso/install/bin" \
          DYLD_LIBRARY_PATH="$dso/install/lib" bash -c "$COUNT_COMPONENTS" 2>/dev/null)
    if [ "${out#components:}" -ge 10 ] 2>/dev/null; then
        ok "pmix_info named ${out#components:} components out of the repository"
    else
        bad "pmix_info named too few components ($out)"
    fi

    banner "malformed MCA parameters must not be memory errors (dso build)"
    out=$(INFO="$dso/install/bin" \
          DYLD_LIBRARY_PATH="$dso/install/lib" bash -c "$BAD_INPUT_CASES" 2>/dev/null)
    case "$out" in
        ""|*PREFLIGHT*)
            bad "the hostile-input stage never ran: ${out:-no output at all}" ;;
        *)
            while read -r label verdict; do
                [ -n "$label" ] || continue
                [ "$verdict" = ok ] && ok "survived: $label" \
                                    || bad "survived: $label -- $verdict"
            done <<< "$out" ;;
    esac
}

########################################################################

case "$mode" in
    linux) test_linux ;;
    macos) test_macos ;;
    *) echo "usage: $0 [linux|macos]" >&2; exit 2 ;;
esac

printf '\n=== summary: %d passed, %d failed, %d skipped ===\n' "$pass" "$fail" "$skip"
[ "$fail" -eq 0 ]
