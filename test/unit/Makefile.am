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
# Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
# Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
# Copyright (c) 2026      Jeff Squyres  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

SUBDIRS = util class mca

# Point the tests at the components in the build tree so that "make
# check" does not require a prior "make install". See the comments in
# test/pmix_test_env.sh.in.
# Run this directory's tests one at a time, even under "make -j".
#
# Most of TESTS below drives a real PMIx server through test/simple's
# simptest, and simptest comes up in the *scheduler* role. A scheduler is
# a node-wide singleton: it claims a fixed rendezvous file
# ($TMPDIR/pmix.sched.<host>), and the second server to want that file is
# refused by design, with "Only one server can fill a given role on a node
# at a time". Automake's parallel test harness gives each test its own
# .log target, so "make -j check" starts several of them at once and all
# but one lose the race. The failure is not flaky and not environmental --
# it is the server correctly enforcing its own invariant.
#
# This was invisible for as long as test/Makefile.am's SUBDIRS was gated
# on !WANT_HIDDEN, because the CI job that builds with "make -j check"
# configures with default visibility and so never descended in here.
#
# Do not "fix" this by giving each test a private TMPDIR to hide the
# collision: two schedulers on one node is precisely the configuration
# PMIx forbids, and a test suite that arranges one is testing something
# users cannot have. If you want these to run concurrently, they need to
# stop being schedulers, not stop seeing each other.
.NOTPARALLEL:

AM_TESTS_ENVIRONMENT = . $(abs_top_builddir)/test/pmix_test_env.sh;

noinst_SCRIPTS = run_gds_fallback.pl run_simpcycle.pl run_toolcycle.pl run_toolswitch.pl run_grpmember.pl run_grpbadinfo.pl run_grpinvite.pl run_grpinviteothers.pl run_grpinvitesuppress.pl run_grpinvitenb.pl run_grptimeout.pl run_grpdecline.pl run_grpabort.pl run_grpmemberfail.pl run_grpleader.pl run_grpcabort.pl run_grpctimeout.pl run_monitor.pl run_tools.pl

EXTRA_DIST = run_gds_fallback.pl.in run_simpcycle.pl.in run_toolcycle.pl.in run_toolswitch.pl.in run_grpmember.pl.in run_grpbadinfo.pl.in run_grpinvite.pl.in run_grpinviteothers.pl.in run_grpinvitesuppress.pl.in run_grpinvitenb.pl.in run_grptimeout.pl.in run_grpdecline.pl.in run_grpabort.pl.in run_grpmemberfail.pl.in run_grpleader.pl.in run_grpcabort.pl.in run_grpctimeout.pl.in run_monitor.pl.in run_tools.pl.in

check_PROGRAMS = client_api common_api compress preg bfrops_regex2 bfrops_alloc_inherit bfrops_darray bfrops_malformed bfrops_get_number bfrops_null_object bfrops_helpers nested_darray gds_fallback gds_datastore pmix_log collective_status collect_job_info trk_complete tracker_match client_cycle tool_cycle event_chain hwloc_datatype progress_threads info_support singleton_register rndz_stale iof_pattern proc_array_id

TESTS = client_api common_api compress preg bfrops_regex2 bfrops_alloc_inherit bfrops_darray bfrops_malformed bfrops_get_number bfrops_null_object bfrops_helpers nested_darray gds_fallback run_gds_fallback.pl run_simpcycle.pl run_toolcycle.pl run_toolswitch.pl run_grpmember.pl run_grpbadinfo.pl run_grpinvite.pl run_grpinviteothers.pl run_grpinvitesuppress.pl run_grpinvitenb.pl run_grptimeout.pl run_grpdecline.pl run_grpabort.pl run_grpmemberfail.pl run_grpleader.pl run_grpcabort.pl run_grpctimeout.pl run_monitor.pl run_tools.pl pmix_log collective_status collect_job_info trk_complete tracker_match client_cycle tool_cycle event_chain hwloc_datatype progress_threads info_support singleton_register rndz_stale iof_pattern proc_array_id

client_api_SOURCES = \
        client_api.c
client_api_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
client_api_LDADD = \
    $(top_builddir)/src/libpmix.la

common_api_SOURCES = \
        common_api.c
common_api_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
common_api_LDADD = \
    $(top_builddir)/src/libpmix.la

compress_SOURCES =  \
        compress.c
compress_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
compress_LDADD = \
    $(top_builddir)/src/libpmix.la

preg_SOURCES = \
        preg.c
preg_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
preg_LDADD = \
    $(top_builddir)/src/libpmix.la

bfrops_regex2_SOURCES = \
        bfrops_regex2.c
bfrops_regex2_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
bfrops_regex2_LDADD = \
    $(top_builddir)/src/libpmix.la

bfrops_alloc_inherit_SOURCES = \
        bfrops_alloc_inherit.c
bfrops_alloc_inherit_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
bfrops_alloc_inherit_LDADD = \
    $(top_builddir)/src/libpmix.la

bfrops_darray_SOURCES = \
        bfrops_darray.c
bfrops_darray_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
bfrops_darray_LDADD = \
    $(top_builddir)/src/libpmix.la

bfrops_get_number_SOURCES = \
        bfrops_get_number.c
bfrops_get_number_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
bfrops_get_number_LDADD = \
    $(top_builddir)/src/libpmix.la

bfrops_null_object_SOURCES = \
        bfrops_null_object.c
bfrops_null_object_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
bfrops_null_object_LDADD = \
    $(top_builddir)/src/libpmix.la

bfrops_helpers_SOURCES = \
        bfrops_helpers.c
bfrops_helpers_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
bfrops_helpers_LDADD = \
    $(top_builddir)/src/libpmix.la

bfrops_malformed_SOURCES = \
        bfrops_malformed.c
bfrops_malformed_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
bfrops_malformed_LDADD = \
    $(top_builddir)/src/libpmix.la

proc_array_id_SOURCES = \
        proc_array_id.c
proc_array_id_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
proc_array_id_LDADD = \
    $(top_builddir)/src/libpmix.la

nested_darray_SOURCES = \
        nested_darray.c
nested_darray_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
nested_darray_LDADD = \
    $(top_builddir)/src/libpmix.la

gds_fallback_SOURCES = \
        gds_fallback.c
gds_fallback_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
gds_fallback_LDADD = \
    $(top_builddir)/src/libpmix.la

gds_datastore_SOURCES = \
        gds_datastore.c
gds_datastore_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
gds_datastore_LDADD = \
    $(top_builddir)/src/libpmix.la

pmix_log_SOURCES = \
        pmix_log.c
pmix_log_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
pmix_log_LDADD = \
    $(top_builddir)/src/libpmix.la

collective_status_SOURCES = \
        collective_status.c
collective_status_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
collective_status_LDADD = \
    $(top_builddir)/src/libpmix.la

collect_job_info_SOURCES = \
        collect_job_info.c
collect_job_info_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
collect_job_info_LDADD = \
    $(top_builddir)/src/libpmix.la

trk_complete_SOURCES = \
        trk_complete.c
trk_complete_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
trk_complete_LDADD = \
    $(top_builddir)/src/libpmix.la

tracker_match_SOURCES = \
        tracker_match.c
tracker_match_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
tracker_match_LDADD = \
    $(top_builddir)/src/libpmix.la

client_cycle_SOURCES = \
        client_cycle.c
client_cycle_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
client_cycle_LDADD = \
    $(top_builddir)/src/libpmix.la

tool_cycle_SOURCES = \
        tool_cycle.c
tool_cycle_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
tool_cycle_LDADD = \
    $(top_builddir)/src/libpmix.la

event_chain_SOURCES = \
        event_chain.c
event_chain_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
event_chain_LDADD = \
    $(top_builddir)/src/libpmix.la

hwloc_datatype_SOURCES = \
        hwloc_datatype.c
hwloc_datatype_CPPFLAGS = -DPMIX_TEST_TOPO_DIR=\"$(abs_top_srcdir)/test/topologies\"
hwloc_datatype_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
hwloc_datatype_LDADD = \
    $(top_builddir)/src/libpmix.la

progress_threads_SOURCES = \
        progress_threads.c
progress_threads_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
progress_threads_LDADD = \
    $(top_builddir)/src/libpmix.la

info_support_SOURCES = \
        info_support.c
info_support_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
info_support_LDADD = \
    $(top_builddir)/src/libpmix.la

singleton_register_SOURCES = \
        singleton_register.c
singleton_register_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
singleton_register_LDADD = \
    $(top_builddir)/src/libpmix.la

rndz_stale_SOURCES = \
        rndz_stale.c
rndz_stale_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
rndz_stale_LDADD = \
    $(top_builddir)/src/libpmix.la

iof_pattern_SOURCES = \
        iof_pattern.c
iof_pattern_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
iof_pattern_LDADD = \
    $(top_builddir)/src/libpmix.la

clean-local:
	rm -f compress preg bfrops_regex2 bfrops_alloc_inherit nested_darray gds_fallback gds_datastore pmix_log collective_status collect_job_info trk_complete tracker_match client_cycle tool_cycle event_chain hwloc_datatype progress_threads info_support singleton_register rndz_stale iof_pattern proc_array_id
