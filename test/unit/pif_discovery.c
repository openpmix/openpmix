/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for pif interface discovery (the src/mca/pif components).
 *
 * Every pif component's whole job is its open function: probe the OS and
 * append one pmix_pif_t per discovered address to the shared
 * pmix_if_list. Which component does that is a configure-time decision -
 * getifaddrs(3) on BSD/macOS, SIOCGIFCONF plus /proc/net/if_inet6 on
 * Linux - so this program deliberately asserts only on the *result*, and
 * runs unchanged on either platform.
 *
 * The check that matters is the netmask one. if_mask is documented to
 * hold a CIDR prefix length, and bsdx_ipv4 derived it from the
 * interface's own address rather than from ifa_netmask, so a /24
 * interface reported a prefix of 32 (or 31, or 25 - whatever the trailing
 * zero bits of the address happened to be). Nothing crashes on that: it
 * silently turns pmix_ifaddrtokindex() into an exact-address match, so a
 * peer on the same subnet stops resolving to the local NIC, and
 * pmix_ifindextomask() hands external consumers a wrong prefix. Only a
 * cross-check against an independent view of the same interface catches
 * it, which is why the expected value here comes from getifaddrs(3)
 * rather than from anything under src/mca/pif.
 *
 * Discovery is also the very first thing that touches the OS in
 * pmix_rte_init(), for every PMIx process, so a NULL dereference in it -
 * getifaddrs(3) documents that ifa_addr may be NULL - is a segfault
 * before the library is up. There is no assertion for that: reaching the
 * report at the bottom at all is the test.
 *
 * This needs the MCA up but no server: pmix_init_util() establishes the
 * install dirs, the variable system and the component repository, which
 * is all that opening a framework requires.
 */

#include "src/include/pmix_config.h"

#include "pmix_common.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/base/pmix_mca_base_framework.h"
#include "src/mca/pif/base/base.h"
#include "src/runtime/pmix_init_util.h"
#include "src/util/pmix_if.h"
#include "src/util/pmix_printf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif
#ifdef HAVE_NETINET_IN_H
#    include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#    include <arpa/inet.h>
#endif
#ifdef HAVE_NET_IF_H
#    include <net/if.h>
#endif
#ifdef HAVE_IFADDRS_H
#    include <ifaddrs.h>
#endif

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        ++nfail;
    }
}

/* netmask (network byte order) -> CIDR prefix length.  Deliberately a
 * different algorithm from the components' prefix() helper, so that a
 * shared mistake in that helper cannot make the comparison below agree
 * with itself. */
static int cidr_of(uint32_t netmask)
{
    uint32_t mask = ntohl(netmask);
    int plen = 0;
    int bit;

    for (bit = 31; bit >= 0; --bit) {
        if (0 == (mask & (((uint32_t) 1) << bit))) {
            break;
        }
        ++plen;
    }
    return plen;
}

/* Every entry has to be internally consistent: an address family that
 * agrees with the sockaddr it is carrying, a prefix length that could
 * belong to that family, a name, and a kernel index that was actually
 * assigned rather than left at the constructor's (uint16_t)-1. */
static void entries_are_well_formed(void)
{
    pmix_pif_t *intf;
    int bad_family = 0, bad_mask = 0, bad_name = 0, bad_kindex = 0;
    int nv4 = 0, nv6 = 0;

    PMIX_LIST_FOREACH (intf, &pmix_if_list, pmix_pif_t) {
        struct sockaddr *sa = (struct sockaddr *) &intf->if_addr;

        if (intf->af_family != sa->sa_family) {
            ++bad_family;
        }
        if (AF_INET == intf->af_family) {
            ++nv4;
            if (intf->if_mask > 32) {
                ++bad_mask;
            }
        } else if (AF_INET6 == intf->af_family) {
            ++nv6;
            if (intf->if_mask > 128) {
                ++bad_mask;
            }
        } else {
            ++bad_family;
        }
        if ('\0' == intf->if_name[0] || '\0' != intf->if_name[PMIX_IF_NAMESIZE]) {
            ++bad_name;
        }
        if (((uint16_t) -1) == intf->if_kernel_index) {
            ++bad_kindex;
        }
    }

    fprintf(stdout, "  (discovered %d IPv4 and %d IPv6 interface addresses)\n", nv4, nv6);
    report("af_family agrees with the stored sockaddr", 0 == bad_family);
    report("if_mask is a prefix length its family can hold", 0 == bad_mask);
    report("if_name is set and NUL-terminated", 0 == bad_name);
    report("if_kernel_index was assigned by the OS", 0 == bad_kindex);
}

/* if_index is the entry's 1-based slot in pmix_if_list, and the
 * ifbegin/ifnext walk is how the ptl listener iterates.  The two have to
 * describe the same list: every entry reachable, in order, exactly once.
 */
static void indices_are_a_dense_walk(void)
{
    pmix_pif_t *intf;
    int expect = 1;
    int ooo = 0;
    int seen = 0;
    int i;

    PMIX_LIST_FOREACH (intf, &pmix_if_list, pmix_pif_t) {
        if (intf->if_index != expect) {
            ++ooo;
        }
        ++expect;
    }
    report("if_index numbers the list 1..N in append order", 0 == ooo);

    for (i = pmix_ifbegin(); i >= 0; i = pmix_ifnext(i)) {
        ++seen;
    }
    report("ifbegin/ifnext reach every entry exactly once",
           seen == (int) pmix_list_get_size(&pmix_if_list));
    report("ifcount agrees with the list", pmix_ifcount() == (int) pmix_list_get_size(&pmix_if_list));
}

#ifdef HAVE_IFADDRS_H
/* The regression test: cross-check each discovered IPv4 prefix against
 * the netmask the OS reports for that same name+address through a path
 * no pif component uses on either platform. */
static void masks_match_the_os(void)
{
    struct ifaddrs *list = NULL, *cur;
    pmix_pif_t *intf;
    int checked = 0, wrong = 0;
    char *label;

    if (getifaddrs(&list) < 0) {
        report("getifaddrs is available to cross-check against", 0);
        return;
    }

    PMIX_LIST_FOREACH (intf, &pmix_if_list, pmix_pif_t) {
        struct sockaddr_in *mine;

        if (AF_INET != intf->af_family) {
            continue;
        }
        mine = (struct sockaddr_in *) &intf->if_addr;

        for (cur = list; NULL != cur; cur = cur->ifa_next) {
            struct sockaddr_in *theirs;
            int expect;

            if (NULL == cur->ifa_addr || AF_INET != cur->ifa_addr->sa_family
                || NULL == cur->ifa_netmask) {
                continue;
            }
            if (0 != strcmp(cur->ifa_name, intf->if_name)) {
                continue;
            }
            theirs = (struct sockaddr_in *) cur->ifa_addr;
            if (0 != memcmp(&theirs->sin_addr, &mine->sin_addr, sizeof(struct in_addr))) {
                continue;
            }

            ++checked;
            expect = cidr_of(((struct sockaddr_in *) cur->ifa_netmask)->sin_addr.s_addr);
            if ((int) intf->if_mask != expect) {
                ++wrong;
                fprintf(stdout, "    %s: pif says /%u, the OS says /%d\n", intf->if_name,
                        (unsigned) intf->if_mask, expect);
            }
            break;
        }
    }
    freeifaddrs(list);

    pmix_asprintf(&label, "if_mask is the OS netmask, not the address (%d checked)", checked);
    report(label, 0 == wrong);
    free(label);
}
#endif /* HAVE_IFADDRS_H */

int main(int argc, char **argv)
{
    int rc;
    size_t first_count;

    (void) argc;
    (void) argv;

    /* keep the caller's own MCA parameter files out of the results, as
     * the mca/ suite does */
    setenv("PMIX_MCA_mca_base_param_files", "none", 1);

    /* pmix_init_util() is what opens pif - it is the last thing the
     * util-init phase does, right after pmix_net_init(). Opening it again
     * here would only take a second reference and the close below would
     * hand that reference back without ever reaching discovery's
     * teardown, so the framework is deliberately left exactly as
     * init_util left it. */
    rc = pmix_init_util(NULL, 0, NULL);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "pmix_init_util failed: %d\n", rc);
        return 1;
    }
    report("util-init opened the framework",
           pmix_mca_base_framework_is_open(&pmix_pif_base_framework));

    first_count = pmix_list_get_size(&pmix_if_list);
    entries_are_well_formed();
    indices_are_a_dense_walk();
#ifdef HAVE_IFADDRS_H
    masks_match_the_os();
#endif

    rc = pmix_mca_base_framework_close(&pmix_pif_base_framework);
    report("the framework closes",
           PMIX_SUCCESS == rc && 0 == pmix_pif_base_framework.framework_refcnt);
    report("close drained the interface list", 0 == pmix_list_get_size(&pmix_if_list));

    /* A second cycle is not redundant: close() destructs the list that
     * open() constructed, and the frameopen latch that makes a re-open a
     * no-op has to have been let go, or discovery silently yields nothing
     * the second time around. */
    rc = pmix_mca_base_framework_open(&pmix_pif_base_framework, PMIX_MCA_BASE_OPEN_DEFAULT);
    report("the framework re-opens", PMIX_SUCCESS == rc);
    report("re-opening rediscovers the same interfaces",
           first_count == pmix_list_get_size(&pmix_if_list));
    entries_are_well_formed();
    indices_are_a_dense_walk();

    rc = pmix_mca_base_framework_close(&pmix_pif_base_framework);
    report("the framework closes again",
           PMIX_SUCCESS == rc && 0 == pmix_pif_base_framework.framework_refcnt
               && 0 == pmix_list_get_size(&pmix_if_list));

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    return (nfail > 0) ? 1 : 0;
}
