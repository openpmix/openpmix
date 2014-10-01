#
# PMIx copyrights:
# Copyright (c) 2013      Intel, Inc. All rights reserved
#
#########################
# Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2005 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2005 High Performance Computing Center Stuttgart, 
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2007      Cisco Systems, Inc.  All rights reserved.
#########################
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

# This makefile.am does not stand on its own - it is included from
# src/Makefile.am

headers += \
        src/include/align.h \
	src/include/constants.h \
        src/include/hash_string.h \
	src/include/pmix_socket_errno.h \
	src/include/types.h \
        src/include/pmix_stdint.h \
        src/include/pmix_config_top.h \
        src/include/pmix_config_bottom.h \
        src/include/pmix_portable_platform.h

nodist_headers += \
	src/include/version.h

version.h: $(top_builddir)/config.status $(srcdir)/pmix/version.h.in
	cd $(top_builddir) && $(SHELL) ./config.status $(subdir)/$@
