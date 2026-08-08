#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

AM_CPPFLAGS = $(pcompress_zstd_CPPFLAGS)

# Non-functional shim used only under --enable-test-build; always shipped
EXTRA_DIST = testbuild_zstd.h

sources = \
        compress_zstd.h \
        compress_zstd_component.c \
        compress_zstd.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_pmix_pcompress_zstd_DSO
component_noinst =
component_install = pmix_mca_pcompress_zstd.la
else
component_noinst = libpmix_mca_pcompress_zstd.la
component_install =
endif

mcacomponentdir = $(pmixlibdir)
mcacomponent_LTLIBRARIES = $(component_install)
pmix_mca_pcompress_zstd_la_SOURCES = $(sources)
pmix_mca_pcompress_zstd_la_LDFLAGS = -module -avoid-version $(pcompress_zstd_LDFLAGS)
pmix_mca_pcompress_zstd_la_LIBADD = $(pcompress_zstd_LIBS)
if NEED_LIBPMIX
pmix_mca_pcompress_zstd_la_LIBADD += $(top_builddir)/src/libpmix.la
endif

noinst_LTLIBRARIES = $(component_noinst)
libpmix_mca_pcompress_zstd_la_SOURCES = $(sources)
libpmix_mca_pcompress_zstd_la_LDFLAGS = -module -avoid-version $(pcompress_zstd_LDFLAGS)
libpmix_mca_pcompress_zstd_la_LIBADD = $(pcompress_zstd_LIBS)
