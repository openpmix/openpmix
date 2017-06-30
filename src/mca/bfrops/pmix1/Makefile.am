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
# Copyright (c) 2013-2017 Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

headers = bfrop_pmix1.h
sources = \
        bfrop_pmix1_component.c \
        bfrop_pmix1.c \
        pack.c \
        unpack.c \
        copy.c \
        print.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_pmix_bfrops_pmix1_DSO
lib =
lib_sources =
component = mca_bfrops_pmix1.la
component_sources = $(headers) $(sources)
else
lib = libmca_bfrops_pmix1.la
lib_sources = $(headers) $(sources)
component =
component_sources =
endif

mcacomponentdir = $(pmixlibdir)
mcacomponent_LTLIBRARIES = $(component)
mca_bfrops_pmix1_la_SOURCES = $(component_sources)
mca_bfrops_pmix1_la_LDFLAGS = -module -avoid-version

noinst_LTLIBRARIES = $(lib)
libmca_bfrops_pmix1_la_SOURCES = $(lib_sources)
libmca_bfrops_pmix1_la_LDFLAGS = -module -avoid-version
