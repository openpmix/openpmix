#
# Copyright (c) 2018      Mellanox Technologies.  All rights reserved.
# Copyright (c) 2018      Intel, Inc. All rights reserved.
# Copyright (c) 2021      Amazon.com, Inc. or its affiliates.  All Rights
#                         reserved.
#
# Copyright (c) 2022-2023 Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# Header files
# we do NOT want picky compilers down here
CFLAGS = $(PMIX_CFLAGS_BEFORE_PICKY)

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
lib_LTLIBRARIES += libpmix_mca_common_dstore.la
else
noinst_LTLIBRARIES += libpmix_mca_common_dstore.la
endif

libpmix_mca_common_dstore_la_SOURCES = $(headers) $(sources)
libpmix_mca_common_dstore_la_LDFLAGS =
if MCA_BUILD_pmix_common_dstore_DSO
libpmix_mca_common_dstore_la_LDFLAGS += -version-info $(libpmix_mca_common_dstore_so_version)
endif
libpmix_mca_common_dstore_la_LIBADD =

pmixdir = $(pmixincludedir)/$(subdir)
pmix_HEADERS = $(headers)
