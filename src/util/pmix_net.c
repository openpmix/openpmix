/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2007      Los Alamos National Security, LLC.  All rights
 *                         reserved.
 * Copyright (c) 2009-2015 Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2015      Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"
#include "pmix_common.h"

#include <stdio.h>
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

#include "src/include/pmix_globals.h"
#include "src/runtime/pmix_rte.h"
#include "src/threads/pmix_tsd.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_net.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_show_help.h"

/* What every routine here answers when it cannot name an address.  It is
 * an array rather than a pointer at a string literal because callers are
 * handed it as a plain char*, and one that writes through it - stripping
 * a suffix, say, the way this file does to a real answer - would
 * otherwise be modifying a string literal. */
static char pmix_hostname_unknown[] = "UNKNOWN";

/* this function doesn't depend on sockaddr_h */
bool pmix_net_isaddr(const char *name)
{
    struct addrinfo hint, *res = NULL;

    /* initialize the hint */
    memset(&hint, '\0', sizeof hint);

    /* indicate that we don't know the family */
    hint.ai_family = PF_UNSPEC;
    hint.ai_flags = AI_NUMERICHOST;

    if (0 != getaddrinfo(name, NULL, &hint, &res)) {
        /* the input wasn't a recognizable address */
        return false;
    }
    /* we don't care what family - all we care is that
     * it is indeed an address
     */
    freeaddrinfo(res);
    return true;
}

#ifdef HAVE_STRUCT_SOCKADDR_IN

typedef struct private_ipv4_t {
    in_addr_t addr;
    uint32_t netmask_bits;
} private_ipv4_t;

static private_ipv4_t *private_ipv4 = NULL;

static pmix_tsd_key_t hostname_tsd_key;

static void hostname_cleanup(void *value)
{
    if (NULL != value)
        free(value);
}

static char *get_hostname_buffer(void)
{
    void *buffer;
    int ret;

    ret = pmix_tsd_getspecific(hostname_tsd_key, &buffer);
    if (PMIX_SUCCESS != ret) {
        return NULL;
    }

    if (NULL == buffer) {
        buffer = calloc((NI_MAXHOST + 1), sizeof(char));
        if (NULL == buffer) {
            return NULL;
        }
        /* the buffer is reachable only through the key - if we cannot
         * bind it there, nothing will ever free it, and every later call
         * would allocate another one. Hand back the failure instead */
        ret = pmix_tsd_setspecific(hostname_tsd_key, buffer);
        if (PMIX_SUCCESS != ret) {
            free(buffer);
            return NULL;
        }
    }

    return (char *) buffer;
}

pmix_status_t pmix_net_setup_private_ipv4(void)
{
    char **args, *arg;
    uint32_t a, b, c, d, bits, addr;
    int i, j, count, found_bad = 0;

    /* the value arrives from the MCA parameter system, so this cannot be
     * folded back into pmix_net_init() - that runs from pmix_init_util(),
     * well before pmix_register_params() gives the variable its value */
    free(private_ipv4);
    private_ipv4 = NULL;

    args = PMIx_Argv_split(pmix_net_private_ipv4, ';');
    if (NULL == args) {
        return PMIX_SUCCESS;
    }
    count = PMIx_Argv_count(args);
    private_ipv4 = (private_ipv4_t *) calloc((count + 1), sizeof(private_ipv4_t));
    if (NULL == private_ipv4) {
        PMIx_Argv_free(args);
        return PMIX_ERR_NOMEM;
    }
    /* j tracks the next slot to fill so that malformed entries are
     * skipped rather than left as zeroed holes - a zero addr is the
     * array terminator, so a hole would truncate the list. */
    for (i = 0, j = 0; i < count; i++) {
        arg = args[i];

        if (5 != sscanf(arg, "%u.%u.%u.%u/%u", &a, &b, &c, &d, &bits)
            || (a > 255) || (b > 255) || (c > 255) || (d > 255) || (bits > 32)) {
            if (0 == found_bad) {
                pmix_show_help("help-pmix-util.txt", "malformed net_private_ipv4", true,
                               args[i]);
                found_bad = 1;
            }
            continue;
        }
        addr = (a << 24) | (b << 16) | (c << 8) | d;
        private_ipv4[j].addr = htonl(addr);
        private_ipv4[j].netmask_bits = bits;
        j++;
    }
    private_ipv4[j].addr = 0;
    private_ipv4[j].netmask_bits = 0;
    PMIx_Argv_free(args);

    return PMIX_SUCCESS;
}

int pmix_net_init(void)
{
    return pmix_tsd_key_create(&hostname_tsd_key, hostname_cleanup);
}

int pmix_net_finalize(void)
{
    free(private_ipv4);
    private_ipv4 = NULL;

    return PMIX_SUCCESS;
}

/* convert a CIDR prefixlen to netmask (in network byte order) */
uint32_t pmix_net_prefix2netmask(uint32_t prefixlen)
{
    /* A prefix of 0 matches everything; anything >= 32 is an exact
     * match. Handle both ends explicitly: shifting a 32-bit value by
     * 32 (or by 0 from the other direction) is undefined behavior, and
     * shifting a signed 1 left by 31 overflows. Compute the mask from
     * an unsigned all-ones value for the in-between cases. */
    if (0 == prefixlen) {
        return 0;
    }
    if (32 <= prefixlen) {
        return htonl(0xFFFFFFFFU);
    }
    return htonl(0xFFFFFFFFU << (32 - prefixlen));
}

bool pmix_net_islocalhost(const struct sockaddr *addr)
{
    switch (addr->sa_family) {
    case AF_INET: {
        const struct sockaddr_in *inaddr = (struct sockaddr_in *) addr;
        /* if it's in the 127.0.0.0/8 domain, it shouldn't be routed
           (0x7f == 127); mask the full top octet so that other
           addresses such as 255.x are not misclassified */
        if (0x7F000000 == (0xFF000000 & ntohl(inaddr->sin_addr.s_addr))) {
            return true;
        }
        return false;
    }

    case AF_INET6: {
        const struct sockaddr_in6 *inaddr = (struct sockaddr_in6 *) addr;
        if (IN6_IS_ADDR_LOOPBACK(&inaddr->sin6_addr)) {
            return true;
        }
        /* an IPv4-mapped address carries an ordinary IPv4 address in its
         * low 32 bits, so ::ffff:127.0.0.1 names this host just as surely
         * as 127.0.0.1 does - answer for the whole 127.0.0.0/8 range the
         * AF_INET arm above covers */
        if (IN6_IS_ADDR_V4MAPPED(&inaddr->sin6_addr)) {
            uint32_t v4;
            memcpy(&v4, ((const uint8_t *) &inaddr->sin6_addr) + 12, sizeof(v4));
            if (0x7F000000 == (0xFF000000 & ntohl(v4))) {
                return true;
            }
        }
        return false;
    }

    default:
        pmix_output(0, "unhandled sa_family %d passed to pmix_net_islocalhost", addr->sa_family);
        return false;
    }
}

bool pmix_net_samenetwork(const struct sockaddr_storage *addr1,
                          const struct sockaddr_storage *addr2,
                          uint32_t plen)
{
    uint32_t prefixlen;
    struct sockaddr a1, a2;

    memcpy(&a1, addr1, sizeof(a1));
    memcpy(&a2, addr2, sizeof(a2));

    if (a1.sa_family != a2.sa_family) {
        return false; // address families must be equal
    }

    switch (a1.sa_family) {
    case AF_INET: {
        if (0 == plen) {
            prefixlen = 32;
        } else {
            prefixlen = plen;
        }
        struct sockaddr_in inaddr1, inaddr2;
        /* Use temporary variables and memcpy's so that we don't
           run into bus errors on Solaris/SPARC */
        memcpy(&inaddr1, addr1, sizeof(inaddr1));
        memcpy(&inaddr2, addr2, sizeof(inaddr2));
        uint32_t netmask = pmix_net_prefix2netmask(prefixlen);

        if ((inaddr1.sin_addr.s_addr & netmask) == (inaddr2.sin_addr.s_addr & netmask)) {
            return true;
        }
        return false;
    }

    case AF_INET6: {
        struct sockaddr_in6 inaddr1, inaddr2;
        const uint8_t *a6_1, *a6_2;
        uint32_t nbytes, nbits;

        /* Use temporary variables and memcpy's so that we don't
           run into bus errors on Solaris/SPARC */
        memcpy(&inaddr1, addr1, sizeof(inaddr1));
        memcpy(&inaddr2, addr2, sizeof(inaddr2));
        a6_1 = (const uint8_t *) &inaddr1.sin6_addr;
        a6_2 = (const uint8_t *) &inaddr2.sin6_addr;

        /* A caller that does not know the prefix gets the /64 that
         * SLAAC gives essentially every autoconfigured interface. */
        if (0 == plen) {
            prefixlen = 64;
        } else if (128 < plen) {
            prefixlen = 128;
        } else {
            prefixlen = plen;
        }

        /* Compare the whole bytes the prefix covers, then whatever bits
         * are left over in the byte it ends inside of.  Doing this a
         * byte at a time out of the in6_addr is endian-independent:
         * an IPv6 address is stored in network byte order, so byte 0 is
         * always the most significant one.
         *
         * This used to answer only /64 and return false for every other
         * prefix, which is why the two pif IPv6 discovery components
         * could not agree on what to report: linux_ipv6 stores the true
         * prefix from /proc, so a loopback entry carrying /128 never
         * matched anything - including itself. */
        nbytes = prefixlen / 8;
        nbits = prefixlen % 8;

        if (0 != nbytes && 0 != memcmp(a6_1, a6_2, nbytes)) {
            return false;
        }
        if (0 != nbits) {
            /* nbits is 1..7 here, so neither shift is undefined, and
             * nbytes is at most 15 - a prefix that ends mid-byte cannot
             * be 128 */
            uint8_t mask = (uint8_t) (0xFFU << (8 - nbits));
            if ((a6_1[nbytes] & mask) != (a6_2[nbytes] & mask)) {
                return false;
            }
        }
        return true;
    }

    default:
        pmix_output(0, "unhandled sa_family %d passed to pmix_samenetwork", a1.sa_family);
    }

    return false;
}

bool pmix_net_addr_isipv6linklocal(const struct sockaddr *addr)
{
    /* PMIX_ENABLE_IPV6 does not gate this. It is off by default, but the
     * pif IPv6 discovery components are gated on the OS rather than on
     * that flag, so an AF_INET6 address is entirely ordinary in a default
     * build - and every one of them used to fall through to the default
     * arm below, which answers "unhandled sa_family" on stream 0 rather
     * than answering the question. Every other family test in this file
     * (islocalhost, samenetwork) is likewise unconditional. */
    switch (addr->sa_family) {
    case AF_INET6: {
        struct sockaddr_in6 if_addr;

        memset(&if_addr, 0, sizeof(if_addr));
        if_addr.sin6_family = AF_INET6;
        if (1 != inet_pton(AF_INET6, "fe80::0000", &if_addr.sin6_addr)) {
            return false;
        }
        return pmix_net_samenetwork((const struct sockaddr_storage *) addr,
                                    (const struct sockaddr_storage *) &if_addr, 64);
    }
    case AF_INET:
        return false;
    default:
        pmix_output(0, "unhandled sa_family %d passed to pmix_net_addr_isipv6linklocal\n",
                    addr->sa_family);
    }

    return false;
}

/**
 * Returns true if the given address is a public IPv4 address.
 */
bool pmix_net_addr_isipv4public(const struct sockaddr *addr)
{
    switch (addr->sa_family) {
    case AF_INET6:
        return false;

    case AF_INET: {
        const struct sockaddr_in *inaddr = (struct sockaddr_in *) addr;
        int i;

        if (NULL == private_ipv4) {
            return true;
        }

        for (i = 0; private_ipv4[i].addr != 0; i++) {
            if (private_ipv4[i].addr
                == (inaddr->sin_addr.s_addr
                    & pmix_net_prefix2netmask(private_ipv4[i].netmask_bits)))
                return false;
        }
    }
        return true;
    default:
        pmix_output(0, "unhandled sa_family %d passed to pmix_net_addr_isipv4public\n",
                    addr->sa_family);
    }

    return false;
}

char *pmix_net_get_hostname(const struct sockaddr *addr)
{
    char *name = get_hostname_buffer();
    int error;
    socklen_t addrlen;
    char *p;

    if (NULL == name) {
        pmix_output(0, "pmix_net_get_hostname: malloc() failed\n");
        return pmix_hostname_unknown;
    }
    memset(name, 0, NI_MAXHOST + 1);

    switch (addr->sa_family) {
    case AF_INET:
        addrlen = sizeof(struct sockaddr_in);
        break;
    case AF_INET6:
#    if defined(__NetBSD__)
        /* hotfix for netbsd: on my netbsd machine, getnameinfo
           returns an unknown error code. */
        if (NULL == inet_ntop(AF_INET6, &((struct sockaddr_in6 *) addr)->sin6_addr, name, NI_MAXHOST)) {
            pmix_output(0, "pmix_sockaddr2str failed with error code %d", errno);
            return pmix_hostname_unknown;
        }
        return name;
#    else
        addrlen = sizeof(struct sockaddr_in6);
#    endif
        break;
    default:
        return pmix_hostname_unknown;
    }

    error = getnameinfo(addr, addrlen, name, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);

    if (error) {
        /* getnameinfo() returns an EAI_* code directly; decode that,
         * not errno (which it does not generally set) */
        pmix_output(0, "pmix_sockaddr2str failed:%s (return code %i)\n", gai_strerror(error), error);
        return pmix_hostname_unknown;
    }
    /* strip any trailing % data as it isn't pertinent */
    if (NULL != (p = strrchr(name, '%'))) {
        *p = '\0';
    }
    return name;
}

int pmix_net_get_port(const struct sockaddr *addr)
{
    switch (addr->sa_family) {
    case AF_INET:
        return ntohs(((struct sockaddr_in *) addr)->sin_port);

    case AF_INET6:
        return ntohs(((struct sockaddr_in6 *) addr)->sin6_port);
    }

    return -1;
}

#else /* HAVE_STRUCT_SOCKADDR_IN */

/* No configuration anyone builds selects this arm, so it gets no compiler
 * coverage: every stub has to name its parameters as deliberately unread,
 * since the tree builds -Wextra -Werror. Force the guard false for one
 * "make pmix_net.lo" before believing an edit here is good. */

pmix_status_t pmix_net_setup_private_ipv4(void)
{
    return PMIX_SUCCESS;
}

int pmix_net_init(void)
{
    return PMIX_SUCCESS;
}

int pmix_net_finalize(void)
{
    return PMIX_SUCCESS;
}

uint32_t pmix_net_prefix2netmask(uint32_t prefixlen)
{
    PMIX_HIDE_UNUSED_PARAMS(prefixlen);
    return 0;
}

bool pmix_net_islocalhost(const struct sockaddr *addr)
{
    PMIX_HIDE_UNUSED_PARAMS(addr);
    return false;
}

bool pmix_net_samenetwork(const struct sockaddr_storage *addr1,
                          const struct sockaddr_storage *addr2,
                          uint32_t prefixlen)
{
    PMIX_HIDE_UNUSED_PARAMS(addr1, addr2, prefixlen);
    return false;
}

bool pmix_net_addr_isipv6linklocal(const struct sockaddr *addr)
{
    PMIX_HIDE_UNUSED_PARAMS(addr);
    return false;
}

bool pmix_net_addr_isipv4public(const struct sockaddr *addr)
{
    PMIX_HIDE_UNUSED_PARAMS(addr);
    return false;
}

char *pmix_net_get_hostname(const struct sockaddr *addr)
{
    /* the contract is that a caller may print the answer without checking
     * it, so this cannot be NULL just because the platform is thin */
    PMIX_HIDE_UNUSED_PARAMS(addr);
    return pmix_hostname_unknown;
}

int pmix_net_get_port(const struct sockaddr *addr)
{
    PMIX_HIDE_UNUSED_PARAMS(addr);
    return -1;
}

#endif /* HAVE_STRUCT_SOCKADDR_IN */
