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
# Copyright (c) 2013-2019 Intel, Inc.  All rights reserved.
# Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
# Copyright (c) 2022      IBM Corporation. All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
headers = examples.h

# The Python ports of these examples. They are not built - they are
# scripts - but they must ship in the tarball.
EXTRA_DIST = \
	python/README.md \
	python/examples.py \
	python/abi_no_init.py \
	python/abi_with_init.py \
	python/abort.py \
	python/alloc.py \
	python/asyncgroup.py \
	python/bad_exit.py \
	python/client.py \
	python/client2.py \
	python/client3.py \
	python/client4.py \
	python/connect_die.py \
	python/debugger.py \
	python/debuggerd.py \
	python/dmodex.py \
	python/dynamic.py \
	python/fault.py \
	python/group.py \
	python/group_bootstrap.py \
	python/group_construct_abort.py \
	python/group_daemon_fail.py \
	python/group_destruct_die.py \
	python/group_die.py \
	python/group_dmodex.py \
	python/group_invite.py \
	python/group_invite_abort.py \
	python/group_invite_decline.py \
	python/group_invite_timeout.py \
	python/group_lcl_cid.py \
	python/group_leave.py \
	python/hello.py \
	python/jctrl.py \
	python/launcher.py \
	python/log.py \
	python/monitor.py \
	python/monitor_multi.py \
	python/monitor_remote.py \
	python/multi_nspace_group.py \
	python/nodeinfo.py \
	python/pset.py \
	python/pub.py \
	python/pub2.py \
	python/pubi.py \
	python/pubstress.py \
	python/resolve.py \
	python/server.py \
	python/simple.py \
	python/simple_resolve.py \
	python/simple_server.py \
	python/spawn_group.py \
	python/tool.py \
	python/toolqry.py

AM_CPPFLAGS = -I$(top_builddir)/src -I$(top_builddir)/src/include -I$(top_builddir)/include -I$(top_builddir)/include/pmix

noinst_PROGRAMS = client client2 dmodex dynamic fault pub pubi \
                  tool debugger debuggerd alloc jctrl group group_dmodex asyncgroup \
                  hello nodeinfo  abi_no_init abi_with_init group_lcl_cid pset log \
                  group_bootstrap client3 client4 launcher spawn_group resolve multi_nspace_group \
                  pub2 toolqry simple_resolve pubstress monitor monitor_remote monitor_multi \
                  simple simple_server abort group_die connect_die group_leave \
                  group_destruct_die group_invite group_invite_timeout group_invite_decline \
                  group_invite_abort group_invite_others group_invite_suppress group_invite_nb topology \
                  spawn_reuse group_badinfo datatypes toolswitch

if !WANT_HIDDEN
# these examples use internal symbols
# use --disable-visibility
noinst_PROGRAMS += server
endif

client_SOURCES = client.c examples.h
client_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
client_LDADD =  $(top_builddir)/src/libpmix.la

client2_SOURCES = client2.c examples.h
client2_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
client2_LDADD =  $(top_builddir)/src/libpmix.la

client3_SOURCES = client3.c examples.h
client3_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
client3_LDADD =  $(top_builddir)/src/libpmix.la

client4_SOURCES = client4.c examples.h
client4_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
client4_LDADD =  $(top_builddir)/src/libpmix.la

debugger_SOURCES = debugger.c examples.h
debugger_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
debugger_LDADD =  $(top_builddir)/src/libpmix.la

debuggerd_SOURCES = debuggerd.c examples.h
debuggerd_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
debuggerd_LDADD =  $(top_builddir)/src/libpmix.la

alloc_SOURCES = alloc.c examples.h
alloc_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
alloc_LDADD =  $(top_builddir)/src/libpmix.la

jctrl_SOURCES = jctrl.c examples.h
jctrl_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
jctrl_LDADD =  $(top_builddir)/src/libpmix.la

dmodex_SOURCES = dmodex.c examples.h
dmodex_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
dmodex_LDADD =  $(top_builddir)/src/libpmix.la

dynamic_SOURCES = dynamic.c examples.h
dynamic_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
dynamic_LDADD = $(top_builddir)/src/libpmix.la

fault_SOURCES = fault.c examples.h
fault_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
fault_LDADD = $(top_builddir)/src/libpmix.la

pub_SOURCES = pub.c examples.h
pub_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
pub_LDADD = $(top_builddir)/src/libpmix.la

pubi_SOURCES = pubi.c examples.h
pubi_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
pubi_LDADD = $(top_builddir)/src/libpmix.la

tool_SOURCES = tool.c examples.h
tool_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
tool_LDADD = $(top_builddir)/src/libpmix.la

group_SOURCES = group.c examples.h
group_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
group_LDADD = $(top_builddir)/src/libpmix.la

group_die_SOURCES = group_die.c examples.h
group_die_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
group_die_LDADD = $(top_builddir)/src/libpmix.la

connect_die_SOURCES = connect_die.c examples.h
connect_die_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
connect_die_LDADD = $(top_builddir)/src/libpmix.la

group_leave_SOURCES = group_leave.c examples.h
group_leave_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
group_leave_LDADD = $(top_builddir)/src/libpmix.la

group_destruct_die_SOURCES = group_destruct_die.c examples.h
group_destruct_die_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
group_destruct_die_LDADD = $(top_builddir)/src/libpmix.la

group_invite_SOURCES = group_invite.c examples.h
group_invite_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
group_invite_LDADD = $(top_builddir)/src/libpmix.la

group_invite_others_SOURCES = group_invite_others.c examples.h
group_invite_others_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
group_invite_others_LDADD = $(top_builddir)/src/libpmix.la

group_badinfo_SOURCES = group_badinfo.c examples.h
group_badinfo_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
group_badinfo_LDADD = $(top_builddir)/src/libpmix.la

spawn_reuse_SOURCES = spawn_reuse.c examples.h
spawn_reuse_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
spawn_reuse_LDADD = $(top_builddir)/src/libpmix.la

group_invite_suppress_SOURCES = group_invite_suppress.c examples.h
group_invite_suppress_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
group_invite_suppress_LDADD = $(top_builddir)/src/libpmix.la

group_invite_nb_SOURCES = group_invite_nb.c examples.h
group_invite_nb_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
group_invite_nb_LDADD = $(top_builddir)/src/libpmix.la

group_invite_timeout_SOURCES = group_invite_timeout.c examples.h
group_invite_timeout_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
group_invite_timeout_LDADD = $(top_builddir)/src/libpmix.la

group_invite_decline_SOURCES = group_invite_decline.c examples.h
group_invite_decline_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
group_invite_decline_LDADD = $(top_builddir)/src/libpmix.la

group_invite_abort_SOURCES = group_invite_abort.c examples.h
group_invite_abort_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
group_invite_abort_LDADD = $(top_builddir)/src/libpmix.la

group_dmodex_SOURCES = group_dmodex.c examples.h
group_dmodex_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
group_dmodex_LDADD = $(top_builddir)/src/libpmix.la

asyncgroup_SOURCES = asyncgroup.c examples.h
asyncgroup_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
asyncgroup_LDADD = $(top_builddir)/src/libpmix.la

if !WANT_HIDDEN
server_SOURCES = server.c examples.h
server_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
server_LDADD = $(top_builddir)/src/libpmix.la
endif

hello_SOURCES = hello.c examples.h
hello_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
hello_LDADD =  $(top_builddir)/src/libpmix.la

abi_no_init_SOURCES = abi_no_init.c
abi_no_init_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
abi_no_init_LDADD =  $(top_builddir)/src/libpmix.la

abi_with_init_SOURCES = abi_with_init.c
abi_with_init_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
abi_with_init_LDADD =  $(top_builddir)/src/libpmix.la

group_lcl_cid_SOURCES = group_lcl_cid.c examples.h
group_lcl_cid_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
group_lcl_cid_LDADD =  $(top_builddir)/src/libpmix.la

nodeinfo_SOURCES = nodeinfo.c examples.h
nodeinfo_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
nodeinfo_LDADD =  $(top_builddir)/src/libpmix.la

topology_SOURCES = topology.c examples.h
topology_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
topology_LDADD =  $(top_builddir)/src/libpmix.la

pset_SOURCES = pset.c examples.h
pset_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
pset_LDADD = $(top_builddir)/src/libpmix.la

log_SOURCES = log.c examples.h
log_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
log_LDADD = $(top_builddir)/src/libpmix.la

group_bootstrap_SOURCES = group_bootstrap.c examples.h
group_bootstrap_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
group_bootstrap_LDADD = $(top_builddir)/src/libpmix.la

launcher_SOURCES = launcher.c examples.h
launcher_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
launcher_LDADD = $(top_builddir)/src/libpmix.la

spawn_group_SOURCES = spawn_group.c examples.h
spawn_group_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
spawn_group_LDADD = $(top_builddir)/src/libpmix.la

resolve_SOURCES = resolve.c examples.h
resolve_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
resolve_LDADD = $(top_builddir)/src/libpmix.la

multi_nspace_group_SOURCES = multi_nspace_group.c examples.h
multi_nspace_group_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
multi_nspace_group_LDADD = $(top_builddir)/src/libpmix.la

pub2_SOURCES = pub2.c examples.h
pub2_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
pub2_LDADD = $(top_builddir)/src/libpmix.la

toolqry_SOURCES = toolqry.c examples.h
toolqry_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
toolqry_LDADD = $(top_builddir)/src/libpmix.la

toolswitch_SOURCES = toolswitch.c examples.h
toolswitch_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
toolswitch_LDADD = $(top_builddir)/src/libpmix.la

simple_resolve_SOURCES = simple_resolve.c examples.h
simple_resolve_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
simple_resolve_LDADD = $(top_builddir)/src/libpmix.la

pubstress_SOURCES = pubstress.c examples.h
pubstress_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
pubstress_LDADD = $(top_builddir)/src/libpmix.la

monitor_SOURCES = monitor.c examples.h
monitor_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
monitor_LDADD = $(top_builddir)/src/libpmix.la

monitor_remote_SOURCES = monitor_remote.c examples.h
monitor_remote_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
monitor_remote_LDADD = $(top_builddir)/src/libpmix.la

monitor_multi_SOURCES = monitor_multi.c examples.h
monitor_multi_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
monitor_multi_LDADD = $(top_builddir)/src/libpmix.la

simple_SOURCES = simple.c examples.h
simple_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
simple_LDADD = $(top_builddir)/src/libpmix.la

simple_server_SOURCES = simple_server.c examples.h
simple_server_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
simple_server_LDADD = $(top_builddir)/src/libpmix.la

datatypes_SOURCES = datatypes.c examples.h
datatypes_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
datatypes_LDADD = $(top_builddir)/src/libpmix.la

abort_SOURCES = abort.c examples.h
abort_LDFLAGS = $(PMIX_PKG_CONFIG_LDFLAGS)
abort_LDADD = $(top_builddir)/src/libpmix.la

distclean-local:
	rm -f *.o alloc asyncgroup bad_exit client client2 \
        debugger debuggerd dmodex dynamic fault group \
        hello jctrl launcher log pub pubi server tool \
        abi_no_init abi_with_init group_lcl_cid pset \
        async_group group_dmodex group_bootstrap client3 \
        group_leave group_destruct_die group_invite group_invite_timeout group_invite_decline \
        group_invite_abort group_invite_suppress group_invite_nb \
        spawn_group resolve multi_nspace_group pub2 toolqry \
        simple_resolve pubstress monitor monitor_remote \
        simple simple_server abort datatypes toolswitch
