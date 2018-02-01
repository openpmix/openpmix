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
# Copyright (c) 2013-2018 Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

python_PYTHON = setup.py client.py server.py cpmix.pxd pmix.pyx

if WANT_PYTHON_BINDINGS

bin_PROGRAM = pmix.c

pmix.c: $(python_PYTHON)
	CFLAGS="-I$(top_builddir)/include" \
	LDFLAGS="-L$(DESTDIR)$(libdir)" \
	$(PYTHON) setup.py build_ext -i
	test -z "$(DESTDIR)$(libdir)/python/site-packages" || $(MKDIR_P) "$(DESTDIR)$(libdir)/python/site-packages"
	cp pmix*.so $(DESTDIR)$(libdir)/python/site-packages
	cp build/*/pmix.o $(DESTDIR)$(libdir)/python/site-packages

install: pmix.c

uninstall-hook:
	rm -rf $(DESTDIR)/$(libdir)/python

clean:
	rm -rf *.o build pmix.c

endif
