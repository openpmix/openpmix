# -*- makefile -*-
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

headers = pnet_efa.h
sources = \
        pnet_efa_component.c \
        pnet_efa.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_pmix_pnet_efa_DSO
lib =
lib_sources =
component = pmix_mca_pnet_efa.la
component_sources = $(headers) $(sources)
else
lib = libpmix_mca_pnet_efa.la
lib_sources = $(headers) $(sources)
component =
component_sources =
endif

mcacomponentdir = $(pmixlibdir)
mcacomponent_LTLIBRARIES = $(component)
pmix_mca_pnet_efa_la_SOURCES = $(component_sources)
pmix_mca_pnet_efa_la_LDFLAGS = -module -avoid-version
if NEED_LIBPMIX
pmix_mca_pnet_efa_la_LIBADD = $(top_builddir)/src/libpmix.la
endif

noinst_LTLIBRARIES = $(lib)
libpmix_mca_pnet_efa_la_SOURCES = $(lib_sources)
libpmix_mca_pnet_efa_la_LDFLAGS = -module -avoid-version
