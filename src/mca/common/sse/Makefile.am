#
# Copyright (c) 2020      Intel, Inc. All rights reserved.
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# Header files

AM_CPPFLAGS = $(LTDLINCL) -I$(top_builddir)/src -I$(top_builddir)/src/include -I$(top_builddir)/include -I$(top_builddir)/include/pmix $(pmix_check_curl_CPPFLAGS)
AM_LFLAGS = -Ppmix_util_sse_yy
LEX_OUTPUT_ROOT = lex.pmix_util_sse_yy

headers = \
	sse.h \
	sse_internal.h

# Source files

sources = \
	sse.c \
    sse_lex.l

lib_LTLIBRARIES =
noinst_LTLIBRARIES =

if MCA_BUILD_pmix_common_sse_DSO
lib_LTLIBRARIES += libmca_common_sse.la
else
noinst_LTLIBRARIES += libmca_common_sse.la
endif

libmca_common_sse_la_SOURCES = $(headers) $(sources)
libmca_common_sse_la_LDFLAGS = $(pmix_check_curl_LDFLAGS)
if MCA_BUILD_pmix_common_sse_DSO
libmca_common_sse_la_LDFLAGS += -version-info $(libmca_common_sse_so_version)
endif
libmca_common_sse_la_LIBADD = $(pmix_check_curl_LIBS)

# Conditionally install the header files

if WANT_INSTALL_HEADERS
pmixdir = $(pmixincludedir)/$(subdir)
pmix_HEADERS = $(headers)
endif

MAINTAINERCLEANFILES = sse_lex.c
