# -*- makefile -*-
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

AM_CPPFLAGS = $(psec_ssl_CPPFLAGS)

EXTRA_DIST = help-psec-ssl.txt testbuild_ssl.h

headers = psec_ssl.h
sources = \
        psec_ssl_component.c \
        psec_ssl.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_pmix_psec_ssl_DSO
lib =
lib_sources =
component = pmix_mca_psec_ssl.la
component_sources = $(headers) $(sources)
else
lib = libpmix_mca_psec_ssl.la
lib_sources = $(headers) $(sources)
component =
component_sources =
endif

mcacomponentdir = $(pmixlibdir)
mcacomponent_LTLIBRARIES = $(component)
pmix_mca_psec_ssl_la_SOURCES = $(component_sources)
pmix_mca_psec_ssl_la_LDFLAGS = -module -avoid-version $(psec_ssl_LDFLAGS)
pmix_mca_psec_ssl_la_LIBADD = $(psec_ssl_LIBS)
if NEED_LIBPMIX
pmix_mca_psec_ssl_la_LIBADD += $(top_builddir)/src/libpmix.la
endif

noinst_LTLIBRARIES = $(lib)
libpmix_mca_psec_ssl_la_SOURCES = $(lib_sources)
libpmix_mca_psec_ssl_la_LDFLAGS = -module -avoid-version $(psec_ssl_LDFLAGS)
libpmix_mca_psec_ssl_la_LIBADD = $(psec_ssl_LIBS)
