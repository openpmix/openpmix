# -*- makefile -*-
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

headers = pnet_cxi.h
sources = \
        pnet_cxi_component.c \
        pnet_cxi.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_pmix_pnet_cxi_DSO
lib =
lib_sources =
component = pmix_mca_pnet_cxi.la
component_sources = $(headers) $(sources)
else
lib = libpmix_mca_pnet_cxi.la
lib_sources = $(headers) $(sources)
component =
component_sources =
endif

mcacomponentdir = $(pmixlibdir)
mcacomponent_LTLIBRARIES = $(component)
pmix_mca_pnet_cxi_la_SOURCES = $(component_sources)
pmix_mca_pnet_cxi_la_LDFLAGS = -module -avoid-version
if NEED_LIBPMIX
pmix_mca_pnet_cxi_la_LIBADD = $(top_builddir)/src/libpmix.la
endif

noinst_LTLIBRARIES = $(lib)
libpmix_mca_pnet_cxi_la_SOURCES = $(lib_sources)
libpmix_mca_pnet_cxi_la_LDFLAGS = -module -avoid-version
