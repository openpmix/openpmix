#
# Copyright (c) 2020      Intel, Inc.  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

AM_CPPFLAGS = -I$(top_builddir)/src -I$(top_builddir)/src/include -I$(top_builddir)/include -I$(top_builddir)/include/pmix $(pmix_check_jansson_CPPFLAGS)

if HAVE_JANSSON
noinst_PROGRAMS = generate server daemon testcoord

generate_SOURCES = \
        generate.c
generate_LDFLAGS = $(pmix_check_jansson_LDFLAGS)
generate_LDADD = $(pmix_check_jansson_LIBS)

server_SOURCES = \
        server.c
server_LDFLAGS = $(pmix_check_jansson_LDFLAGS)
server_LDADD = $(pmix_check_jansson_LIBS)

daemon_SOURCES = \
        daemon.c
daemon_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
daemon_LDADD = \
    $(top_builddir)/src/libpmix.la

testcoord_SOURCES = \
        testcoord.c
testcoord_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
testcoord_LDADD = \
    $(top_builddir)/src/libpmix.la
endif
