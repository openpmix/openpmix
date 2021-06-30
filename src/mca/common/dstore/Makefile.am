#
# Copyright (c) 2018      Mellanox Technologies.  All rights reserved.
# Copyright (c) 2018      Intel, Inc. All rights reserved.
# Copyright (c) 2021      Amazon.com, Inc. or its affiliates.  All Rights
#                         reserved.
#
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# Header files

headers = \
        dstore_common.h \
        dstore_base.h \
        dstore_segment.h \
        dstore_file.h

# Source files

sources = \
        dstore_base.c \
        dstore_segment.c

lib_LTLIBRARIES =
noinst_LTLIBRARIES =

if MCA_BUILD_pmix_common_dstore_DSO
lib_LTLIBRARIES += libmca_common_dstore.la
else
noinst_LTLIBRARIES += libmca_common_dstore.la
endif

libmca_common_dstore_la_SOURCES = $(headers) $(sources)
libmca_common_dstore_la_LDFLAGS =
if MCA_BUILD_pmix_common_dstore_DSO
libmca_common_dstore_la_LDFLAGS += -version-info $(libmca_common_dstore_so_version)
endif
libmca_common_dstore_la_LIBADD =

# Conditionally install the header files

if WANT_INSTALL_HEADERS
pmixdir = $(pmixincludedir)/$(subdir)
pmix_HEADERS = $(headers)
endif
