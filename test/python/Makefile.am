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
# LD_LIBRARY_PATH is what makes that work on Linux.
#
# DYLD_LIBRARY_PATH is here for macOS and *cannot* reach the test from
# here, which is worth knowing before trying: System Integrity Protection
# strips every DYLD_* variable as it executes a protected binary, and
# automake reaches a test through several of them - the recipe's /bin/sh,
# then the /bin/sh running test-driver. Whatever is set here is gone by
# the time the test starts. It is left set because it costs nothing and
# is correct on any platform without SIP.
#
# The two .sh tests therefore set it themselves, inside their own shell,
# and name $(PYTHON) outright rather than reaching it through a
# "#!/usr/bin/env python3" shebang (/usr/bin/env is protected too); see
# run_server.sh.in. test_bindings.py has no such wrapper, so on macOS it
# still cannot load the extension and reports SKIP - which is the silent
# "tested nothing" this block exists to prevent, and wants the same
# wrapper treatment.
# PYTHONPATH likewise has to name the *build* tree. test_bindings.py
# falls back to locating the extension itself, but it does so relative to
# its own __file__ -- which is in the source tree, where a VPATH build
# puts no extension. It then reports "No module named 'pmix'" and SKIPs,
# so the whole bindings suite silently tested nothing out of tree.
# Run the .py tests through $(PYTHON) rather than their shebang -- see
# the DYLD_LIBRARY_PATH note above. Without this, test_bindings.py loses
# the library path, cannot load the extension, and reports SKIP: a test
# that silently tested nothing, which is what this whole block exists to
# prevent.
AM_TESTS_ENVIRONMENT = \
    LD_LIBRARY_PATH=$(abs_top_builddir)/src/.libs$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}; \
    export LD_LIBRARY_PATH; \
    DYLD_LIBRARY_PATH=$(abs_top_builddir)/src/.libs$${DYLD_LIBRARY_PATH:+:$$DYLD_LIBRARY_PATH}; \
    export DYLD_LIBRARY_PATH; \
    PYTHONPATH=`echo $(abs_top_builddir)/bindings/python/build/lib.*`$${PYTHONPATH:+:$$PYTHONPATH}; \
    export PYTHONPATH;

endif
