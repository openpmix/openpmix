#
# Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
#                         University Research and Technology
#                         Corporation.  All rights reserved.
# Copyright (c) 2004-2006 The University of Tennessee and The University
#                         of Tennessee Research Foundation.  All rights
#                         reserved.
# Copyright (c) 2004-2009 High Performance Computing Center Stuttgart,
#                         University of Stuttgart.  All rights reserved.
# Copyright (c) 2004-2005 The Regents of the University of California.
#                         All rights reserved.
# Copyright (c) 2006-2020 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2008      Sun Microsystems, Inc.  All rights reserved.
# Copyright (c) 2014      Research Organization for Information Science
#                         and Technology (RIST). All rights reserved.
# Copyright (c) 2019      Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

DEFS="-DEXEEXT=\"$(EXEEXT)\""

MD_FILES = \
        pmix_wrapper.1.md
MD_FILES_NOINST = \
        generic_wrapper.1.md

man_pages_from_md = $(MD_FILES:.1.md=.1)
man_pages_from_md_noinst = $(MD_FILES_NOINST:.1.md=.1)

EXTRA_DIST = $(MD_FILES) $(MD_FILES_NOINST) $(man_pages_from_md) $(man_pages_from_md_noinst)

include $(top_srcdir)/Makefile.pmix-rules

nodist_man_MANS =

if PMIX_INSTALL_BINARIES

bin_PROGRAMS = pmix_wrapper

if PMIX_ENABLE_MAN_PAGES
nodist_man_MANS += $(man_pages_from_md)
endif # PMIX_ENABLE_MAN_PAGES

dist_pmixdata_DATA = help-pmix-wrapper.txt

# Only install the following for developer-level installs
if WANT_INSTALL_HEADERS

nodist_pmixdata_DATA = \
	pmixcc-wrapper-data.txt

if PMIX_ENABLE_MAN_PAGES
nodist_man_MANS += pmixcc.1
endif # PMIX_ENABLE_MAN_PAGES

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = pmix.pc

install-exec-hook:
	(cd $(DESTDIR)$(bindir); rm -f pmixcc$(EXEEXT); $(LN_S) pmix_wrapper$(EXEECT) pmixcc$(EXEEXT))

uninstall-local:
	rm -f $(DESTDIR)$(bindir)/pmixcc$(EXEEXT)

endif # WANT_INSTALL_HEADERS

endif # PMIX_INSTALL_BINARIES

pmix_wrapper_SOURCES = pmix_wrapper.c
pmix_wrapper_LDADD = $(top_builddir)/src/libpmix.la

pmixcc.1: generic_wrapper.1
	rm -f pmixcc.1
	sed -e 's/#COMMAND#/pmixcc/g' -e 's/#PROJECT#/PMIx/g' -e 's/#PROJECT_SHORT#/PMIx/g' -e 's/#LANGUAGE#/C/g' < $(top_builddir)/src/tools/wrapper/generic_wrapper.1 > pmixcc.1
EXTRA_DIST += pmixcc.1

distclean-local:
	rm -f pmixcc.1
