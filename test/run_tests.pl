#!/usr/bin/env perl
#
# Copyright (c) 2019      Intel, Inc.
#
# $COPYRIGHT$
#
# Additional copyrights may follow

use strict;

my @tests = ("-n 4 --ns-dist 3:1 --fence \"[db | 0:0-2;1:0]\"",
             "-n 4 --ns-dist 3:1 --fence \"[db | 0:;1:0]\"",
             "-n 4 --ns-dist 3:1 --fence \"[db | 0:;1:]\"",
             "-n 4 --ns-dist 3:1 --fence \"[0:]\"",
             "-n 4 --ns-dist 3:1 --fence \"[b | 0:]\"",
             "-n 4 --ns-dist 3:1 --fence \"[d | 0:]\" --noise \"[0:0,1]\"",
             "-n 4 --job-fence -c",
             "-n 4 --job-fence",
             "-n 2 --test-publish",
             "-n 2 --test-spawn",
             "-n 2 --test-connect",
             "-n 5 --test-resolve-peers --ns-dist \"1:2:2\"",
             "-n 5 --test-replace 100:0,1,10,50,99",
             "-n 5 --test-internal 10",
             "-s 2 -n 2 --job-fence",
             "-s 2 -n 2 --job-fence -c");

my $test;
my $cmd;
my $output;
my $status = 0;

foreach $test (@tests) {
    $cmd = "./pmix_test " . $test . " 2>&1";
    print $cmd . "\n";
    $output = `$cmd`;
    print $output . "\n";
    print "CODE $?\n";
    if ("0" != "$?") {
        $status = "$?";
    }
}
exit($status >> 8);
