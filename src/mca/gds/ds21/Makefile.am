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
# Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
# Copyright (c) 2017      Research Organization for Information Science
#                         and Technology (RIST). All rights reserved.
# Copyright (c) 2017-2018 Mellanox Technologies, Inc.
#                         All rights reserved.
# Copyright (c) 2022-2023 Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# we do NOT want picky compilers down here
CFLAGS = $(PMIX_CFLAGS_BEFORE_PICKY)

headers = \
        gds_ds21_base.h \
        gds_ds21_lock.h \
        gds_ds21_file.h

sources = \
        gds_ds21_base.c \
        gds_ds21_lock.c \
        gds_ds21_lock_pthread.c \
        gds_ds21_component.c \
        gds_ds21_file.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_pmix_gds_ds21_DSO
lib =
lib_sources =
component = pmix_mca_gds_ds21.la
component_sources = $(headers) $(sources)
else
lib = libpmix_mca_gds_ds21.la
lib_sources = $(headers) $(sources)
component =
component_sources =
endif

mcacomponentdir = $(pmixlibdir)
mcacomponent_LTLIBRARIES = $(component)
pmix_mca_gds_ds21_la_SOURCES = $(component_sources)
pmix_mca_gds_ds21_la_LDFLAGS = -module -avoid-version \
    $(PMIX_TOP_BUILDDIR)/src/mca/common/dstore/libpmix_mca_common_dstore.la
if NEED_LIBPMIX
pmix_mca_gds_ds21_la_LIBADD = $(top_builddir)/src/libpmix.la
endif

noinst_LTLIBRARIES = $(lib)
libpmix_mca_gds_ds21_la_SOURCES = $(lib_sources)
libpmix_mca_gds_ds21_la_LDFLAGS = -module -avoid-version
