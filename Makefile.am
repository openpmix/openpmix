#
# Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2005 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2009 High Performance Computing Center Stuttgart,
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2006-2016 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2012-2013 Los Alamos National Security, Inc.  All rights reserved.
# Copyright (c) 2013-2022 Intel, Inc.  All rights reserved.
# Copyright (c) 2019      Amazon.com, Inc. or its affiliates.  All Rights
#                         reserved.
# Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# Note that the -I directory must *exactly* match what was specified
# via AC_CONFIG_MACRO_DIR in configure.ac.
ACLOCAL_AMFLAGS = -I ./config

SUBDIRS = config contrib include src etc bindings docs

AM_DISTCHECK_CONFIGURE_FLAGS = --disable-dlopen

headers =
sources =
nodist_headers =
EXTRA_DIST = AUTHORS README.md VERSION LICENSE autogen.pl

# Only install the valgrind suppressions file and man pages
# if we're building in standalone mode
dist_pmixdata_DATA =
dist_pmixdata_DATA += contrib/pmix-valgrind.supp

if PMIX_TESTS_EXAMPLES
SUBDIRS += . test examples
endif

# Say so when "make check" has nothing to run. Without this, configuring
# --without-tests-examples leaves test/ out of SUBDIRS entirely, so "make
# check" recurses into nothing and exits 0 -- a result identical to a
# clean full run. The same silent-skip shape (a conditional SUBDIRS) hid
# the whole of test/ from every default-visibility build for as long as
# the WANT_HIDDEN condition was there; see the note in test/Makefile.am.
if !PMIX_TESTS_EXAMPLES
check-local:
	@echo "==============================================================="
	@echo "NOTE: no tests were run."
	@echo ""
	@echo "This tree was configured --without-tests-examples, so test/ is"
	@echo "not in SUBDIRS and 'make check' had nothing to descend into."
	@echo "This is a success only in the sense that nothing failed."
	@echo "Reconfigure with --with-tests-examples to run the suite."
	@echo "==============================================================="
endif

pmixdir = $(pmixincludedir)/$(subdir)
nobase_pmix_HEADERS = $(headers)

dist-hook:
	env LS_COLORS= sh "$(top_srcdir)/config/distscript.sh" "$(top_srcdir)" "$(distdir)" "$(PMIX_VERSION)" "$(PMIX_REPO_REV)"

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = maint/pmix.pc
