#
# Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2005 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2009 High Performance Computing Center Stuttgart,
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2006-2020 Cisco Systems, Inc.  All rights reserved
# Copyright (c) 2012-2013 Los Alamos National Security, Inc.  All rights reserved.
# Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2021      Amazon.com, Inc. or its affiliates.
#                         All Rights reserved.
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

SUBDIRS = \
    include \
    util/keyval \
    util/showhelp \
    mca/base \
    $(MCA_pmix_FRAMEWORKS_SUBDIRS) \
    $(MCA_pmix_FRAMEWORK_COMPONENT_STATIC_SUBDIRS) \
    . \
    $(MCA_pmix_FRAMEWORK_COMPONENT_DSO_SUBDIRS)

DIST_SUBDIRS = \
    include \
    util/keyval \
    util/showhelp \
    mca/base \
    $(MCA_pmix_FRAMEWORKS_SUBDIRS) \
    $(MCA_pmix_FRAMEWORK_COMPONENT_ALL_SUBDIRS)

headers =
sources =
nodist_headers =
EXTRA_DIST =
dist_pmixdata_DATA =
nobase_pmix_HEADERS =
pmixdir = $(pmixincludedir)/$(subdir)

libpmix_la_LIBADD = \
	mca/base/libpmix_mca_base.la \
	$(MCA_pmix_FRAMEWORK_LIBS) \
	$(PMIX_EXTRA_LIB)
libpmix_la_DEPENDENCIES = \
	mca/base/libpmix_mca_base.la \
	$(MCA_pmix_FRAMEWORK_LIBS)

if PMIX_EMBEDDED_MODE

if WANT_INSTALL_HEADERS

lib_LTLIBRARIES = libpmix.la
libpmix_la_SOURCES = $(headers) $(sources)
libpmix_la_LDFLAGS = -version-info $(libpmix_so_version)

else

noinst_LTLIBRARIES = libpmix.la
libpmix_la_SOURCES = $(headers) $(sources)
libpmix_la_LDFLAGS =

endif

else

lib_LTLIBRARIES = libpmix.la
libpmix_la_SOURCES = $(headers) $(sources)
libpmix_la_LDFLAGS = -version-info $(libpmix_so_version)
libpmix_la_LIBADD += \
        include/libpmixglobal.la
libpmix_la_DEPENDENCIES += \
        include/libpmixglobal.la

endif !PMIX_EMBEDDED_MODE

include atomics/sys/Makefile.include
include threads/Makefile.include
include class/Makefile.include
include event/Makefile.include
include hwloc/Makefile.include
include mca/Makefile.include
include util/Makefile.include
include client/Makefile.include
include server/Makefile.include
include runtime/Makefile.include
include tool/Makefile.include
include tools/Makefile.include
include common/Makefile.include

if WANT_INSTALL_HEADERS
nobase_pmix_HEADERS += $(headers)
endif

CLEANFILES = core.* *~
