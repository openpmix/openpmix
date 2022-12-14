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
# Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
# Copyright (c) 2018      Research Organization for Information Science
#                         and Technology (RIST).  All rights reserved.
# Copyright (c) 2021      IBM Corporation.  All rights reserved.
# Copyright (c) 2022      Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# we do NOT want picky compilers down here
CFLAGS = $(PMIX_CFLAGS_BEFORE_PICKY)
PYTHONPATH=$PYTHONPATH:$(DESTDIR)$(prefix)/lib/python$(PYTHON_VERSION)/site-packages

helpers = setup.py pmix.pyx pmix.pxi construct.py

EXTRA_DIST = $(helpers)

if WANT_PYTHON_BINDINGS

install-exec-local: $(helpers)
	$(PYTHON) $(top_srcdir)/bindings/python/construct.py --src="$(top_builddir)/include" --include-dir="$(top_srcdir)/include"
	PMIX_BINDINGS_TOP_SRCDIR=$(PMIX_TOP_SRCDIR) PMIX_BINDINGS_TOP_BUILDDIR=$(PMIX_TOP_BUILDDIR) \
		$(PYTHON) $(top_srcdir)/bindings/python/setup.py build_ext --library-dirs="$(DESTDIR)$(libdir)" --user
		if [ $(PMIX_TOP_SRCDIR) != $(PMIX_TOP_BUILDDIR) ]; then \
			pushd $(PMIX_TOP_BUILDDIR)/bindings/python; \
			rm -f setup.py; \
			cp $(PMIX_TOP_SRCDIR)/bindings/python/setup.py .; \
			popd; \
		fi
	PMIX_BINDINGS_TOP_SRCDIR=$(PMIX_TOP_SRCDIR) PMIX_BINDINGS_TOP_BUILDDIR=$(PMIX_TOP_BUILDDIR) \
		$(PYTHON) -m pip install --prefix="$(DESTDIR)$(prefix)" --editable $(top_builddir)/bindings/python

uninstall-hook:
	rm -f $(pythondir)/pmix*.so
	rm -f $(pythondir)/pypmix-*.egg-info

CLEANFILES += pmix.c

clean-local:
	rm -rf build pypmix.egg-info dist

endif
