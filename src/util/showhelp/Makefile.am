#
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
# Copyright (c) 2016      Intel, Inc. All rights reserved
# Copyright (c) 2021      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#


AM_LFLAGS = -Ppmix_showhelp_yy
# we do NOT want picky compilers down here
CFLAGS = $(PMIX_CFLAGS_BEFORE_PICKY)

LEX_OUTPUT_ROOT = lex.pmix_showhelp_yy

noinst_LTLIBRARIES = libpmixutilshowhelp.la

# Source code files

libpmixutilshowhelp_la_SOURCES = \
	showhelp_lex.h \
    showhelp_lex.l

MAINTAINERCLEANFILES = showhelp_lex.c
