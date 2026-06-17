#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

check_PROGRAMS = util_hash util_name_fns \
    util_output util_error util_cmd_line \
    util_show_help util_timings util_context_fns

TESTS = util_hash util_name_fns \
    util_output util_error util_cmd_line \
    util_show_help util_timings util_context_fns

util_hash_SOURCES = util_hash.c
util_hash_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
util_hash_LDADD = \
    $(top_builddir)/src/libpmix.la

util_name_fns_SOURCES = util_name_fns.c
util_name_fns_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
util_name_fns_LDADD = \
    $(top_builddir)/src/libpmix.la

util_output_SOURCES = util_output.c
util_output_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
util_output_LDADD = \
    $(top_builddir)/src/libpmix.la

util_error_SOURCES = util_error.c
util_error_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
util_error_LDADD = \
    $(top_builddir)/src/libpmix.la

util_cmd_line_SOURCES = util_cmd_line.c
util_cmd_line_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
util_cmd_line_LDADD = \
    $(top_builddir)/src/libpmix.la

util_show_help_SOURCES = util_show_help.c
util_show_help_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
util_show_help_LDADD = \
    $(top_builddir)/src/libpmix.la

util_timings_SOURCES = util_timings.c
util_timings_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
util_timings_LDADD = \
    $(top_builddir)/src/libpmix.la

util_context_fns_SOURCES = util_context_fns.c
util_context_fns_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
util_context_fns_LDADD = \
    $(top_builddir)/src/libpmix.la

clean-local:
	rm -f util_hash util_name_fns \
	    util_output util_error util_cmd_line \
	    util_show_help util_timings util_context_fns
