#
# Copyright (c) 2004-2010 The Trustees of Indiana University.
#                         All rights reserved.
# Copyright (c) 2014-2015 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2017      IBM Corporation.  All rights reserved.
# Copyright (c) 2019      Intel, Inc.  All rights reserved.
# Copyright (c) 2022-2024 Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

AM_CPPFLAGS = $(pcompress_zlibng_CPPFLAGS)

sources = \
        compress_zlibng.h \
        compress_zlibng_component.c \
        compress_zlibng.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_pmix_pcompress_zlibng_DSO
component_noinst =
component_install = pmix_mca_pcompress_zlibng.la
else
component_noinst = libpmix_mca_pcompress_zlibng.la
component_install =
endif

mcacomponentdir = $(pmixlibdir)
mcacomponent_LTLIBRARIES = $(component_install)
pmix_mca_pcompress_zlibng_la_SOURCES = $(sources)
pmix_mca_pcompress_zlibng_la_LDFLAGS = -module -avoid-version $(pcompress_zlibng_LDFLAGS)
pmix_mca_pcompress_zlibng_la_LIBADD = $(pcompress_zlibng_LIBS)
if NEED_LIBPMIX
pmix_mca_pcompress_zlibng_la_LIBADD += $(top_builddir)/src/libpmix.la
endif

noinst_LTLIBRARIES = $(component_noinst)
libpmix_mca_pcompress_zlibng_la_SOURCES = $(sources)
libpmix_mca_pcompress_zlibng_la_LDFLAGS = -module -avoid-version $(pcompress_zlibng_LDFLAGS)
libpmix_mca_pcompress_zlibng_la_LIBADD = $(pcompress_zlibng_LIBS)
