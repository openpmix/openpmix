/*
 * Copyright (c) 2010      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2010      Oracle and/or its affiliates.  All rights reserved.
 * Copyright (c) 2018-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"
#include "pmix_common.h"

#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <errno.h>
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif
#ifdef HAVE_SYS_SOCKIO_H
#    include <sys/sockio.h>
#endif
#ifdef HAVE_SYS_IOCTL_H
#    include <sys/ioctl.h>
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
#ifdef HAVE_NETDB_H
#    include <netdb.h>
#endif
#ifdef HAVE_IFADDRS_H
#    include <ifaddrs.h>
#endif

#include "src/mca/pif/base/base.h"
#include "src/mca/pif/pif.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_if.h"

static int if_bsdx_ipv6_open(void);

/* Discovers IPv6 interfaces for:
 *
 * NetBSD
 * OpenBSD
 * FreeBSD
 * 386BSD
 * bsdi
 * Apple
 */
pmix_pif_base_component_t pmix_mca_pif_bsdx_ipv6_component = {
    PMIX_MCA_BASE_VERSION(pif),

    /* Component name and version */
    .pmix_mca_component_name = "bsdx_ipv6",
    PMIX_MCA_BASE_MAKE_VERSION(component,
                               PMIX_MAJOR_VERSION,
                               PMIX_MINOR_VERSION,
                               PMIX_RELEASE_VERSION),

    /* Component open and close functions */
    .pmix_mca_open_component = if_bsdx_ipv6_open
};
PMIX_MCA_BASE_COMPONENT_INIT(pmix, pif, bsdx_ipv6)

/* convert an IPv6 netmask to a prefix length by counting the leading
 * one bits.  An address is stored in network byte order, so walking it a
 * byte at a time from the front is endian-independent. */
static uint32_t prefix6(const struct sockaddr_in6 *netmask)
{
    const uint8_t *m = (const uint8_t *) &netmask->sin6_addr;
    uint32_t plen = 0;
    int i, bit;

    for (i = 0; i < 16; i++) {
        if (0xFF == m[i]) {
            plen += 8;
            continue;
        }
        for (bit = 7; bit >= 0; --bit) {
            if (0 == (m[i] & (1 << bit))) {
                return plen;
            }
            plen += 1;
        }
        return plen;
    }

    return plen;
}

/* configure using getifaddrs(3) */
static int if_bsdx_ipv6_open(void)
{
    struct ifaddrs *ifadd_list = NULL;
    struct ifaddrs *cur_ifaddrs;
    struct sockaddr_in6 *sin_addr;

    pmix_output_verbose(1, pmix_pif_base_framework.framework_output,
                        "searching for IPv6 interfaces");

    /* getifaddrs(3) allocates the list itself and hands it back through
     * the pointer we give it; freeifaddrs(3) releases it again. */
    if (getifaddrs(&ifadd_list) < 0) {
        pmix_output(0, "pmix_ifinit: getifaddrs() failed with error=%d\n", errno);
        return PMIX_ERROR;
    }

    for (cur_ifaddrs = ifadd_list; NULL != cur_ifaddrs; cur_ifaddrs = cur_ifaddrs->ifa_next) {
        pmix_pif_t *intf;
        struct in6_addr a6;

        /* getifaddrs(3) leaves ifa_addr NULL for an interface that carries
         * no address at all, so it cannot be dereferenced unchecked */
        if (NULL == cur_ifaddrs->ifa_addr) {
            pmix_output_verbose(1, pmix_pif_base_framework.framework_output,
                                "skipping address-less interface %s.\n", cur_ifaddrs->ifa_name);
            continue;
        }

        /* skip non-ipv6 interface addresses */
        if (AF_INET6 != cur_ifaddrs->ifa_addr->sa_family) {
            pmix_output_verbose(1, pmix_pif_base_framework.framework_output,
                                "skipping non-ipv6 interface %s[%d].\n", cur_ifaddrs->ifa_name,
                                (int) cur_ifaddrs->ifa_addr->sa_family);
            continue;
        }

        /* skip interface if it is down (IFF_UP not set) */
        if (0 == (cur_ifaddrs->ifa_flags & IFF_UP)) {
            pmix_output_verbose(1, pmix_pif_base_framework.framework_output,
                                "skipping non-up interface %s.\n", cur_ifaddrs->ifa_name);
            continue;
        }

        /* skip if it is a point-to-point interface */
        /* TODO: do we really skip p2p? */
        if (0 != (cur_ifaddrs->ifa_flags & IFF_POINTOPOINT)) {
            pmix_output_verbose(1, pmix_pif_base_framework.framework_output,
                                "skipping p2p interface %s.\n", cur_ifaddrs->ifa_name);
            continue;
        }

        sin_addr = (struct sockaddr_in6 *) cur_ifaddrs->ifa_addr;

        /*
         * skip IPv6 address starting with fe80:, as this is supposed to be
         * link-local scope. sockaddr_in6->sin6_scope_id doesn't always work
         * TODO: test whether scope id is set to a sensible value on
         * linux and/or bsd (including osx)
         *
         * MacOSX: fe80::... has a scope of 0, but ifconfig -a shows
         * a scope of 4 on that particular machine,
         * so the scope returned by getifaddrs() isn't working properly
         */

        if ((IN6_IS_ADDR_LINKLOCAL(&sin_addr->sin6_addr))) {
            pmix_output_verbose(1, pmix_pif_base_framework.framework_output,
                                "skipping link-local ipv6 address on interface "
                                "%s with scope %d.\n",
                                cur_ifaddrs->ifa_name, sin_addr->sin6_scope_id);
            continue;
        }

        if (0 < pmix_output_get_verbosity(pmix_pif_base_framework.framework_output)) {
            char addr_name[INET6_ADDRSTRLEN];

            if (NULL != inet_ntop(AF_INET6, &sin_addr->sin6_addr, addr_name, sizeof(addr_name))) {
                pmix_output(0, "ipv6 capable interface %s discovered, address %s.\n",
                            cur_ifaddrs->ifa_name, addr_name);
            }
        }

        /* fill values into the pmix_pif_t */
        memcpy(&a6, &(sin_addr->sin6_addr), sizeof(struct in6_addr));

        intf = PMIX_NEW(pmix_pif_t);
        if (NULL == intf) {
            pmix_output(0, "pmix_ifinit: unable to allocate %lu bytes\n",
                        (unsigned long) sizeof(pmix_pif_t));
            freeifaddrs(ifadd_list);
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
        intf->af_family = AF_INET6;
        pmix_strncpy(intf->if_name, cur_ifaddrs->ifa_name, PMIX_IF_NAMESIZE - 1);
        intf->if_index = pmix_list_get_size(&pmix_if_list) + 1;
        ((struct sockaddr_in6 *) &intf->if_addr)->sin6_addr = a6;
        ((struct sockaddr_in6 *) &intf->if_addr)->sin6_family = AF_INET6;

        /* since every scope != 0 is ignored, we just set the scope to 0 */
        ((struct sockaddr_in6 *) &intf->if_addr)->sin6_scope_id = 0;

        /* Store the real prefix length rather than assuming the /64
         * that SLAAC happens to hand out: getifaddrs(3) reports the
         * netmask, ::1 is a /128, and linux_ipv6 has always recorded
         * what the kernel actually said.  ifa_netmask is NULL when the
         * address carries no mask, which we treat as a host route - the
         * safe direction, since it makes pmix_net_samenetwork() demand
         * an exact match rather than claim a whole /64 we cannot see. */
        if (NULL == cur_ifaddrs->ifa_netmask) {
            intf->if_mask = 128;
        } else {
            intf->if_mask = prefix6(
                (const struct sockaddr_in6 *) cur_ifaddrs->ifa_netmask);
        }
        intf->if_flags = cur_ifaddrs->ifa_flags;

        /*
         * FIXME: figure out how to gain access to the kernel index
         * (or create our own), getifaddrs() does not contain such
         * data
         */
        intf->if_kernel_index = (uint16_t) if_nametoindex(cur_ifaddrs->ifa_name);
        pmix_list_append(&pmix_if_list, &(intf->super));
    } /*  of for loop over ifaddrs list */

    freeifaddrs(ifadd_list);

    return PMIX_SUCCESS;
}
