# -*- shell-script -*-
#
# Copyright (C) 2015-2017 Mellanox Technologies, Inc.
#                         All rights reserved.
# Copyright (c) 2015      Research Organization for Information Science
#                         and Technology (RIST). All rights reserved.
# Copyright (c) 2016      Los Alamos National Security, LLC. All rights
#                         reserved.
# Copyright (c) 2016 Cisco Systems, Inc.  All rights reserved.
# Copyright (c) 2021      Nanook Consulting.  All rights reserved.
# Copyright (c) 2022      Amazon.com, Inc. or its affiliates.
#                         All Rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#

# MCA_pmix_pgpu_nvd_CONFIG(prefix, [action-if-found], [action-if-not-found])
# --------------------------------------------------------
AC_DEFUN([MCA_pmix_pgpu_nvd_CONFIG],[
    AC_CONFIG_FILES([src/mca/pgpu/nvd/Makefile])

    # This component builds everywhere. It links nothing and needs no
    # vendor SDK: all it does is read info attributes hwloc already
    # recorded and set environment variables, so there is nothing here
    # for configure to look for. Whether it does any work is settled at
    # RUN time by its own component_open, which asks the local topology
    # whether this vendor's hardware is present and declines if it is
    # not - and that is the only place the question can be answered,
    # since the machine that builds PMIx is routinely not the machine
    # that runs it. Gating it at build time instead meant a cluster with
    # NVIDIA GPUs got no support unless somebody had thought to
    # configure --enable-test-build.
    $1
    pmix_pgpu_nvd_happy=yes

    PMIX_SUMMARY_ADD([GPUs], [NVIDIA], [], [$pmix_pgpu_nvd_happy])
])
