#
# Copyright (c) 2018      Mellanox Technologies.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# Header files

AM_CPPFLAGS = $(LTDLINCL)

headers = \
        common_dstore.h \
        gds_dstore.h

# Source files

sources = \
        gds_dstore.c

lib_LTLIBRARIES =
noinst_LTLIBRARIES =
comp_inst = libmca_common_dstore.la
comp_noinst = libmca_common_dstore_noinst.la

if MCA_BUILD_pmix_gds_ds12_DSO
lib_LTLIBRARIES += $(comp_inst)
else
noinst_LTLIBRARIES += $(comp_noinst)
endif

libmca_common_dstore_la_SOURCES = $(headers) $(sources)
libmca_common_dstore_la_CPPFLAGS = $(common_dstore_CPPFLAGS)
libmca_common_dstore_la_LDFLAGS = $(common_dstore_LDFLAGS)
libmca_common_dstore_la_LIBADD = $(common_dstore_LIBS)
libmca_common_dstore_noinst_la_SOURCES = $(headers) $(sources)

# Conditionally install the header files

if WANT_INSTALL_HEADERS
pmixdir = $(pmixincludedir)/$(subdir)
pmix_HEADERS = $(headers)
endif
