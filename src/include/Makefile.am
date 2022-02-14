# -*- makefile -*-
#
# Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2005 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2017      Research Organization for Information Science
#                         and Technology (RIST). All rights reserved.
# Copyright (c) 2021-2022 Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

pmixdir = $(pmixincludedir)/$(subdir)

noinst_LTLIBRARIES = libpmixglobal.la

libpmixglobal_la_SOURCES = \
	pmix_globals.h \
        pmix_globals.c

headers = pmix_globals.h

headers += \
        pmix_atomic.h \
        pmix_hash_string.h \
        pmix_socket_errno.h \
        pmix_stdint.h \
        pmix_prefetch.h \
        pmix_types.h \
        pmix_config.h \
        pmix_config_top.h \
        pmix_config_bottom.h \
        pmix_portable_platform.h \
        pmix_portable_platform_real.h \
        pmix_frameworks.h \
        dictionary.h

nobase_pmix_HEADERS = $(headers)

# Need to ensure that dictionary.h is built first -- before any other
# targets.
BUILT_SOURCES = dictionary.h

dictionary.h: $(top_srcdir)/include/pmix_common.h.in
	$(PYTHON) $(top_srcdir)/contrib/construct_dictionary.py

MAINTAINERCLEANFILES = dictionary.h
