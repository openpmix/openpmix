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

SUBDIRS = util class

noinst_SCRIPTS = run_gds_fallback.pl run_simpcycle.pl run_toolcycle.pl run_toolswitch.pl run_grpmember.pl run_grpinvite.pl run_grptimeout.pl run_grpdecline.pl run_grpabort.pl run_grpmemberfail.pl run_grpleader.pl run_grpcabort.pl run_grpctimeout.pl run_monitor.pl

EXTRA_DIST = run_gds_fallback.pl.in run_simpcycle.pl.in run_toolcycle.pl.in run_toolswitch.pl.in run_grpmember.pl.in run_grpinvite.pl.in run_grptimeout.pl.in run_grpdecline.pl.in run_grpabort.pl.in run_grpmemberfail.pl.in run_grpleader.pl.in run_grpcabort.pl.in run_grpctimeout.pl.in run_monitor.pl.in

check_PROGRAMS = compress preg bfrops_regex2 bfrops_alloc_inherit gds_fallback pmix_log collective_status collect_job_info trk_complete tracker_match client_cycle tool_cycle event_chain hwloc_datatype progress_threads info_support

TESTS = compress preg bfrops_regex2 bfrops_alloc_inherit gds_fallback run_gds_fallback.pl run_simpcycle.pl run_toolcycle.pl run_toolswitch.pl run_grpmember.pl run_grpinvite.pl run_grptimeout.pl run_grpdecline.pl run_grpabort.pl run_grpmemberfail.pl run_grpleader.pl run_grpcabort.pl run_grpctimeout.pl run_monitor.pl pmix_log collective_status collect_job_info trk_complete tracker_match client_cycle tool_cycle event_chain hwloc_datatype progress_threads info_support

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

gds_fallback_SOURCES = \
        gds_fallback.c
gds_fallback_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
gds_fallback_LDADD = \
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

clean-local:
	rm -f compress preg bfrops_regex2 bfrops_alloc_inherit gds_fallback pmix_log collective_status collect_job_info trk_complete tracker_match client_cycle tool_cycle event_chain hwloc_datatype progress_threads info_support
