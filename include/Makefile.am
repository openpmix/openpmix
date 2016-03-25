#
# Copyright 2015-2016 Intel, Inc. All rights reserved
# Copyright (c) 2016      Mellanox Technologies, Inc.
#                         All rights reserved.
#
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# Only install the headers if we're in standalone mode


if ! PMIX_EMBEDDED_MODE
include_HEADERS = \
        include/pmix.h \
        include/pmix_server.h \
        include/pmi.h \
        include/pmi2.h

include_pmixdir = $(includedir)/pmix
include_pmix_HEADERS = \
    include/pmix/rename.h \
    include/pmix/pmix_common.h

endif ! PMIX_EMBEDDED_MODE
