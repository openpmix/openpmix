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
# Copyright (c) 2006-2010 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2012-2013 Los Alamos National Security, Inc.  All rights reserved.
# Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
# Copyright (c) 2026      Jeff Squyres  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

SUBDIRS = util

noinst_SCRIPTS = run_gds_fallback.pl

EXTRA_DIST = run_gds_fallback.pl.in

check_PROGRAMS = compress preg bfrops_regex2 gds_fallback pmix_log \
    util_basename util_string_copy util_argv util_path \
    util_environ util_alfg util_printf util_os_dirpath

TESTS = compress preg bfrops_regex2 gds_fallback run_gds_fallback.pl pmix_log \
    util_basename util_string_copy util_argv util_path \
    util_environ util_alfg util_printf util_os_dirpath

compress_SOURCES =  \
        compress.c
compress_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
compress_LDADD = \
    $(top_builddir)/src/libpmix.la

preg_SOURCES = \
        preg.c
preg_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
preg_LDADD = \
    $(top_builddir)/src/libpmix.la

bfrops_regex2_SOURCES = \
        bfrops_regex2.c
bfrops_regex2_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
bfrops_regex2_LDADD = \
    $(top_builddir)/src/libpmix.la

gds_fallback_SOURCES = \
        gds_fallback.c
gds_fallback_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
gds_fallback_LDADD = \
    $(top_builddir)/src/libpmix.la

pmix_log_SOURCES = \
        pmix_log.c
pmix_log_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
pmix_log_LDADD = \
    $(top_builddir)/src/libpmix.la

util_basename_SOURCES = \
        util_basename.c
util_basename_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
util_basename_LDADD = \
    $(top_builddir)/src/libpmix.la

util_string_copy_SOURCES = \
        util_string_copy.c
util_string_copy_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
util_string_copy_LDADD = \
    $(top_builddir)/src/libpmix.la

util_argv_SOURCES = \
        util_argv.c
util_argv_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
util_argv_LDADD = \
    $(top_builddir)/src/libpmix.la

util_path_SOURCES = \
        util_path.c
util_path_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
util_path_LDADD = \
    $(top_builddir)/src/libpmix.la

util_environ_SOURCES = \
        util_environ.c
util_environ_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
util_environ_LDADD = \
    $(top_builddir)/src/libpmix.la

util_alfg_SOURCES = \
        util_alfg.c
util_alfg_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
util_alfg_LDADD = \
    $(top_builddir)/src/libpmix.la

util_printf_SOURCES = \
        util_printf.c
util_printf_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
util_printf_LDADD = \
    $(top_builddir)/src/libpmix.la

util_os_dirpath_SOURCES = \
        util_os_dirpath.c
util_os_dirpath_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
util_os_dirpath_LDADD = \
    $(top_builddir)/src/libpmix.la

clean-local:
	rm -f compress preg bfrops_regex2 gds_fallback pmix_log \
	    util_basename util_string_copy util_argv util_path \
	    util_environ util_alfg util_printf util_os_dirpath

