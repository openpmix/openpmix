#
# Copyright 2015      Intel, Inc. All rights reserved
#
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

# Only install the headers if we're in standalone mode

if PMIX_BUILD_STANDALONE
include_HEADERS = include/pmix.h include/pmix_server.h include/pmi.h include/pmi2.h
include_pmixdir = $(includedir)/pmix
include_pmix_HEADERS = \
	include/pmix/pmix_common.h \
	include/pmix/rename.h

include_pmix_autogendir = $(includedir)/pmix/autogen
nodist_include_pmix_autogen_HEADERS = include/pmix/autogen/config.h

noinst_HEADERS = \
	include/private/align.h
	include/private/hash_string.h \
	include/private/pmix_socket_errno.h \
	include/private/pmix_stding.h \
	include/private/prefetch.h \
	include/private/types.h

server_headers += \
	include/pmix_server.h

EXTRA_DIST += \
	include/pmix/pmix_common.h.in

endif PMIX_BUILD_STANDALONE
