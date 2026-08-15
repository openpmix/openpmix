#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

AM_CPPFLAGS = $(pcompress_lz4_CPPFLAGS)

# Non-functional shim used only under --enable-test-build; always shipped
EXTRA_DIST = testbuild_lz4.h

sources = \
        compress_lz4.h \
        compress_lz4_component.c \
        compress_lz4.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_pmix_pcompress_lz4_DSO
component_noinst =
component_install = pmix_mca_pcompress_lz4.la
else
component_noinst = libpmix_mca_pcompress_lz4.la
component_install =
endif

mcacomponentdir = $(pmixlibdir)
mcacomponent_LTLIBRARIES = $(component_install)
pmix_mca_pcompress_lz4_la_SOURCES = $(sources)
pmix_mca_pcompress_lz4_la_LDFLAGS = -module -avoid-version $(pcompress_lz4_LDFLAGS)
pmix_mca_pcompress_lz4_la_LIBADD = $(pcompress_lz4_LIBS)
if NEED_LIBPMIX
pmix_mca_pcompress_lz4_la_LIBADD += $(top_builddir)/src/libpmix.la
endif

noinst_LTLIBRARIES = $(component_noinst)
libpmix_mca_pcompress_lz4_la_SOURCES = $(sources)
libpmix_mca_pcompress_lz4_la_LDFLAGS = -module -avoid-version $(pcompress_lz4_LDFLAGS)
libpmix_mca_pcompress_lz4_la_LIBADD = $(pcompress_lz4_LIBS)
