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
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

check_PROGRAMS = compress preg

TESTS = compress preg

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

clean-local:
	rm -f compress preg
