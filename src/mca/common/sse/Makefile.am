#
# Copyright (c) 2020      Intel, Inc. All rights reserved.
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# A word of explanation...
#
# This library is linked against various MCA components because all
# REST based components need to
# share some common code and data.  There's two cases:
#
# 1. libmca_common_sse.la is a shared library.  By linking that shared
# library to all components that need it, the OS linker will
# automatically load it into the process as necessary, and there will
# only be one copy (i.e., all the components will share *one* copy of
# the code and data).
#
# 2. libmca_common_sse.la is a static library.  In this case, it will
# be rolled up into the top-level libpmix.la.  It will also be rolled
# into each component, but then the component will also be rolled up
# into the upper-level libpmix.la.  Linkers universally know how to
# "figure this out" so that we end up with only one copy of the code
# and data.
#
# Note that building this common component statically and linking
# against other dynamic components is *not* supported!

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

# As per above, we'll either have an installable or noinst result.
# The installable one should follow the same MCA prefix naming rules
# (i.e., libmca_<type>_<name>.la).  The noinst one can be named
# whatever it wants, although libmca_<type>_<name>_noinst.la is
# recommended.

# To simplify components that link to this library, we will *always*
# have an output libtool library named libmca_<type>_<name>.la -- even
# for case 2) described above (i.e., so there's no conditional logic
# necessary in component Makefile.am's that link to this library).
# Hence, if we're creating a noinst version of this library (i.e.,
# case 2), we sym link it to the libmca_<type>_<name>.la name
# (libtool will do the Right Things under the covers).  See the
# all-local and clean-local rules, below, for how this is effected.

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

# These two rules will sym link the "noinst" libtool library filename
# to the installable libtool library filename in the case where we are
# compiling this component statically (case 2), described above).

# See Makefile.pmix-rules for an explanation of the "V" macros, below
V=0
PMIX_V_LN_SCOMP = $(pmix__v_LN_SCOMP_$V)
pmix__v_LN_SCOMP_ = $(pmix__v_LN_SCOMP_$AM_DEFAULT_VERBOSITY)
pmix__v_LN_SCOMP_0 = @echo "  LN_S    " `basename $(comp_inst)`;

all-local:
	$(PMIX_V_LN_SCOMP) if test -z "$(lib_LTLIBRARIES)"; then \
	  rm -f "$(comp_inst)"; \
	  $(LN_S) "$(comp_noinst)" "$(comp_inst)"; \
	fi

clean-local:
	if test -z "$(lib_LTLIBRARIES)"; then \
		rm -f "$(comp_inst)"; \
	fi

MAINTAINERCLEANFILES = sse_lex.c
