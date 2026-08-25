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
 * Copyright (c) 2008      Sun Microsystems, Inc.  All rights reserved.
 * Copyright (c) 2013      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2016-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* @file */

#ifndef PMIX_PIF_UTIL_
#define PMIX_PIF_UTIL_

#include "src/include/pmix_config.h"

#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif
#ifdef HAVE_NETINET_IN_H
#    include <netinet/in.h>
#endif

#include "pmix_common.h"
#include "src/class/pmix_list.h"

#define PMIX_IF_NAMESIZE 256

BEGIN_C_DECLS

#define PMIX_PIF_FORMAT_ADDR(n)                                                        \
    (((n) >> 24) & 0x000000FF), (((n) >> 16) & 0x000000FF), (((n) >> 8) & 0x000000FF), \
        ((n) &0x000000FF)

#define PMIX_PIF_ASSEMBLE_FABRIC(n1, n2, n3, n4)                                           \
    (((n1) << 24) & 0xFF000000) | (((n2) << 16) & 0x00FF0000) | (((n3) << 8) & 0x0000FF00) \
        | ((n4) &0x000000FF)

typedef struct pmix_pif_t {
    pmix_list_item_t super;
    char if_name[PMIX_IF_NAMESIZE + 1];
    int if_index;
    uint32_t if_kernel_index;
    uint16_t af_family;
    int if_flags;
    int if_speed;
    struct sockaddr_storage if_addr;
    uint32_t if_mask;
    uint32_t if_bandwidth;
    uint8_t if_mac[6];
    int ifmtu; /* Can't use if_mtu because of a
                #define collision on some BSDs */
} pmix_pif_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_pif_t);


/* "global" list of available interfaces */
PMIX_EXPORT extern pmix_list_t pmix_if_list;

/* global flags */
PMIX_EXPORT extern bool pmix_if_do_not_resolve;

/**
 *  Lookup an interface by address and return its name.
 *
 *  @param if_addr (IN)   Interface address (hostname or dotted-quad)
 *  @param if_name (OUT)  Interface name buffer
 *  @param size    (IN)   Interface name buffer size
 */
PMIX_EXPORT int pmix_ifaddrtoname(const char *if_addr, char *if_name, int size);

/**
 *  Lookup an interface by name and return its pmix_list index.
 *
 *  @param if_name (IN)  Interface name
 *  @return              Interface pmix_list index
 */
PMIX_EXPORT int pmix_ifnametoindex(const char *if_name);

/**
 *  Lookup an interface by name and return its kernel index.
 *
 *  @param if_name (IN)  Interface name
 *  @return              Interface kernel index
 */
PMIX_EXPORT int pmix_ifnametokindex(const char *if_name);

/*
 *  Attempt to resolve an address (given as either IPv4/IPv6 string
 *  or hostname) and return the kernel index of the interface
 *  that is on the same network as the specified address
 */
PMIX_EXPORT int pmix_ifaddrtokindex(const char *if_addr);

/**
 *  Lookup an interface by pmix_list index and return its kernel index.
 *
 *  @param if_name (IN)  Interface pmix_list index
 *  @return              Interface kernel index
 */
PMIX_EXPORT int pmix_ifindextokindex(int if_index);

/**
 *  Returns the number of available interfaces.
 */
PMIX_EXPORT int pmix_ifcount(void);

/**
 *  Returns the index of the first available interface, or -1 when no
 *  interface has been discovered.
 */
PMIX_EXPORT int pmix_ifbegin(void);

/**
 *  Lookup the current position in the interface list by
 *  index and return the next available index (if it exists).
 *
 *  @param if_index   Returns the next available index from the
 *                    current position.
 */
PMIX_EXPORT int pmix_ifnext(int if_index);

/**
 *  Lookup an interface by index and return its name.
 *
 *  @param if_index (IN)  Interface index
 *  @param if_name (OUT)  Interface name buffer
 *  @param size (IN)      Interface name buffer size
 */
PMIX_EXPORT int pmix_ifindextoname(int if_index, char *if_name, int);

/**
 *  Lookup an interface by kernel index and return its name.
 *
 *  @param if_index (IN)  Interface kernel index
 *  @param if_name (OUT)  Interface name buffer
 *  @param size (IN)      Interface name buffer size
 */
PMIX_EXPORT int pmix_ifkindextoname(int if_kindex, char *if_name, int);

/**
 *  Lookup an interface by index and return its primary address.
 *
 *  @param if_index (IN)  Interface index
 *  @param if_name (OUT)  Interface address buffer
 *  @param size (IN)      Interface address buffer size
 */
PMIX_EXPORT int pmix_ifindextoaddr(int if_index, struct sockaddr *, unsigned int);

/**
 *  Lookup an interface by kernel index and return its primary address.
 *
 *  Caution: a kernel index does not identify a single address.  An
 *  interface carrying both an IPv4 and an IPv6 address occupies one
 *  pmix_if_list entry per address, and those entries share a kernel
 *  index, so this returns whichever address discovery found first -- on
 *  Linux routinely the IPv6 one, always so for loopback.  Callers that
 *  need an address of a particular family must walk pmix_if_list
 *  themselves and check each entry's sa_family; they cannot assume the
 *  answer here is IPv4.
 *
 *  @param if_kindex (IN) Interface kernel index
 *  @param if_addr (OUT)  Interface address buffer
 *  @param length (IN)    Interface address buffer size
 */
PMIX_EXPORT int pmix_ifkindextoaddr(int if_kindex, struct sockaddr *if_addr, unsigned int length);

/**
 *  Lookup an interface by index and return its network mask (in CIDR
 *  notation -- NOT the actual netmask itself!).
 *
 *  @param if_index (IN)  Interface index
 *  @param if_name (OUT)  Interface address buffer
 *  @param size (IN)      Interface address buffer size
 */
PMIX_EXPORT int pmix_ifindextomask(int if_index, uint32_t *, int);

/**
 *  Lookup an interface by index and return its MAC address.
 *
 *  @param if_index (IN)  Interface index
 *  @param if_mac (OUT)   Interface's MAC address
 */
PMIX_EXPORT int pmix_ifindextomac(int if_index, uint8_t if_mac[6]);

/**
 *  Lookup an interface by index and return its MTU.
 *
 *  @param if_index (IN)  Interface index
 *  @param if_mtu (OUT)   Interface's MTU
 */
PMIX_EXPORT int pmix_ifindextomtu(int if_index, int *mtu);

/**
 *  Lookup an interface by index and return its flags.
 *
 *  @param if_index (IN)  Interface index
 *  @param if_flags (OUT) Interface flags
 */
PMIX_EXPORT int pmix_ifindextoflags(int if_index, uint32_t *);

/**
 * Determine if given hostname / IP address is a local address
 *
 * @param hostname (IN)    Hostname (or stringified IP address)
 * @return                 true if \c hostname is local, false otherwise
 */
PMIX_EXPORT bool pmix_ifislocal(const char *hostname);

/**
 * Convert a dot-delimited network tuple to an IP address
 *
 * The tuple may be partial ("192.168"), in which case the mask is
 * inferred from the number of dots, or it may carry an explicit
 * "/N" prefix length or "/a.b.c.d" dotted netmask.
 *
 * Note that the mask handed back here is a real bit mask in *host*
 * byte order -- unlike pmix_ifindextomask(), which yields a CIDR
 * prefix length.
 *
 * @param addr (IN) character string tuple
 * @param net (OUT) Pointer to returned network address; may be NULL
 * @param mask (OUT) Pointer to returned netmask; may be NULL
 * @return PMIX_SUCCESS if no problems encountered
 * @return PMIX_ERR_FABRIC_NOT_PARSEABLE if the tuple, the prefix
 *         length, or the dotted netmask is malformed.  Neither
 *         out-parameter can be relied on in that case.
 */
PMIX_EXPORT int pmix_iftupletoaddr(const char *addr, uint32_t *net, uint32_t *mask);

/**
 * Determine if given interface is loopback
 *
 *  @param if_index (IN)  Interface index
 */
PMIX_EXPORT bool pmix_ifisloopback(int if_index);

/*
 * Determine if a specified interface is included in a NULL-terminated argv array
 */
PMIX_EXPORT int pmix_ifmatches(int kidx, char **nets);

/*
 * Provide a list of strings that contain all known aliases for this node
 */
PMIX_EXPORT void pmix_ifgetaliases(char ***aliases);

END_C_DECLS

#endif
