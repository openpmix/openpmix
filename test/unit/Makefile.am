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

noinst_SCRIPTS = run_gds_fallback.pl run_simpcycle.pl run_toolcycle.pl run_toolswitch.pl run_grpmember.pl run_grpbadinfo.pl run_grpinvite.pl run_grpinviteendpts.pl run_grpinviteothers.pl run_grpinvitesuppress.pl run_grpinvitenb.pl run_grptimeout.pl run_grpdecline.pl run_grpabort.pl run_grpmemberfail.pl run_grpleader.pl run_grpcabort.pl run_grpctimeout.pl run_monitor.pl run_tools.pl

EXTRA_DIST = run_gds_fallback.pl.in run_simpcycle.pl.in run_toolcycle.pl.in run_toolswitch.pl.in run_grpmember.pl.in run_grpbadinfo.pl.in run_grpinvite.pl.in run_grpinviteendpts.pl.in run_grpinviteothers.pl.in run_grpinvitesuppress.pl.in run_grpinvitenb.pl.in run_grptimeout.pl.in run_grpdecline.pl.in run_grpabort.pl.in run_grpmemberfail.pl.in run_grpleader.pl.in run_grpcabort.pl.in run_grpctimeout.pl.in run_monitor.pl.in run_tools.pl.in

check_PROGRAMS = client_api client_commit get_api common_api compress compress_block preg bfrops_regex2 bfrops_alloc_inherit bfrops_darray bfrops_malformed bfrops_get_number bfrops_null_object bfrops_helpers nested_darray gds_fallback gds_datastore pmix_log collective_status collect_job_info trk_complete tracker_match trk_peer_lost client_cycle tool_cycle event_chain hwloc_datatype hwloc_devices progress_threads info_support singleton_register rndz_stale iof_pattern iof_flow iof_output iof_pending iof_inherit proc_array_id get_perf ptl_uri ptl_handshake tool_nspace tool_rndz tool_api tool_evcache tool_relay runtime_init server_get server_group resolve_api spawn_api server_control server_events server_dmodex server_inventory server_registration server_ops server_op_replies server_fabric server_fence server_connect server_resolve server_setup attr_dictionary event_forward server_extprogress pgpu_visible_devices pgpu_affinity_mask pgpu_envar_blob pstat_parse pstat_frame pstat_query pnet_assigned_devices pnet_envar_blob pnet_opa_seckey pnet_tcp_ports pnet_simptest_map pmdl_envars plog_routing psec_credentials threads_primitives pif_discovery

TESTS = client_api client_commit get_api common_api compress compress_block preg bfrops_regex2 bfrops_alloc_inherit bfrops_darray bfrops_malformed bfrops_get_number bfrops_null_object bfrops_helpers nested_darray gds_fallback run_gds_fallback.pl run_simpcycle.pl run_toolcycle.pl run_toolswitch.pl run_grpmember.pl run_grpbadinfo.pl run_grpinvite.pl run_grpinviteendpts.pl run_grpinviteothers.pl run_grpinvitesuppress.pl run_grpinvitenb.pl run_grptimeout.pl run_grpdecline.pl run_grpabort.pl run_grpmemberfail.pl run_grpleader.pl run_grpcabort.pl run_grpctimeout.pl run_monitor.pl run_tools.pl pmix_log collective_status collect_job_info trk_complete tracker_match trk_peer_lost client_cycle tool_cycle event_chain hwloc_datatype hwloc_devices progress_threads info_support singleton_register rndz_stale iof_pattern iof_flow iof_output iof_pending iof_inherit proc_array_id gds_datastore get_perf ptl_uri ptl_handshake tool_nspace tool_rndz tool_api tool_evcache tool_relay runtime_init server_get server_group resolve_api spawn_api server_control server_events server_dmodex server_inventory server_registration server_ops server_op_replies server_fabric server_fence server_connect server_resolve server_setup attr_dictionary event_forward server_extprogress pgpu_visible_devices pgpu_affinity_mask pgpu_envar_blob pstat_parse pstat_frame pstat_query pnet_assigned_devices pnet_envar_blob pnet_opa_seckey pnet_tcp_ports pnet_simptest_map pmdl_envars plog_routing psec_credentials threads_primitives pif_discovery

client_api_SOURCES = \
        client_api.c
client_api_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
client_api_LDADD = \
    $(top_builddir)/src/libpmix.la

client_commit_SOURCES = \
        client_commit.c
client_commit_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
client_commit_LDADD = \
    $(top_builddir)/src/libpmix.la

get_api_SOURCES = \
        get_api.c
get_api_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
get_api_LDADD = \
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

compress_block_SOURCES =  \
        compress_block.c
compress_block_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
compress_block_LDADD = \
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

# The attribute dictionary and the key index it seeds. See the header
# comment in attr_dictionary.c for why the ids matter.
attr_dictionary_SOURCES = \
        attr_dictionary.c
attr_dictionary_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
attr_dictionary_LDADD = \
    $(top_builddir)/src/libpmix.la

proc_array_id_SOURCES = \
        proc_array_id.c
proc_array_id_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
proc_array_id_LDADD = \
    $(top_builddir)/src/libpmix.la

# End-to-end PMIx_Get timings. Asserts correctness only and prints
# timings; see the header comment in get_perf.c.
get_perf_SOURCES = \
        get_perf.c
get_perf_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
get_perf_LDADD = \
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

trk_peer_lost_SOURCES = \
        trk_peer_lost.c
trk_peer_lost_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
trk_peer_lost_LDADD = \
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

hwloc_devices_SOURCES = \
        hwloc_devices.c
hwloc_devices_CPPFLAGS = -DPMIX_TEST_TOPO_DIR=\"$(abs_top_srcdir)/test/topologies\"
hwloc_devices_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
hwloc_devices_LDADD = \
    $(top_builddir)/src/libpmix.la

pgpu_visible_devices_SOURCES = \
        pgpu_visible_devices.c
pgpu_visible_devices_CPPFLAGS = -DPMIX_TEST_TOPO_DIR=\"$(abs_top_srcdir)/test/topologies\"
pgpu_visible_devices_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
pgpu_visible_devices_LDADD = \
    $(top_builddir)/src/libpmix.la

pnet_assigned_devices_SOURCES = \
        pnet_assigned_devices.c
pnet_assigned_devices_CPPFLAGS = -DPMIX_TEST_TOPO_DIR=\"$(abs_top_srcdir)/test/topologies\"
pnet_assigned_devices_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
pnet_assigned_devices_LDADD = \
    $(top_builddir)/src/libpmix.la

pnet_envar_blob_SOURCES = \
        pnet_envar_blob.c
pnet_envar_blob_CPPFLAGS = -DPMIX_TEST_TOPO_DIR=\"$(abs_top_srcdir)/test/topologies\"
pnet_envar_blob_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
pnet_envar_blob_LDADD = \
    $(top_builddir)/src/libpmix.la

pnet_opa_seckey_SOURCES = \
        pnet_opa_seckey.c
pnet_opa_seckey_CPPFLAGS = -DPMIX_TEST_TOPO_DIR=\"$(abs_top_srcdir)/test/topologies\"
pnet_opa_seckey_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
pnet_opa_seckey_LDADD = \
    $(top_builddir)/src/libpmix.la

pnet_tcp_ports_SOURCES = \
        pnet_tcp_ports.c
pnet_tcp_ports_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
pnet_tcp_ports_LDADD = \
    $(top_builddir)/src/libpmix.la

pnet_simptest_map_SOURCES = \
        pnet_simptest_map.c
pnet_simptest_map_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
pnet_simptest_map_LDADD = \
    $(top_builddir)/src/libpmix.la

pgpu_affinity_mask_SOURCES = \
        pgpu_affinity_mask.c
pgpu_affinity_mask_CPPFLAGS = -DPMIX_TEST_TOPO_DIR=\"$(abs_top_srcdir)/test/topologies\"
pgpu_affinity_mask_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
pgpu_affinity_mask_LDADD = \
    $(top_builddir)/src/libpmix.la

plog_routing_SOURCES = \
        plog_routing.c
plog_routing_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
plog_routing_LDADD = \
    $(top_builddir)/src/libpmix.la

# The psec framework's credential contract: PMIX_CRED_TYPE screening, the
# *info output array, and the native create/validate round trip. See the
# header comment in psec_credentials.c.
psec_credentials_SOURCES = \
        psec_credentials.c
psec_credentials_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
psec_credentials_LDADD = \
    $(top_builddir)/src/libpmix.la

pmdl_envars_SOURCES = \
        pmdl_envars.c
pmdl_envars_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
pmdl_envars_LDADD = \
    $(top_builddir)/src/libpmix.la

pgpu_envar_blob_SOURCES = \
        pgpu_envar_blob.c
pgpu_envar_blob_CPPFLAGS = -DPMIX_TEST_TOPO_DIR=\"$(abs_top_srcdir)/test/topologies\"
pgpu_envar_blob_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
pgpu_envar_blob_LDADD = \
    $(top_builddir)/src/libpmix.la

server_extprogress_SOURCES = \
        server_extprogress.c
server_extprogress_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
server_extprogress_LDADD = \
    $(top_builddir)/src/libpmix.la

runtime_init_SOURCES = \
        runtime_init.c
runtime_init_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
runtime_init_LDADD = \
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

iof_flow_SOURCES = \
        iof_flow.c
iof_flow_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
iof_flow_LDADD = \
    $(top_builddir)/src/libpmix.la

iof_output_SOURCES = \
        iof_output.c
iof_output_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
iof_output_LDADD = \
    $(top_builddir)/src/libpmix.la

iof_pending_SOURCES = \
        iof_pending.c
iof_pending_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
iof_pending_LDADD = \
    $(top_builddir)/src/libpmix.la

iof_inherit_SOURCES = \
        iof_inherit.c
iof_inherit_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
iof_inherit_LDADD = \
    $(top_builddir)/src/libpmix.la

# The pstat framework's request-parsing helpers. See the header comment in
# pstat_parse.c for why the malformed cases matter.
pstat_parse_SOURCES = \
        pstat_parse.c
pstat_parse_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
pstat_parse_LDADD = \
    $(top_builddir)/src/libpmix.la

# The pstat framework's open/close lifecycle, driven with the dedicated
# progress thread turned on. See the header comment in pstat_frame.c.
pstat_frame_SOURCES = \
        pstat_frame.c
pstat_frame_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
pstat_frame_LDADD = \
    $(top_builddir)/src/libpmix.la

# The pstat framework's query contract, including the malformed requests
# that arrive off the wire. See the header comment in pstat_query.c.
pstat_query_SOURCES = \
        pstat_query.c
pstat_query_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
pstat_query_LDADD = \
    $(top_builddir)/src/libpmix.la

clean-local:
	rm -f compress compress_block preg bfrops_regex2 bfrops_alloc_inherit nested_darray gds_fallback gds_datastore pmix_log collective_status collect_job_info trk_complete tracker_match trk_peer_lost client_cycle tool_cycle event_chain hwloc_datatype hwloc_devices progress_threads info_support singleton_register rndz_stale iof_pattern iof_flow iof_output iof_pending iof_inherit proc_array_id get_perf attr_dictionary pstat_parse pstat_frame pstat_query

ptl_uri_SOURCES = \
        ptl_uri.c
ptl_uri_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
ptl_uri_LDADD = \
    $(top_builddir)/src/libpmix.la

ptl_handshake_SOURCES = \
        ptl_handshake.c
ptl_handshake_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
ptl_handshake_LDADD = \
    $(top_builddir)/src/libpmix.la

tool_nspace_SOURCES = \
        tool_nspace.c
tool_nspace_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
tool_nspace_LDADD = \
    $(top_builddir)/src/libpmix.la

tool_rndz_SOURCES = \
        tool_rndz.c
tool_rndz_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
tool_rndz_LDADD = \
    $(top_builddir)/src/libpmix.la

tool_api_SOURCES = \
        tool_api.c
tool_api_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
tool_api_LDADD = \
    $(top_builddir)/src/libpmix.la

tool_evcache_SOURCES = \
        tool_evcache.c
tool_evcache_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
tool_evcache_LDADD = \
    $(top_builddir)/src/libpmix.la

event_forward_SOURCES = \
        event_forward.c
event_forward_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
event_forward_LDADD = \
    $(top_builddir)/src/libpmix.la

tool_relay_SOURCES = \
        tool_relay.c
tool_relay_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
tool_relay_LDADD = \
    $(top_builddir)/src/libpmix.la

server_get_SOURCES = \
        server_get.c
server_get_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
server_get_LDADD = \
    $(top_builddir)/src/libpmix.la

server_group_SOURCES = \
        server_group.c
server_group_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
server_group_LDADD = \
    $(top_builddir)/src/libpmix.la

server_ops_SOURCES = \
        server_ops.c
server_ops_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
server_ops_LDADD = \
    $(top_builddir)/src/libpmix.la

resolve_api_SOURCES = \
        resolve_api.c
resolve_api_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
resolve_api_LDADD = \
    $(top_builddir)/src/libpmix.la

spawn_api_SOURCES = \
        spawn_api.c
spawn_api_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
spawn_api_LDADD = \
    $(top_builddir)/src/libpmix.la

server_control_SOURCES = \
        server_control.c
server_control_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
server_control_LDADD = \
    $(top_builddir)/src/libpmix.la

server_events_SOURCES = \
        server_events.c
server_events_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
server_events_LDADD = \
    $(top_builddir)/src/libpmix.la

server_connect_SOURCES = \
        server_connect.c
server_connect_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
server_connect_LDADD = \
    $(top_builddir)/src/libpmix.la

server_resolve_SOURCES = \
        server_resolve.c
server_resolve_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
server_resolve_LDADD = \
    $(top_builddir)/src/libpmix.la

server_setup_SOURCES = \
        server_setup.c
server_setup_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
server_setup_LDADD = \
    $(top_builddir)/src/libpmix.la

server_fence_SOURCES = \
        server_fence.c
server_fence_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
server_fence_LDADD = \
    $(top_builddir)/src/libpmix.la

server_dmodex_SOURCES = \
        server_dmodex.c
server_dmodex_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
server_dmodex_LDADD = \
    $(top_builddir)/src/libpmix.la

server_inventory_SOURCES = \
        server_inventory.c
server_inventory_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
server_inventory_LDADD = \
    $(top_builddir)/src/libpmix.la

server_registration_SOURCES = \
        server_registration.c
server_registration_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
server_registration_LDADD = \
    $(top_builddir)/src/libpmix.la

server_op_replies_SOURCES = \
        server_op_replies.c
server_op_replies_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
server_op_replies_LDADD = \
    $(top_builddir)/src/libpmix.la

server_fabric_SOURCES = \
        server_fabric.c
server_fabric_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
server_fabric_LDADD = \
    $(top_builddir)/src/libpmix.la

threads_primitives_SOURCES = \
        threads_primitives.c
threads_primitives_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
threads_primitives_LDADD = \
    $(top_builddir)/src/libpmix.la

pif_discovery_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
pif_discovery_LDADD = \
    $(top_builddir)/src/libpmix.la
