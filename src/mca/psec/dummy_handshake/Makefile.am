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
# Copyright (c) 2019      Mellanox Technologies, Inc.
#                         All rights reserved.
# Copyright (c) 2022      Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

if MCA_BUILD_PSEC_DUMMY_HANDSHAKE

headers = psec_dummy_handshake.h
sources = \
        psec_dummy_handshake_component.c \
        psec_dummy_handshake.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_pmix_psec_dummy_handshake_DSO
lib =
lib_sources =
component = pmix_mca_psec_dummy_handshake.la
component_sources = $(headers) $(sources)
else
lib = libpmix_mca_psec_dummy_handshake.la
lib_sources = $(headers) $(sources)
component =
component_sources =
endif

mcacomponentdir = $(pmixlibdir)
mcacomponent_LTLIBRARIES = $(component)
pmix_mca_psec_dummy_handshake_la_SOURCES = $(component_sources)
pmix_mca_psec_dummy_handshake_la_LDFLAGS = -module -avoid-version
if NEED_LIBPMIX
pmix_mca_psec_dummy_handshake_la_LIBADD = $(top_builddir)/src/libpmix.la
endif

noinst_LTLIBRARIES = $(lib)
libpmix_mca_psec_dummy_handshake_la_SOURCES = $(lib_sources)
libpmix_mca_psec_dummy_handshake_la_LDFLAGS = -module -avoid-version

endif
