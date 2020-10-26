#
# Copyright (c) 2020      Intel, Inc. All rights reserved.
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
comp_inst = libmca_common_sse.la
comp_noinst = libmca_common_sse_noinst.la

if MCA_BUILD_pmix_common_sse_DSO
lib_LTLIBRARIES += $(comp_inst)
else
noinst_LTLIBRARIES += $(comp_noinst)
endif

libmca_common_sse_la_SOURCES = $(headers) $(sources)
libmca_common_sse_la_LDFLAGS = -version-info $(libmca_common_sse_so_version) $(pmix_check_curl_LDFLAGS)
libmca_common_sse_la_LIBADD = $(pmix_check_curl_LIBS)
libmca_common_sse_noinst_la_SOURCES = $(headers) $(sources)

# Conditionally install the header files

if WANT_INSTALL_HEADERS
pmixdir = $(pmixincludedir)/$(subdir)
pmix_HEADERS = $(headers)
endif

all-local:
	if test -z "$(lib_LTLIBRARIES)"; then \
		rm -f "$(comp_inst)"; \
		$(LN_S) "$(comp_noinst)" "$(comp_inst)"; \
	fi

clean-local:
	if test -z "$(lib_LTLIBRARIES)"; then \
		rm -f "$(comp_inst)"; \
	fi

MAINTAINERCLEANFILES = sse_lex.c
