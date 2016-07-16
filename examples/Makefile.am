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
# Copyright (c) 2006-2016 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2012-2013 Los Alamos National Security, Inc.  All rights reserved.
# Copyright (c) 2013-2016 Intel, Inc. All rights reserved
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

AM_CPPFLAGS = -I$(top_builddir)/src -I$(top_builddir)/src/include -I$(top_builddir)/src/api

noinst_PROGRAMS = client dmodex dynamic fault pub tool

libpmix = $(top_builddir)/src/libpmix.la

client_SOURCES = client.c
client_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
client_LDADD =  $(libpmix)

dmodex_SOURCES = dmodex.c
dmodex_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
dmodex_LDADD =  $(libpmix)

dynamic_SOURCES = dynamic.c
dynamic_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
dynamic_LDADD = $(libpmix)

fault_SOURCES = fault.c
fault_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
fault_LDADD = $(libpmix)

pub_SOURCES = pub.c
pub_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
pub_LDADD = $(libpmix)

tool_SOURCES = tool.c
tool_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
tool_LDADD = $(libpmix)

distclean-local:
	rm -f *.o client dmodex dynamic fault pub server
