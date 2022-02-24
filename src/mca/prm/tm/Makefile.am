# -*- makefile -*-
#
# Copyright (c) 2020      Intel, Inc.  All rights reserved.
# Copyright (c) 2022      Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

AM_CPPFLAGS = $(prm_tm_CPPFLAGS)

headers = prm_tm.h
sources = \
        prm_tm_component.c \
        prm_tm.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_pmix_prm_tm_DSO
lib =
lib_sources =
component = pmix_mca_prm_tm.la
component_sources = $(headers) $(sources)
else
lib = libpmix_mca_prm_tm.la
lib_sources = $(headers) $(sources)
component =
component_sources =
endif

mcacomponentdir = $(pmixlibdir)
mcacomponent_LTLIBRARIES = $(component)
pmix_mca_prm_tm_la_SOURCES = $(component_sources)
pmix_mca_prm_tm_la_LIBADD = $(prm_tm_LIBS)
pmix_mca_prm_tm_la_LDFLAGS = -module -avoid-version $(prm_tm_LDFLAGS)
if NEED_LIBPMIX
pmix_mca_prm_tm_la_LIBADD += $(top_builddir)/src/libpmix.la
endif

noinst_LTLIBRARIES = $(lib)
libpmix_mca_prm_tm_la_SOURCES = $(lib_sources)
libpmix_mca_prm_tm_la_LDFLAGS = -module -avoid-version $(prm_tm_LDFLAGS)
