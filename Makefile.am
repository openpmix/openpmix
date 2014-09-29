# PMIx copyrights:
# Copyright (c) 2013      Intel, Inc. All rights reserved
#
#########################
# This code has been adapted from opal_get_version.m4sh in the Open MPI
# code base - per the Open MPI license, all copyrights are retained below.
#
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
#########################
# $COPYRIGHT$
# 
# Additional copyrights may follow
# 
# $HEADER$
#

ACLOCAL_AMFLAGS = -I ./config

SUBDIRS = config src
EXTRA_DIST = README INSTALL VERSION LICENSE autogen.sh \
             config/pmix_get_version.sh

# setup the distribution tarball
DIST_SUBDIRS = $(SUBDIRS)


dist-hook:
	csh "$(top_srcdir)/config/distscript.csh" "$(top_srcdir)" "$(distdir)" "$(OMPI_VERSION)" "$(OMPI_SVN_R)"

