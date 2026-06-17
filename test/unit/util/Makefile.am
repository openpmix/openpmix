#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

check_PROGRAMS = util_hash util_name_fns

TESTS = util_hash util_name_fns

util_hash_SOURCES = util_hash.c
util_hash_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
util_hash_LDADD = \
    $(top_builddir)/src/libpmix.la

util_name_fns_SOURCES = util_name_fns.c
util_name_fns_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
util_name_fns_LDADD = \
    $(top_builddir)/src/libpmix.la

clean-local:
	rm -f util_hash util_name_fns
