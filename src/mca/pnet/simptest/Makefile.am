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
# Copyright (c) 2012      Los Alamos National Security, Inc.  All rights reserved.
# Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2017      Research Organization for Information Science
#                         and Technology (RIST). All rights reserved.
# Copyright (c) 2022-2025 Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

EXTRA_DIST = help-pnet-simptest.txt

headers = pnet_simptest.h
sources = \
        pnet_simptest_component.c \
        pnet_simptest.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_pmix_pnet_simptest_DSO
lib =
lib_sources =
component = pmix_mca_pnet_simptest.la
component_sources = $(headers) $(sources)
else
lib = libpmix_mca_pnet_simptest.la
lib_sources = $(headers) $(sources)
component =
component_sources =
endif

mcacomponentdir = $(pmixlibdir)
mcacomponent_LTLIBRARIES = $(component)
pmix_mca_pnet_simptest_la_SOURCES = $(component_sources)
pmix_mca_pnet_simptest_la_LDFLAGS = -module -avoid-version
if NEED_LIBPMIX
pmix_mca_pnet_simptest_la_LIBADD = $(top_builddir)/src/libpmix.la
endif

noinst_LTLIBRARIES = $(lib)
libpmix_mca_pnet_simptest_la_SOURCES = $(lib_sources)
libpmix_mca_pnet_simptest_la_LDFLAGS = -module -avoid-version
