#
# Copyright (c) 2020      Intel, Inc. All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# Header files

AM_CPPFLAGS = $(LTDLINCL) -I$(top_builddir)/src -I$(top_builddir)/src/include -I$(top_builddir)/include -I$(top_builddir)/include/pmix

headers = \
	gds_common.h

# Source files

sources = \
	gds_common.c

lib_LTLIBRARIES =
noinst_LTLIBRARIES =
comp_inst = libmca_common_gds.la
comp_noinst = libmca_common_gds_noinst.la

if MCA_BUILD_pmix_common_gds_DSO
lib_LTLIBRARIES += $(comp_inst)
else
noinst_LTLIBRARIES += $(comp_noinst)
endif

libmca_common_gds_la_SOURCES = $(headers) $(sources)
libmca_common_gds_la_LDFLAGS = -version-info $(libmca_common_gds_so_version)
libmca_common_gds_noinst_la_SOURCES = $(headers) $(sources)

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
