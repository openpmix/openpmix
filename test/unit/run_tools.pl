#!/usr/bin/env perl
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow

# Smoke-test the user-facing command-line tools in src/tools that do NOT
# need to connect to a PMIx server. This guards the shared tool spine -
# command-line parsing, the pinstalldirs/show_help bootstrap, and a clean
# library shutdown - by running the built binaries and asserting they
# parse, execute, and finalize with the expected exit status and output.
#
# We deliberately exercise only the no-connect paths:
#   pmix_info --version / -c        (never calls PMIx_tool_init)
#   pmixcc  -showme:*               (compiler wrapper; never connects)
#   pattrs  --client/server/tool-fns (uses PMIX_TOOL_DO_NOT_CONNECT)
# The server-connecting tools (pquery, palloc, pctrl, plookup, pevent) are
# intentionally left out: a developer's machine may have live PMIx servers
# or stale pmix.* rendezvous files in $TMPDIR that make those paths
# non-deterministic. Connecting tools belong in a simptest-based harness.

use strict;
use warnings;

# Locate the build tree from this script's own path: test/unit/ -> root.
use File::Basename;
use Cwd 'abs_path';
my $here = dirname(abs_path($0));
my $root = abs_path("$here/../..");
my $tdir = "$root/src/tools";

# The tools are static (non-DSO) in this build, so they need no MCA
# component path to run from the build tree.

# Bound every invocation so a wedged tool can never hang "make check".
sub run {
    my ($label, $argv, $want_exit, $want_re) = @_;
    my $cmd = join(" ", @$argv) . " </dev/null 2>&1";
    my $out = "";
    my $status;
    eval {
        local $SIG{ALRM} = sub { die "alarm\n"; };
        alarm(120);
        $out = `$cmd`;
        $status = $? >> 8;
        alarm(0);
    };
    if ($@) {
        print "FAIL [$label]: timed out\n";
        return 1;
    }
    if (defined($want_exit) && $status != $want_exit) {
        print "FAIL [$label]: exit $status (wanted $want_exit)\n$out\n";
        return 1;
    }
    if (defined($want_re) && $out !~ $want_re) {
        print "FAIL [$label]: output did not match $want_re\n$out\n";
        return 1;
    }
    print "PASS [$label]\n";
    return 0;
}

# Make sure the binaries were actually built before asserting on them.
my %bin = (
    pmix_info => "$tdir/pmix_info/pmix_info",
    pmixcc    => "$tdir/wrapper/pmixcc",
    pattrs    => "$tdir/pattrs/pattrs",
);
foreach my $name (sort keys %bin) {
    unless (-x $bin{$name}) {
        print "SKIP: $name not built at $bin{$name}\n";
        exit(77);   # automake "skipped" status
    }
}

my $fails = 0;

# pmix_info: prints the version banner, exit 0.
$fails += run("pmix_info --version", [$bin{pmix_info}, "--version"],
              0, qr/\(PMIx\)/);
# pmix_info -c: dumps configure line, exit 0.
$fails += run("pmix_info -c", [$bin{pmix_info}, "-c"], 0, qr/Configured/);

# pmixcc showme variants: each is a dry run, exit 0, non-empty where useful.
$fails += run("pmixcc -showme:version", [$bin{pmixcc}, "-showme:version"],
              0, qr/\(PMIx\)/);
$fails += run("pmixcc -showme:compile", [$bin{pmixcc}, "-showme:compile"],
              0, qr/\S/);
$fails += run("pmixcc -showme:link", [$bin{pmixcc}, "-showme:link"],
              0, qr/-lpmix/);
$fails += run("pmixcc -showme:libs", [$bin{pmixcc}, "-showme:libs"], 0, undef);

# pattrs function lists: use PMIX_TOOL_DO_NOT_CONNECT, exit 0.
$fails += run("pattrs --client-fns", [$bin{pattrs}, "--client-fns"],
              0, qr/CLIENT SUPPORTED FUNCTIONS/);
$fails += run("pattrs --server-fns", [$bin{pattrs}, "--server-fns"],
              0, qr/SERVER SUPPORTED FUNCTIONS/);
$fails += run("pattrs --tool-fns", [$bin{pattrs}, "--tool-fns"],
              0, qr/TOOL SUPPORTED FUNCTIONS/);

if ($fails) {
    print "\n$fails tool smoke-test(s) FAILED\n";
    exit(1);
}
print "\nAll tool smoke-tests passed\n";
exit(0);
