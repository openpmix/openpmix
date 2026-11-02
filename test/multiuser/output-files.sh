#!/bin/bash
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# A DVM running as one non-root user, and prun started by another, with
# output files. Output files are created as the job's owner: a launcher
# whose PMIx supports PMIX_IOF_FILE_WRITER writes them itself, as the
# user it runs as, and a server that cannot act as the owner writes none.
# This runs that across two accounts:
#
#   dvmsvc   runs the DVM
#   jobuser  runs prun, writing to a directory of its own (0700)
#
# Neither is in the other's groups. Two settings let them meet: the DVM
# takes tools of other users only with PRTE_MCA_pmix_no_foreign_tools=0,
# and jobuser's prun talks to a server of another user only when
# PMIX_MCA_ptl_base_server_access names it.
#
# Cases:
#   1. --output dir=   files and directories are jobuser's, 0600 and
#                      0700, each line once; the DVM reports nothing
#   2. --output file=  the same for <file>.<nspace>.<rank>.out
#   3. copy, tag       a copy reaches prun's terminal; the files carry tags
#   4. a child job     a client's PMIx_Spawn with its own output directory:
#                      the child's files are jobuser's too
#   5. no writer       a tool that forwards none of the output to itself:
#                      the DVM is left to write, cannot act as jobuser,
#                      says so once, and writes nothing
#   6. a launcher of an earlier release - not run here
#   7. a path jobuser cannot write: nothing is written there
#   and through 1-7 the DVM logs no PMIx error
#   8. the DVM as root: cases 1 and 2 again, the files still jobuser's
#
# Run as root, with the PMIx and PRRTE to test first on PATH and
# LD_LIBRARY_PATH (or PMIX_PREFIX / PRTE_PREFIX set), both readable by
# the two accounts. The accounts are created if missing. KEEP=1 keeps the
# scratch directory. Exits 0 when every check passed.

set -u

npass=0
nfail=0
pass() { echo "  PASS: $*"; npass=$((npass + 1)); }
fail() { echo "  FAIL: $*"; nfail=$((nfail + 1)); }
check() {
    local what=$1
    shift
    if "$@"; then pass "$what"; else fail "$what"; fi
}
info() { echo "  INFO: $*"; }

if [ 0 -ne "$(id -u)" ]; then
    echo "$0: must run as root" >&2
    exit 2
fi

[ -n "${PRTE_PREFIX:-}" ] && PATH=$PRTE_PREFIX/bin:$PATH && LD_LIBRARY_PATH=$PRTE_PREFIX/lib:${LD_LIBRARY_PATH:-}
[ -n "${PMIX_PREFIX:-}" ] && PATH=$PMIX_PREFIX/bin:$PATH && LD_LIBRARY_PATH=$PMIX_PREFIX/lib:${LD_LIBRARY_PATH:-}
export PATH LD_LIBRARY_PATH
for t in prte prun pterm pmixcc; do
    if ! command -v $t > /dev/null; then
        echo "$0: $t is not on PATH" >&2
        exit 2
    fi
done

DVMUSER=dvmsvc
JOBUSER=jobuser
for u in $DVMUSER $JOBUSER; do
    id $u > /dev/null 2>&1 || useradd -m $u || exit 2
done
if id -nG $DVMUSER | tr ' ' '\n' | grep -qx "$(id -gn $JOBUSER)" ||
   id -nG $JOBUSER | tr ' ' '\n' | grep -qx "$(id -gn $DVMUSER)"; then
    echo "$0: $DVMUSER and $JOBUSER must not share a group" >&2
    exit 2
fi

# run as a user, with the paths to the build under test
as() {
    local u=$1
    shift
    runuser -u $u -- env PATH="$PATH" LD_LIBRARY_PATH="$LD_LIBRARY_PATH" HOME=$(eval echo ~$u) "$@"
}
for u in $DVMUSER $JOBUSER; do
    if ! as $u prun --version > /dev/null 2>&1; then
        echo "$0: $u cannot run $(command -v prun) - make the install readable by all" >&2
        exit 2
    fi
done

W=$(mktemp -d /tmp/pmix-multiuser.XXXXXX)
chmod 755 $W
cleanup() {
    [ -n "${DVMPID:-}" ] && kill $DVMPID 2> /dev/null
    if [ "${KEEP:-0}" = 1 ]; then echo "kept $W"; else rm -rf $W; fi
}
trap cleanup EXIT

# built as jobuser, who runs it: as root, pmixcc refuses a wrapper data
# file another user can write, which is what an install made by a
# non-root user has - as in CI
mkdir $W/cc
cp "$(dirname "$0")/mu_spawn.c" $W/cc/
chown -R $JOBUSER: $W/cc
if ! as $JOBUSER pmixcc -o $W/cc/mu_spawn $W/cc/mu_spawn.c > $W/cc.log 2>&1; then
    cat $W/cc.log
    echo "$0: could not build mu_spawn" >&2
    exit 2
fi

JOBOPTS="--map-by :oversubscribe"
# no prun or tool is let hang the run
TMO=${MU_TIMEOUT:-120}
# the job's output: a line on each stream naming the rank
PRINT="/bin/sh -c 'echo out-\$PMIX_RANK; echo err-\$PMIX_RANK >&2'"

# start a DVM as $1, its URI in $W/<tag>/uri and its stdout/stderr in
# $W/<tag>/log - not daemonized, so what it reports is kept
start_dvm() {
    local u=$1 tag=$2 extra=${3:-}
    mkdir -p $W/$tag
    chown $u: $W/$tag
    as $u PRTE_MCA_pmix_no_foreign_tools=0 prte $extra --report-uri $W/$tag/uri \
        > $W/$tag/log 2>&1 &
    DVMPID=$!
    for _ in $(seq 1 100); do
        [ -s $W/$tag/uri ] && return 0
        sleep 0.2
    done
    return 1
}
stop_dvm() {
    local u=$1 tag=$2
    as $u pterm --dvm-uri file:$W/$tag/uri > /dev/null 2>&1
    wait $DVMPID 2> /dev/null
    DVMPID=
}
# prun as jobuser against the DVM of $1
run_job() {
    local tag=$1
    shift
    eval timeout $TMO runuser -u $JOBUSER -- env PATH=\"\$PATH\" LD_LIBRARY_PATH=\"\$LD_LIBRARY_PATH\" \
        PMIX_MCA_ptl_base_server_access=users=$DVMUSER \
        prun --dvm-uri file:$W/$tag/uri $JOBOPTS "$@"
}
# a directory of jobuser's own
jobdir() {
    mkdir -p $1
    chown $JOBUSER: $1
    chmod 700 $1
}

# every file and directory below $1 is jobuser's, files 0600 and
# directories 0700, and there is at least one file
owned_right() {
    local bad
    [ -n "$(find $1 -mindepth 1 -type f)" ] || return 1
    bad=$(find $1 -mindepth 1 \( -type f \( ! -user $JOBUSER -o ! -perm 0600 \) \) -o \
                               \( -type d \( ! -user $JOBUSER -o ! -perm 0700 \) \))
    [ -z "$bad" ] || { echo "    not as expected: $bad"; return 1; }
}
# $2 appears exactly once in the files below $1
once() {
    [ 1 -eq "$(cat $(find $1 -type f) 2> /dev/null | grep -c -- "$2")" ]
}
not_reported() {
    ! grep -q 'output files of a job were not written' $W/$1/log
}

files_cases() {
    local tag=$1 label=$2 out

    out=$W/$tag-out1
    jobdir $out
    run_job $tag --output dir=$out -n 2 $PRINT > $W/$tag-prun1.log 2>&1
    check "$label --output dir=: prun succeeds" [ 0 -eq $? ]
    check "$label --output dir=: files and directories are $JOBUSER's, 0600 and 0700" owned_right $out
    check "$label --output dir=: one file per rank and stream" \
        [ 4 -eq "$(find $out -type f \( -name stdout -o -name stderr \) | wc -l)" ]
    check "$label --output dir=: each line once" eval 'once $out out-0 && once $out out-1 && once $out err-0 && once $out err-1'
    check "$label --output dir=: nothing on prun's terminal" eval '! grep -q "out-0" $W/$tag-prun1.log'

    out=$W/$tag-out2
    jobdir $out
    run_job $tag --output file=$out/f -n 2 $PRINT > $W/$tag-prun2.log 2>&1
    check "$label --output file=: prun succeeds" [ 0 -eq $? ]
    check "$label --output file=: files are $JOBUSER's and 0600" owned_right $out
    check "$label --output file=: named <file>.<nspace>.<rank>.out" \
        [ 2 -eq "$(find $out -type f -name 'f.*.[01].out' | wc -l)" ]
    check "$label --output file=: each line once" eval 'once $out out-0 && once $out out-1'
}

echo
echo "=== a DVM of $DVMUSER, prun of $JOBUSER, output files ==="
echo

if ! start_dvm $DVMUSER dvm; then
    fail "the DVM of $DVMUSER starts"
    cat $W/dvm/log
    exit 1
fi
pass "the DVM of $DVMUSER starts"
run_job dvm -n 1 /bin/true > $W/connect.log 2>&1
if [ 0 -ne $? ]; then
    fail "prun of $JOBUSER connects to it"
    cat $W/connect.log
    exit 1
fi
pass "prun of $JOBUSER connects to it"

# 1, 2
files_cases dvm "1-2:"

# 3. copy and tag
out=$W/out3
jobdir $out
run_job dvm --output tag,dir=$out:copy -n 2 $PRINT > $W/prun3.log 2>&1
check "3: --output dir=:copy succeeds" [ 0 -eq $? ]
check "3: the copy reaches prun's terminal" eval 'grep -q out-0 $W/prun3.log && grep -q out-1 $W/prun3.log'
check "3: the files are still $JOBUSER's" owned_right $out
check "3: tag puts the tags in the files" \
    grep -q '^\[.*\]<stdout>: *out-0' $(find $out -path '*rank.0*' -name stdout)

# 4. a child job with files of its own
out=$W/out4
jobdir $out
run_job dvm -n 1 $W/cc/mu_spawn --dir $out -- $PRINT > $W/prun4.log 2>&1
check "4: a client's child job with an output directory runs" [ 0 -eq $? ]
check "4: the child's files are $JOBUSER's" owned_right $out
check "4: ...one per stream" [ 2 -eq "$(find $out -type f | wc -l)" ]
out=$W/out4b
jobdir $out
jobdir $W/out4p
run_job dvm --output dir=$W/out4p -n 1 $W/cc/mu_spawn --dir $out -- $PRINT > $W/prun4b.log 2>&1
check "4: ...and with --output for the parent too" owned_right $out
check "1-4: the DVM never says it could not write a job's files" not_reported dvm

# 5. no writer: the DVM is asked to write, and cannot as jobuser. The
# directory is one the DVM could write, so writing as itself would show
mkdir $W/shared
chmod 1777 $W/shared
timeout $TMO runuser -u $JOBUSER -- env PATH="$PATH" LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
    PMIX_MCA_ptl_base_server_access=users=$DVMUSER $W/cc/mu_spawn --tool $W/dvm/uri --nofwd --dir $W/shared -- $PRINT > $W/tool5.log 2>&1
check "5: a tool's job forwarding no output to it runs" [ 0 -eq $? ]
sleep 1
# the DVM reports it to the tool that started the job
check "5: the DVM says it could not write the job's files" \
    grep -q 'output files of a job were not written' $W/tool5.log
check "5: ...once" \
    [ 1 -eq "$(cat $W/tool5.log $W/dvm/log | grep -c 'output files of a job were not written')" ]
check "5: nothing is written" [ -z "$(find $W/shared -mindepth 1)" ]

# 6.
info "6: a launcher of an earlier release is not run here"

# 7. a path jobuser cannot write
nowrite=$(eval echo ~$DVMUSER)/out7
chmod 700 $(eval echo ~$DVMUSER)
run_job dvm --output dir=$nowrite -n 1 $PRINT > $W/prun7.log 2>&1
rc=$?
check "7: nothing is written below a directory $JOBUSER cannot write" [ ! -e $nowrite ]
info "7: prun exited $rc and printed $(grep -c . $W/prun7.log) lines; first: $(grep -m1 . $W/prun7.log)"

stop_dvm $DVMUSER dvm
# what the DVM could not do for another user's jobs it falls back from
# quietly: its log holds no error
check "1-7: the DVM of $DVMUSER logs no PMIx error" eval '! grep -q "PMIX ERROR" $W/dvm/log'

# 8. control: the DVM as root
echo
echo "=== the same DVM run as root ==="
echo
if start_dvm root rootdvm --allow-run-as-root; then
    pass "8: a DVM of root starts"
    files_cases rootdvm "8:"
    stop_dvm root rootdvm
else
    fail "8: a DVM of root starts"
    cat $W/rootdvm/log
fi

echo
echo "$npass passed, $nfail failed"
[ 0 -eq $nfail ]
