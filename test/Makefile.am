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
# Copyright (c) 2006-2010 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2012-2013 Los Alamos National Security, Inc.  All rights reserved.
# Copyright (c) 2013-2015 Intel, Inc. All rights reserved
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

headers = test_common.h

AM_CPPFLAGS = -I$(top_builddir)/src -I$(top_builddir)/src/include -I$(top_builddir)/src/api

bin_PROGRAMS = pmix_test pmix_client pmix_test_lite pmi_client pmi2_client

check_PROGRAMS = pmix_test pmix_test_lite

TESTS = \
	$(check_PROGRAMS)

pmix_test_SOURCES = \
        pmix_test.c test_common.c

pmix_test_LDADD = \
	$(top_builddir)/libpmix-server.la 

pmix_test_lite_SOURCES = \
        pmix_test_lite.c test_common.c

pmix_test_lite_LDADD = \
	$(top_builddir)/libpmix-server.la 

pmi_client_SOURCES = \
        pmi_client.c 

pmi_client_LDADD = \
	$(top_builddir)/libpmix.la

pmi2_client_SOURCES = \
        pmi2_client.c

pmi2_client_LDADD = \
	$(top_builddir)/libpmix.la

pmix_client_SOURCES = \
        pmix_client.c test_common.c

pmix_client_LDADD = \
	$(top_builddir)/libpmix.la
