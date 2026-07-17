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
    test_bindings.py

#########################
# Support for "make check"

if WANT_PYTHON_BINDINGS

# test_bindings.py is a self-contained unittest suite that needs no live
# server; it locates the built extension itself and SKIPs (exit 77) if the
# pmix module cannot be imported.  run_server.sh / run_sched.sh exercise the
# connected client/server/scheduler round-trip paths.
TESTS = \
    test_bindings.py \
    run_server.sh \
    run_sched.sh

endif
