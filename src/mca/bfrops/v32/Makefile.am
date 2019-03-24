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
# Copyright (c) 2013-2017 Intel, Inc. All rights reserved.
# Copyright (c) 2019      Mellanox Technologies, Inc.
#                         All rights reserve
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

headers = bfrop_pmix32.h
sources = \
        bfrop_pmix32_component.c \
        bfrop_pmix32.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_pmix_bfrops_v32_DSO
lib =
lib_sources =
component = mca_bfrops_v32.la
component_sources = $(headers) $(sources)
else
lib = libmca_bfrops_v32.la
lib_sources = $(headers) $(sources)
component =
component_sources =
endif

mcacomponentdir = $(pmixlibdir)
mcacomponent_LTLIBRARIES = $(component)
mca_bfrops_v32_la_SOURCES = $(component_sources)
mca_bfrops_v32_la_LDFLAGS = -module -avoid-version

noinst_LTLIBRARIES = $(lib)
libmca_bfrops_v32_la_SOURCES = $(lib_sources)
libmca_bfrops_v32_la_LDFLAGS = -module -avoid-version
