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
# Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

headers = ptl_tcp.h
sources = \
        ptl_tcp_component.c \
        ptl_tcp.c

# Make the output library in this directory, and name it either
# pmix_mca_<type>_<name>.la (for DSO builds) or libpmix_mca_<type>_<name>.la
# (for static builds) - configure.m4 makes this one always static.

if MCA_BUILD_pmix_ptl_tcp_DSO
lib =
lib_sources =
component = pmix_mca_ptl_tcp.la
component_sources = $(headers) $(sources)
else
lib = libpmix_mca_ptl_tcp.la
lib_sources = $(headers) $(sources)
component =
component_sources =
endif

mcacomponentdir = $(pmixlibdir)
mcacomponent_LTLIBRARIES = $(component)
pmix_mca_ptl_tcp_la_SOURCES = $(component_sources)
pmix_mca_ptl_tcp_la_LDFLAGS = -module -avoid-version
if NEED_LIBPMIX
pmix_mca_ptl_tcp_la_LIBADD = $(top_builddir)/src/libpmix.la
endif

noinst_LTLIBRARIES = $(lib)
libpmix_mca_ptl_tcp_la_SOURCES = $(lib_sources)
libpmix_mca_ptl_tcp_la_LDFLAGS = -module -avoid-version
