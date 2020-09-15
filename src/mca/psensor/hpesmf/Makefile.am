#
# Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
#
# Copyright (c) 2017-2020 Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

AM_CPPFLAGS = $(pmix_check_jansson_CPPFLAGS)

dist_pmixdata_DATA = help-pmix-psensor-hpe.txt

sources = \
        psensor_hpe.c \
        psensor_hpe.h \
        psensor_hpe_component.c

# Make the output library in this directory, and name it either
# mca_<type>_<name>.la (for DSO builds) or libmca_<type>_<name>.la
# (for static builds).

if MCA_BUILD_pmix_psensor_hpesmf_DSO
component_noinst =
component_install = mca_psensor_hpesmf.la
else
component_noinst = libmca_psensor_hpesmf.la
component_install =
endif

mcacomponentdir = $(pmixlibdir)
mcacomponent_LTLIBRARIES = $(component_install)
mca_psensor_hpesmf_la_SOURCES = $(sources)
mca_psensor_hpesmf_la_LDFLAGS = -module -avoid-version $(pmix_check_jansson_LDFLAGS)
mca_psensor_hpesmf_la_LIBADD = $(pmix_check_jansson_LIBS)
if NEED_LIBPMIX
mca_psensor_hpesmf_la_LIBADD += $(top_builddir)/src/libpmix.la
endif

noinst_LTLIBRARIES = $(component_noinst)
libmca_psensor_hpesmf_la_SOURCES =$(sources)
libmca_psensor_hpesmf_la_LDFLAGS = -module -avoid-version
