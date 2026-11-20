#!/bin/sh
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# Build the release tarballs and the source RPM for one tag or branch.
#
#   with_hwloc=<dir> with_libevent=<dir> contrib/pmix-release.sh <ref> [options]
#
# <ref> is a tag or branch of the repository this script is run from.
# Any options are passed to make_dist_tarball in place of the default
# "--greekonly --verok". with_hwloc and with_libevent name the
# installations to build against: configure takes a with_<pkg> variable
# in its environment as it takes --with-<pkg>, and so does the configure
# that "make distcheck" runs.
#
# This is the procedure a release is built by:
#
#   ./contrib/make_dist_tarball --greekonly --verok
#   cp pmix-<version>.tar.bz2 contrib/ && cd contrib
#   ./buildrpm.sh pmix-<version>.tar.bz2      # -> $HOME/RPMBUILD/SRPMS
#
# run from a fresh clone of <ref> in a scratch directory, which must be
# on a filesystem of the machine's own. "make distcheck" makes the
# unpacked tree read-only and then runs "make dist" from it, whose
# "cp -fpR" of docs/_build/html creates the copy read-only and makes it
# writable again through an O_PATH descriptor (fchmodat on
# /proc/self/fd/N). A bind mount from a macOS host into a Docker
# container ("fakeowner") refuses that chmod, and the make phase fails
# with "cp: setting permissions for '.../docs/_build/html': Permission
# denied". The scratch directory is checked for this before anything is
# built.
#
# Environment:
#   PMIX_RELEASE_REPO     the repository to clone <ref> from (default:
#                         the one this script is in). Name one when this
#                         checkout is a git worktree whose repository is
#                         not at the same path here - inside a container,
#                         say.
#   PMIX_RELEASE_SCRATCH  where to clone and build (default: a new
#                         directory under ${TMPDIR:-/tmp})
#   PMIX_RELEASE_OUTDIR   where to put the tarballs and SRPM (default:
#                         the directory above the repository)
#   PMIX_RELEASE_KEEP     if set, keep the scratch directory on success
#   rpmtopdir             passed through to buildrpm.sh (default
#                         $HOME/RPMBUILD)

usage() {
    echo "Usage: with_hwloc=<dir> with_libevent=<dir> $0 <tag-or-branch> [make_dist_tarball options]"
    exit 1
}

fail() {
    echo "*** $*" >&2
    exit 1
}

if test $# -lt 1; then
    usage
fi
# a tarball made on macOS carries artifacts that break it when it is
# unpacked on Linux - make_dist_tarball refuses unless given --macosx,
# and a release is never made that way. Run this on Linux, in the
# container where the release is built.
if test "`uname -s`" = "Darwin"; then
    fail "release tarballs are made on Linux only - run this inside the Linux build container"
fi
ref=$1
shift
if test $# -eq 0; then
    set -- --greekonly --verok
fi

if test -z "$with_hwloc" || test -z "$with_libevent"; then
    fail "with_hwloc and with_libevent must name the hwloc and libevent installations"
fi
export with_hwloc with_libevent

if test -n "$PMIX_RELEASE_REPO"; then
    repo=$(cd "$PMIX_RELEASE_REPO" && git rev-parse --show-toplevel) || fail "$PMIX_RELEASE_REPO is not a repository"
else
    repo=$(cd "$(dirname "$0")/.." && git rev-parse --show-toplevel) || fail "cannot find the repository"
fi
outdir=${PMIX_RELEASE_OUTDIR:-`dirname "$repo"`}
test -d "$outdir" || fail "output directory $outdir does not exist"
outdir=`cd "$outdir" && pwd`

if test -n "$PMIX_RELEASE_SCRATCH"; then
    scratch=$PMIX_RELEASE_SCRATCH
    test -d "$scratch" || mkdir -p "$scratch" || fail "cannot make $scratch"
    test -z "`ls -A "$scratch"`" || fail "$scratch is not empty"
else
    scratch=`mktemp -d "${TMPDIR:-/tmp}/pmix-release.XXXXXX"` || fail "cannot make a scratch directory"
fi
scratch=`cd "$scratch" && pwd`
echo "*** Building $ref in $scratch"

# the copy "make distcheck" depends on: a read-only directory, copied
# with its mode
probe=$scratch/probe
mkdir -p "$probe/src/d" "$probe/out" && echo x > "$probe/src/d/f" && chmod -R a-w "$probe/src"
if ! (cd "$probe" && cp -fpR src/d out/) >/dev/null 2>&1; then
    chmod -R u+w "$probe" 2>/dev/null
    rm -rf "$probe" 2>/dev/null
    fail "$scratch cannot hold a copy of a read-only directory, which \"make distcheck\" makes - use a directory on a local filesystem (PMIX_RELEASE_SCRATCH)"
fi
chmod -R u+w "$probe" && rm -rf "$probe"

# a fresh clone, so the tarball is made from <ref> and nothing else
git clone -q --no-checkout "$repo" "$scratch/pmix" || fail "cannot clone $repo"
cd "$scratch/pmix" || fail "cannot enter $scratch/pmix"
git checkout -q "$ref" || fail "cannot check out $ref"
git submodule init -q || fail "cannot initialize the submodules"
# take the submodule from the checkout beside us when it has the commit,
# else from where .gitmodules says
oac_url=`git config submodule.oac.url`
if git -C "$repo/config/oac" rev-parse -q --verify "`git rev-parse HEAD:config/oac`^{commit}" >/dev/null 2>&1; then
    git config submodule.oac.url "$repo/config/oac"
fi
if ! git -c protocol.file.allow=always submodule update -q; then
    git config submodule.oac.url "$oac_url"
    git submodule update -q || fail "cannot check out the submodules"
fi
echo "*** Checked out `git log -1 --format='%h %s'`"

echo "*** Running make_dist_tarball $*"
./contrib/make_dist_tarball "$@" --distdir="$scratch" > "$scratch/dist.log" 2>&1
status=$?
if test $status -ne 0 || test -z "`ls "$scratch"/pmix-*.tar.bz2 2>/dev/null`"; then
    tail -n 30 "$scratch/dist.log"
    fail "make_dist_tarball failed - the full output is in $scratch/dist.log"
fi

rpmtopdir=${rpmtopdir:-$HOME/RPMBUILD}
export rpmtopdir
artifacts=
for tarball in "$scratch"/pmix-*.tar.bz2; do
    name=`basename "$tarball"`
    version=`echo "$name" | sed -e 's/^pmix-//' -e 's/\.tar\.bz2$//'`
    echo "*** Building the SRPM for $name"
    cp "$tarball" contrib/ || fail "cannot copy $name into contrib"
    (cd contrib && ./buildrpm.sh "$name") > "$scratch/rpm-$version.log" 2>&1
    srpm=`ls -t "$rpmtopdir"/SRPMS/pmix-"$version"-*.src.rpm 2>/dev/null | head -n 1`
    if test -z "$srpm"; then
        tail -n 30 "$scratch/rpm-$version.log"
        fail "buildrpm.sh made no SRPM for $version - the full output is in $scratch/rpm-$version.log"
    fi
    artifacts="$artifacts $tarball $scratch/pmix-$version.tar.gz $srpm"
done

cd "$outdir" || fail "cannot enter $outdir"
for f in $artifacts; do
    test -f "$f" || continue
    cp "$f" "$outdir/" || fail "cannot copy `basename "$f"` to $outdir"
done
if command -v sha1sum >/dev/null 2>&1; then
    sha="sha1sum"
else
    sha="shasum"
fi
echo "*** Wrote to $outdir:"
for f in $artifacts; do
    test -f "$f" && $sha "`basename "$f"`"
done

cd /
if test -z "$PMIX_RELEASE_KEEP"; then
    rm -rf "$scratch"
else
    echo "*** Kept $scratch"
fi
exit 0
