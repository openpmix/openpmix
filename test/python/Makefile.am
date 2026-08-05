#
# Copyright (c) 2019      Intel, Inc.  All rights reserved.
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# we do NOT want picky compilers down here
CFLAGS = $(PMIX_CFLAGS_BEFORE_PICKY)

noinst_SCRIPTS = \
	run_server.sh.in \
	run_sched.sh.in \
    server.py \
    sched.py \
    client.py \
    tool.py \
    test_bindings.py \
    test_construct.py

#########################
# Support for "make check"

if WANT_PYTHON_BINDINGS

# test_bindings.py is a self-contained unittest suite that needs no live
# server; it locates the built extension itself and SKIPs (exit 77) if the
# pmix module cannot be imported.  test_construct.py checks the constants
# generator and needs neither the extension nor the library, only Python.
# run_server.sh / run_sched.sh exercise the connected
# client/server/scheduler round-trip paths.
TESTS = \
    test_bindings.py \
    test_construct.py \
    run_server.sh \
    run_sched.sh

# The extension is linked against libpmix but carries no rpath -- setup.py
# is given --library-dirs, which is a *link* path, not a run-time one. So
# importing it finds the module and then fails to load libpmix.so, and the
# only test that reports that clearly is the one round-trip that dies on
# it: test_bindings.py treats an unimportable module as "nothing to test"
# and SKIPs, which looks like a pass. Point the loader at the library we
# just built.
#
# DYLD_LIBRARY_PATH is set for macOS, with the caveat that System
# Integrity Protection strips DYLD_* when it executes a protected binary,
# so it only takes effect for a python3 outside /usr/bin.
# PYTHONPATH likewise has to name the *build* tree. test_bindings.py
# falls back to locating the extension itself, but it does so relative to
# its own __file__ -- which is in the source tree, where a VPATH build
# puts no extension. It then reports "No module named 'pmix'" and SKIPs,
# so the whole bindings suite silently tested nothing out of tree.
AM_TESTS_ENVIRONMENT = \
    LD_LIBRARY_PATH=$(abs_top_builddir)/src/.libs$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}; \
    export LD_LIBRARY_PATH; \
    DYLD_LIBRARY_PATH=$(abs_top_builddir)/src/.libs$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH}; \
    export DYLD_LIBRARY_PATH; \
    PYTHONPATH=`echo $(abs_top_builddir)/bindings/python/build/lib.*`$${PYTHONPATH:+:$$PYTHONPATH}; \
    export PYTHONPATH;

endif
