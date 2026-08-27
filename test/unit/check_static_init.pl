#!/usr/bin/env perl
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# A static initializer that names the object it initializes must name the
# RIGHT object.
#
# PMIX_LIST_STATIC_INIT takes the list it is initializing, because an
# empty list is one whose sentinel points at itself and there is no other
# way to spell that address at compile time. Handing it a different list
# of the same type compiles perfectly and produces a list whose sentinel
# belongs to somebody else: appends land on the wrong list, walks find
# the wrong items, and destructing either one leaves the other pointing
# into freed memory. Nothing downstream catches it, which is why this
# exists.
#
# Nothing here is maintained by hand. The set of macros to check is
# derived from the headers: a macro is "self-naming" when its definition
# uses its parameter as an object -- "(p)." -- rather than as a type or a
# class, which is what separates PMIX_LIST_STATIC_INIT(l) from
# PMIX_OBJ_STATIC_INIT(pmix_object_t). A new one is covered the day it is
# written.

use strict;
use warnings;
use File::Basename;
use File::Find;

# Top of the source tree, from this script's own location, so that a
# VPATH build and an in-tree build both work without being told.
my $top = dirname(dirname(dirname(File::Spec->rel2abs($0))));
$top = $ENV{srcdir} . "/../.." if (!-d "$top/src" && defined $ENV{srcdir});
if (!-d "$top/src") {
    print "check_static_init: cannot locate the source tree from $0\n";
    exit 77;    # skip rather than fail: this is not a defect in the tree
}

my @files;
find(sub { push @files, $File::Find::name if (/\.[ch]$/); },
     "$top/src", "$top/test", "$top/examples");
@files = sort @files;

# ---------------------------------------------------------------- #
# 1. which *_STATIC_INIT macros name an object?
# ---------------------------------------------------------------- #
my %selfnaming;
foreach my $f (@files) {
    next unless ($f =~ /\.h$/);
    open(my $fh, '<', $f) or next;
    my @lines = <$fh>;
    close($fh);
    for (my $i = 0; $i < @lines; $i++) {
        next unless ($lines[$i] =~ /^\s*#\s*define\s+(\w+_STATIC_INIT)\s*\(\s*(\w+)\s*\)/);
        my ($name, $param) = ($1, $2);
        # gather the whole definition, following backslash continuations
        my $body = $lines[$i];
        while ($body =~ /\\\s*$/ && $i + 1 < @lines) {
            $body .= $lines[++$i];
        }
        # the parameter used as an object - "(p)." - not as a type name
        $selfnaming{$name} = 1 if ($body =~ /\(\s*\Q$param\E\s*\)\s*\./);
    }
}
if (!keys %selfnaming) {
    print "check_static_init: found no self-naming initializers to check\n";
    exit 77;
}
my $macros = join('|', map { quotemeta } sort keys %selfnaming);

# ---------------------------------------------------------------- #
# 2. every use must name the object it is initializing
# ---------------------------------------------------------------- #
my ($checked, @bad) = (0);
foreach my $f (@files) {
    open(my $fh, '<', $f) or next;
    my $lno = 0;
    while (my $line = <$fh>) {
        $lno++;
        my $rel = $f;
        $rel =~ s/^\Q$top\E\///;

        # a struct member:   .field = NAME(obj.field)
        #
        # The argument may itself contain parentheses - a composed
        # initializer passes "(e).actives" down - so it runs to the ")"
        # that closes the macro, which is the one followed by a comma, a
        # brace, a semicolon, or the end of the line (a line continuation
        # included).
        if ($line =~ /\.(\w+)\s*=\s*($macros)\s*\(\s*(.*?)\s*\)\s*(?=[,;}]|\\?\s*$)/) {
            my ($field, $macro, $arg) = ($1, $2, $3);
            $checked++;
            next if ($arg =~ /\.\Q$field\E$/);
            push @bad, sprintf("%s:%d: .%s is initialized by %s(%s)\n"
                             . "    the argument must name this member, i.e. end in \".%s\"",
                               $rel, $lno, $field, $macro, $arg, $field);
            next;
        }

        # a whole object:    pmix_list_t foo = NAME(foo)
        if ($line =~ /\b(\w+)\s*=\s*($macros)\s*\(\s*(.*?)\s*\)\s*(?=[,;}]|\\?\s*$)/) {
            my ($var, $macro, $arg) = ($1, $2, $3);
            $checked++;
            next if ($arg eq $var);
            push @bad, sprintf("%s:%d: %s is initialized by %s(%s)\n"
                             . "    the argument must name the object itself, i.e. \"%s\"",
                               $rel, $lno, $var, $macro, $arg, $var);
        }
    }
    close($fh);
}

printf("checked %d use%s of %d self-naming initializer%s (%s)\n",
       $checked, ($checked == 1 ? "" : "s"),
       scalar(keys %selfnaming), (keys %selfnaming == 1 ? "" : "s"),
       join(", ", sort keys %selfnaming));

if (@bad) {
    print "\nFAILED - a static initializer names the wrong object:\n\n";
    print "  $_\n\n" foreach (@bad);
    exit 1;
}
print "all of them name the object they initialize\n";
exit 0;
