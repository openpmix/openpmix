# -*- makefile -*-
#
# Copyright (c) 2019      IBM Corporation.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

headers = psquash_none.h
sources = \
        psquash_none_component.c \
        psquash_none.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_pmix_psquash_none_DSO
lib =
lib_sources =
component = mca_psquash_none.la
component_sources = $(headers) $(sources)
else
lib = libmca_psquash_none.la
lib_sources = $(headers) $(sources)
component =
component_sources =
endif

mcacomponentdir = $(pmixlibdir)
mcacomponent_LTLIBRARIES = $(component)
mca_psquash_none_la_SOURCES = $(component_sources)
mca_psquash_none_la_LDFLAGS = -module -avoid-version

noinst_LTLIBRARIES = $(lib)
libmca_psquash_none_la_SOURCES = $(lib_sources)
libmca_psquash_none_la_LDFLAGS = -module -avoid-version
