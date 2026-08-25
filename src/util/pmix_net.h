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
 * Copyright (c) 2017      Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* @file */

#ifndef PMIX_UTIL_NET_H
#define PMIX_UTIL_NET_H

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

BEGIN_C_DECLS

/**
 * Initialize the network helper subsystem
 *
 * Initialize the network helper subsystem.  Should be called exactly
 * once for any process that will use any function in the network
 * helper subsystem.
 *
 * @retval PMIX_SUCCESS   Success
 * @retval PMIX_ERR_OUT_OF_RESOURCE  No thread-specific-data key was
 *                        available
 * @retval PMIX_ERR_NOMEM Out of memory
 */
PMIX_EXPORT int pmix_net_init(void);

/**
 * Finalize the network helper subsystem
 *
 * Finalize the network helper subsystem.  Should be called exactly
 * once for any process that will use any function in the network
 * helper subsystem.
 *
 * @retval PMIX_SUCCESS   Success
 */
PMIX_EXPORT int pmix_net_finalize(void);

/**
 * Parse the pmix_net_private_ipv4 MCA parameter
 *
 * Build the table of "private" IPv4 networks that
 * pmix_net_addr_isipv4public() consults, from the current value of the
 * pmix_net_private_ipv4 MCA parameter.  This is deliberately *not* part
 * of pmix_net_init(): that runs from pmix_init_util(), which completes
 * before pmix_register_params() has given the variable its value, so a
 * parse there would only ever see the NULL initializer and leave every
 * address looking public.  Call this once the parameters are registered;
 * calling it again simply replaces the table.
 *
 * @retval PMIX_SUCCESS   Success - including the case where the
 *                        parameter is empty, which leaves no network
 *                        classified as private
 * @retval PMIX_ERR_NOMEM Out of memory
 */
PMIX_EXPORT pmix_status_t pmix_net_setup_private_ipv4(void);

/**
 * Calculate netmask in network byte order from CIDR notation
 *
 * @param prefixlen (IN)  CIDR prefixlen
 * @return                netmask in network byte order
 */
PMIX_EXPORT uint32_t pmix_net_prefix2netmask(uint32_t prefixlen);

/**
 * Determine if given IP address is in the localhost range
 *
 * Determine if the given IP address is in the localhost range, meaning
 * that it can't be used to connect to machines outside the current host.
 * That is 127.0.0.0/8 for IPv4, and ::1 or an IPv4-mapped address whose
 * embedded IPv4 address is itself in 127.0.0.0/8 for IPv6.
 *
 * @param addr             struct sockaddr of IP address
 * @return                 true if \c addr is a localhost address,
 *                         false otherwise.
 */
PMIX_EXPORT bool pmix_net_islocalhost(const struct sockaddr *addr);

/**
 * Are we on the same network?
 *
 * Compares the leading \c prefixlen bits of the two addresses, which
 * must be of the same family.  A \c prefixlen of 0 means "caller does
 * not know" and is taken as the family's usual default - /32 for IPv4
 * (an exact match) and /64 for IPv6 (what SLAAC hands out); an IPv4
 * prefix is clamped at 32 and an IPv6 one at 128.
 *
 * @param addr1             struct sockaddr of address
 * @param addr2             struct sockaddr of address
 * @param prefixlen         netmask (either CIDR or IPv6 prefixlen)
 * @return                  true if \c addr1 and \c addr2 are on the
 *                          same net, false otherwise.
 */
PMIX_EXPORT bool pmix_net_samenetwork(const struct sockaddr_storage *addr1,
                                      const struct sockaddr_storage *addr2,
                                      uint32_t prefixlen);

/**
 * Is the given address a link-local IPv6 address?  Returns false for IPv4
 * address.
 *
 * @param addr      address as struct sockaddr
 * @return          true, if \c addr is IPv6 link-local, false otherwise
 */
PMIX_EXPORT bool pmix_net_addr_isipv6linklocal(const struct sockaddr *addr);

/**
 * Is the given address a public IPv4 address?  Returns false for IPv6
 * address.
 *
 * @param addr      address as struct sockaddr
 * @return          true, if \c addr is IPv4 public, false otherwise
 */
PMIX_EXPORT bool pmix_net_addr_isipv4public(const struct sockaddr *addr);

/**
 * Get string version of address
 *
 * Return the un-resolved address in a string format.  The string will
 * be returned in a per-thread static buffer and should not be freed
 * by the user; it is valid until this thread's next call.
 *
 * This never returns NULL - an address it cannot render (an unsupported
 * family, a failed conversion, no memory for the buffer) reads as
 * "UNKNOWN", so a caller that is about to print the answer does not have
 * to check it.
 *
 * @param addr              struct sockaddr of address
 * @return                  literal representation of \c addr
 */
PMIX_EXPORT char *pmix_net_get_hostname(const struct sockaddr *addr);

/**
 * Get port number from struct sockaddr
 *
 * Return the port number (as an integer) from either a struct
 * sockaddr_in or a struct sockaddr_in6.
 *
 * @param addr             struct sockaddr containing address
 * @return                 port number from \c addr in host byte order,
 *                         or -1 if it carries neither
 */
PMIX_EXPORT int pmix_net_get_port(const struct sockaddr *addr);

/**
 * Test if a string is actually an IP address
 *
 * Returns true if the string is of IPv4 or IPv6 address form
 */
PMIX_EXPORT bool pmix_net_isaddr(const char *name);

END_C_DECLS

#endif /* PMIX_UTIL_NET_H */
