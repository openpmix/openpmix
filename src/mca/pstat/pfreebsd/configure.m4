# -*- shell-script -*-
#
# Copyright (c) 2026      Nanook Consulting  All rights reserved.
# $COPYRIGHT$
#
# Additional copyrights may follow
#
# $HEADER$
#
# MCA_pmix_pstat_pfreebsd_CONFIG(action-if-can-compile,
#                                [action-if-cant-compile])
# ------------------------------------------------
AC_DEFUN([MCA_pmix_pstat_pfreebsd_CONFIG],[
    AC_CONFIG_FILES([src/mca/pstat/pfreebsd/Makefile])

    AC_REQUIRE([OAC_CHECK_OS_FLAVORS])

    # The FreeBSD reader is built only on FreeBSD, and only where the
    # kernel's process, devstat and swap records have the shape it reads:
    # FreeBSD 12 and later. Everything comes from sysctl(3) and
    # getifaddrs(3), so it needs no library of its own.
    pstat_pfreebsd_happy="no"
    AS_IF([test "$oac_found_freebsd" = "yes"],
          [AC_MSG_CHECKING([for a FreeBSD with the records pstat/pfreebsd reads])
           AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#include <sys/param.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/devicestat.h>
#include <vm/vm_param.h>
#if __FreeBSD_version < 1200000
#error too old
#endif
]], [[struct kinfo_proc kp; struct devstat ds; struct xswdev xs;
       return (int) (sizeof(kp.ki_pctcpu) + sizeof(ds.busy_time) + sizeof(xs.xsw_used));]])],
               [pstat_pfreebsd_happy="yes"])
           AC_MSG_RESULT([$pstat_pfreebsd_happy])])

    AS_IF([test "$pstat_pfreebsd_happy" = "yes"],
          [$1],
          [$2])
])
