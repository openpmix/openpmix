/*
 * Copyright (c) 2004-2010 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2013-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014-2015 Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016      Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* The TCP transport: its listening socket, on a loopback interface, and
 * its tcp4:// and tcp6:// URIs. */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"
#include "src/include/pmix_socket_errno.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif
#ifdef HAVE_NETINET_IN_H
#    include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#    include <arpa/inet.h>
#endif

#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_fd.h"
#include "src/util/pmix_if.h"
#include "src/util/pmix_net.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"

#include "src/mca/ptl/base/base.h"
#include "src/mca/ptl/tcp/ptl_tcp.h"

static pmix_status_t tcp_setup_listener(pmix_info_t info[], size_t ninfo, pmix_listener_t *lt);
static pmix_status_t uri_to_addr(const char *uri, struct sockaddr_storage *connection,
                                 size_t *len);
static void tcp_close(void);

PMIX_EXPORT pmix_ptl_module_t pmix_ptl_tcp_module = {
    .name = "tcp",
    .setup_listener = tcp_setup_listener,
    .uri_to_addr = uri_to_addr,
    .close = tcp_close
};

static void tcp_close(void)
{
    memset(&pmix_mca_ptl_tcp_component.connection, 0, sizeof(struct sockaddr_storage));
}

/* A port directive is a single port as a PMIX_INT, or a list or range as
 * a PMIX_STRING. Zero, a negative number, and a NULL string all ask for
 * the ephemeral port. A number too large to be a port is refused rather
 * than bound: the bind loop narrows it to 16 bits, so 70000 used to
 * listen on 4464. */
static pmix_status_t port_directive(const pmix_info_t *info, char ***ports)
{
    char num[16];

    if (PMIX_INT == info->value.type) {
        if (65535 < info->value.data.integer) {
            pmix_output(0, "ptl:tcp:setup_listener: %s value %d is not a port number",
                        info->key, info->value.data.integer);
            return PMIX_ERR_BAD_PARAM;
        }
        snprintf(num, sizeof(num), "%d",
                 (0 < info->value.data.integer) ? info->value.data.integer : 0);
        return pmix_ptl_base_set_ports(num, ports);
    }
    if (PMIX_STRING == info->value.type) {
        return pmix_ptl_base_set_ports(info->value.data.string, ports);
    }
    /* any other type was never acted on - keep it that way */
    return PMIX_SUCCESS;
}


/* Convert the port field of a URI. It has to be the whole of what follows
 * the separator and a port a socket can actually be connected to: atoi()
 * read "" and "x" as 0 and wrapped 70000 onto 4464, so a mistyped URI
 * connected to some other port on the host instead of being refused */
static bool parse_port(const char *str, uint16_t *port)
{
    char *end;
    unsigned long val;

    if (!isdigit((unsigned char) str[0])) {
        return false;
    }
    errno = 0;
    val = strtoul(str, &end, 10);
    if (0 != errno || '\0' != *end || 0 == val || 65535 < val) {
        return false;
    }
    *port = htons((uint16_t) val);
    return true;
}


/* The address a tcp4:// or tcp6:// URI names. Any other scheme is not
 * ours, and is left to another transport */
static pmix_status_t uri_to_addr(const char *uri, struct sockaddr_storage *connection,
                                 size_t *len)
{
    char *p = NULL, *p2, *host;
    struct sockaddr_in *in;
    struct sockaddr_in6 *in6;

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "ptl:tcp setup connection to %s", uri);

    /* a URI of any other scheme, or none, is another transport's to take
     * - so the scheme is looked at before anything else */
    if (NULL == uri || (0 != strncmp(uri, "tcp4://", 7) && 0 != strncmp(uri, "tcp6://", 7))) {
        return PMIX_ERR_NOT_SUPPORTED;
    }
    memset(connection, 0, sizeof(struct sockaddr_storage));
    /* an address follows the scheme - indexing past a string that ends
     * there would read off the end of what reached us from a rendezvous
     * file or an environment variable */
    if ('\0' == uri[7]) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERR_BAD_PARAM;
    }
    if (0 == strncmp(uri, "tcp4", 4)) {
        /* need to skip the tcp4: part */
        p = strdup(&uri[7]);
        if (NULL == p) {
            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
            return PMIX_ERR_NOMEM;
        }

        /* separate the IP address from the port */
        p2 = strrchr(p, ':');
        if (NULL == p2) {
            free(p);
            PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
            return PMIX_ERR_BAD_PARAM;
        }
        *p2 = '\0';
        p2++;
        host = p;
        /* load the address */
        in = (struct sockaddr_in *) connection;
        in->sin_family = AF_INET;
        in->sin_addr.s_addr = inet_addr(host);
        if (in->sin_addr.s_addr == INADDR_NONE || !parse_port(p2, &in->sin_port)) {
            free(p);
            PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
            return PMIX_ERR_BAD_PARAM;
        }
        *len = sizeof(struct sockaddr_in);
    } else if (0 == strncmp(uri, "tcp6", 4)) {
        /* need to skip the tcp6: part */
        p = strdup(&uri[7]);
        if (NULL == p) {
            PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
            return PMIX_ERR_NOMEM;
        }

        p2 = strrchr(p, ':');
        if (NULL == p2) {
            free(p);
            PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
            return PMIX_ERR_BAD_PARAM;
        }
        *p2 = '\0';
        p2++;
        /* nothing before the port separator - there is no last character
         * to inspect, and p[strlen(p) - 1] would be p[-1] */
        if ('\0' == p[0]) {
            free(p);
            PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
            return PMIX_ERR_BAD_PARAM;
        }
        if (']' == p[strlen(p) - 1]) {
            p[strlen(p) - 1] = '\0';
        }
        if ('[' == p[0]) {
            host = &p[1];
        } else {
            host = &p[0];
        }
        /* load the address */
        in6 = (struct sockaddr_in6 *) connection;
        in6->sin6_family = AF_INET6;
        if (0 == inet_pton(AF_INET6, host, (void *) &in6->sin6_addr)) {
            pmix_output(0, "ptl_tcp_parse_uri: Could not convert %s\n", host);
            free(p);
            PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
            return PMIX_ERR_BAD_PARAM;
        }
        if (!parse_port(p2, &in6->sin6_port)) {
            free(p);
            PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
            return PMIX_ERR_BAD_PARAM;
        }
        *len = sizeof(struct sockaddr_in6);
    } else {
        /* not a scheme we speak */
        return PMIX_ERR_NOT_SUPPORTED;
    }
    if (NULL != p) {
        free(p);
    }

    return PMIX_SUCCESS;
}


/*
 * Go through a comma-delimited list of interface names and subnets
 * (a.b.c.d/e - IPv4 only), resolving each subnet to the names of the
 * interfaces on it. *interfaces gets the names, or NULL for a list with no
 * entries. An entry that is not a name or a subnet is reported, and the
 * list refused with PMIX_ERR_BAD_PARAM. A subnet no interface is on is
 * reported and passed over - a list can name subnets for hosts other than
 * this one - but an include list none of whose entries is on this host is
 * refused with PMIX_ERR_NOT_FOUND: it would otherwise read as no list at
 * all, and the listener would open where it was told not to.
 */
pmix_status_t pmix_ptl_tcp_split_and_resolve(const char *orig_str, const char *name,
                                             char ***interfaces_out)
{
    int i, ret, if_index;
    char **argv, **interfaces, *str;
    char if_name[PMIX_IF_NAMESIZE];
    struct sockaddr_storage argv_inaddr, if_inaddr;
    uint32_t argv_prefix;
    bool found;

    *interfaces_out = NULL;
    /* Sanity check */
    if (NULL == orig_str) {
        return PMIX_SUCCESS;
    }

    /* "" and "," split to nothing at all - a NULL, not an empty array */
    argv = PMIx_Argv_split(orig_str, ',');
    if (NULL == argv) {
        return PMIX_SUCCESS;
    }
    interfaces = NULL;
    for (i = 0; NULL != argv[i]; ++i) {
        if (isalpha((unsigned char) argv[i][0])) {
            /* This is an interface name. If not already in the interfaces array, add it */
            PMIx_Argv_append_unique_nosize(&interfaces, argv[i]);
            pmix_output_verbose(20,
                                pmix_ptl_base_framework.framework_output,
                                "ptl:tcp: Using interface: %s ", argv[i]);
            continue;
        }

        /* Found a subnet notation.  Convert it to an IP
           address/netmask.  Get the prefix first. */
        argv_prefix = 0;
        str = strchr(argv[i], '/');
        if (NULL == str) {
            pmix_show_help("help-ptl-base.txt", "bad if_inexclude", true,
                           name, pmix_globals.hostname, argv[i],
                           "Invalid specification (missing \"/\")");
            PMIx_Argv_free(argv);
            PMIx_Argv_free(interfaces);
            return PMIX_ERR_BAD_PARAM;
        }
        *str = '\0';
        argv_prefix = atoi(str + 1);

        /* Now convert the IPv4 address */
        ((struct sockaddr *) &argv_inaddr)->sa_family = AF_INET;
        ret = inet_pton(AF_INET, argv[i], &((struct sockaddr_in *) &argv_inaddr)->sin_addr);
        *str = '/';

        if (1 != ret) {
            pmix_show_help("help-ptl-base.txt", "bad if_inexclude", true,
                           name, pmix_globals.hostname, argv[i],
                           "Invalid specification (inet_pton() failed)");
            PMIx_Argv_free(argv);
            PMIx_Argv_free(interfaces);
            return PMIX_ERR_BAD_PARAM;
        }
        pmix_output_verbose(20, pmix_ptl_base_framework.framework_output,
                            "ptl:tcp: Searching for %s address+prefix: %s / %u", name,
                            pmix_net_get_hostname((struct sockaddr *) &argv_inaddr), argv_prefix);

        /* Go through all interfaces and see if we can find a match */
        found = false;
        for (if_index = pmix_ifbegin(); if_index >= 0; if_index = pmix_ifnext(if_index)) {
            pmix_ifindextoaddr(if_index,
                               (struct sockaddr*) &if_inaddr,
                               sizeof(if_inaddr));
            if (pmix_net_samenetwork(&argv_inaddr, &if_inaddr, argv_prefix)) {
                /* We found a match. If it's not already in the interfaces array,
                   add it. If it's already in the array, treat it as a match */
                found = true;
                pmix_ifindextoname(if_index, if_name, sizeof(if_name));
                PMIx_Argv_append_unique_nosize(&interfaces, if_name);
                pmix_output_verbose(20,
                                    pmix_ptl_base_framework.framework_output,
                                    "ptl:tcp: Found match: %s (%s)",
                                    pmix_net_get_hostname((struct sockaddr*) &if_inaddr),
                                    if_name);
            }

        }
        /* If we didn't find a match, report it but keep trying */
        if (!found) {
            pmix_show_help("help-ptl-base.txt", "invalid if_inexclude", true,
                           name, pmix_globals.hostname, argv[i],
                           "Did not find interface matching this subnet");
        }
    }

    if (NULL == interfaces && 0 == strcmp(name, "include")) {
        pmix_show_help("help-ptl-base.txt", "no-include-match", true,
                       pmix_globals.hostname, orig_str);
        PMIx_Argv_free(argv);
        return PMIX_ERR_NOT_FOUND;
    }
    PMIx_Argv_free(argv);
    *interfaces_out = interfaces;
    return PMIX_SUCCESS;
}


/* Open the listening socket, on a loopback interface: the TCP transport
 * serves peers of earlier releases on this host, and nothing else. The
 * listener gets the socket and its URI */
static pmix_status_t tcp_setup_listener(pmix_info_t info[], size_t ninfo, pmix_listener_t *lt)
{
    int flags = 0;
    int i, rc = 0, saveindex = -1;
    char **interfaces = NULL, **candidates = NULL, **ports;
    bool including = false;
    char name[32];
    struct sockaddr_storage my_ss;
    struct sockaddr_storage *conn = &pmix_mca_ptl_tcp_component.connection;
    int kindex;
    pmix_socklen_t addrlen;
    char *prefix;
    char myconnhost[PMIX_MAXHOSTNAMELEN] = {0};
    int myport;
    char *leftover;
    size_t n;
    uint16_t port = 0;
    long portnum;

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "ptl:tcp:setup_listener");

    /* the directives meant for this transport */
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_TCP_IF_INCLUDE)) {
            if (!pmix_ptl_base_replace_string(&info[n], &pmix_ptl_base.if_include, &rc)) {
                return rc;
            }

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_TCP_IF_EXCLUDE)) {
            if (!pmix_ptl_base_replace_string(&info[n], &pmix_ptl_base.if_exclude, &rc)) {
                return rc;
            }

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_TCP_IPV4_PORT)) {
            rc = port_directive(&info[n], &pmix_ptl_base.ipv4_ports);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }

#if PMIX_ENABLE_IPV6
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_TCP_IPV6_PORT)) {
            rc = port_directive(&info[n], &pmix_ptl_base.ipv6_ports);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
#endif

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_TCP_DISABLE_IPV4)) {
            pmix_ptl_base.disable_ipv4_family = PMIX_INFO_TRUE(&info[n]);

#if PMIX_ENABLE_IPV6
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_TCP_DISABLE_IPV6)) {
            pmix_ptl_base.disable_ipv6_family = PMIX_INFO_TRUE(&info[n]);
#endif

        }
    }

    if (NULL != pmix_ptl_base.if_include && NULL != pmix_ptl_base.if_exclude) {
        pmix_show_help("help-ptl-base.txt", "include-exclude", true, pmix_ptl_base.if_include,
                       pmix_ptl_base.if_exclude);
        return PMIX_ERR_SILENT;
    }

    /* if interface include was given, construct a list
     * of those interfaces which match the specifications - remember,
     * the includes could be given as named interfaces, IP addrs, or
     * subnet+mask
     */
    if (NULL != pmix_ptl_base.if_include) {
        rc = pmix_ptl_tcp_split_and_resolve(pmix_ptl_base.if_include, "include", &interfaces);
        including = true;
    } else if (NULL != pmix_ptl_base.if_exclude) {
        rc = pmix_ptl_tcp_split_and_resolve(pmix_ptl_base.if_exclude, "exclude", &interfaces);
        including = false;
    }
    if (PMIX_SUCCESS != rc) {
        /* said why */
        return PMIX_ERR_SILENT;
    }

    /* look at all available interfaces and pick a loopback one - we are
     * only talking locally */
    for (i = pmix_ifbegin(); i >= 0; i = pmix_ifnext(i)) {
        if (PMIX_SUCCESS != pmix_ifindextoaddr(i, (struct sockaddr *) &my_ss, sizeof(my_ss))) {
            pmix_output(0, "ptl:tcp:setup_listener: problems getting address for index %i (kernel index %i)\n", i,
                        pmix_ifindextokindex(i));
            continue;
        }
        /* ignore non-ip4/6 interfaces */
        if (AF_INET != my_ss.ss_family && AF_INET6 != my_ss.ss_family) {
            continue;
        }
        /* get the name for diagnostic purposes */
        pmix_ifindextoname(i, name, sizeof(name));

        /* ignore any virtual interfaces */
        if (0 == strncmp(name, "vir", 3)) {
            continue;
        }
        /* ignore any interfaces in a disabled family */
        if (AF_INET == my_ss.ss_family) {
            if (pmix_ptl_base.disable_ipv4_family) {
                continue;
            }
#if PMIX_ENABLE_IPV6
        } else if (AF_INET6 == my_ss.ss_family) {
            if (pmix_ptl_base.disable_ipv6_family) {
                continue;
            }
#endif
        } else {
            /* ignore any other type */
            continue;
        }

        /* get the kernel index */
        kindex = pmix_ifindextokindex(i);
        if (kindex <= 0) {
            continue;
        }

        // cache the candidates
        PMIx_Argv_append_nosize(&candidates, name);

        pmix_output_verbose(10, pmix_ptl_base_framework.framework_output,
                            "WORKING INTERFACE %d KERNEL INDEX %d FAMILY: %s", i, kindex,
                            (AF_INET == my_ss.ss_family) ? "V4" : "V6");
        /* handle include/exclude directives */
        if (NULL != interfaces) {
            /* check for match */
            rc = pmix_ifmatches(kindex, interfaces);
            /* if one of the network specifications isn't parseable, then
             * error out as we can't do what was requested
             */
            if (PMIX_ERR_FABRIC_NOT_PARSEABLE == rc) {
                pmix_show_help("help-ptl-base.txt", "not-parseable", true);
                PMIx_Argv_free(interfaces);
                PMIx_Argv_free(candidates);
                return PMIX_ERR_BAD_PARAM;
            }
            /* if we are including, then ignore this if not present */
            if (including) {
                if (PMIX_SUCCESS != rc) {
                    pmix_output_verbose(
                        10, pmix_ptl_base_framework.framework_output,
                        "ptl:tcp:setup_listener rejecting interface %s (not in include list)", name);
                    continue;
                }
            } else {
                /* we are excluding, so ignore if present */
                if (PMIX_SUCCESS == rc) {
                    pmix_output_verbose(10, pmix_ptl_base_framework.framework_output,
                                        "ptl:tcp:setup_listener rejecting interface %s (in exclude list)",
                                        name);
                    continue;
                }
            }
        }

        /* an address on the loopback device that is not a loopback
         * address is reached from other hosts too, and does not count */
        if (pmix_ifisloopback(i) && pmix_net_islocalhost((struct sockaddr *) &my_ss)) {
            pmix_output_verbose(5, pmix_ptl_base_framework.framework_output,
                                "ptl:tcp:setup_listener loopback interface %s found", name);
            saveindex = i;
            break;
        }
    }
    /* cleanup */
    if (NULL != interfaces) {
        PMIx_Argv_free(interfaces);
    }

    /* with no loopback interface left by the directives there is nowhere
     * to listen: a public interface is reached from other hosts */
    if (saveindex < 0) {
        prefix = PMIx_Argv_join(candidates, ',');
        pmix_show_help("help-ptl-base.txt", "no-loopback-interfaces", true,
                       (NULL == pmix_ptl_base.if_include) ? "NULL" : pmix_ptl_base.if_include,
                       (NULL == pmix_ptl_base.if_exclude) ? "NULL" : pmix_ptl_base.if_exclude,
                       prefix);
        PMIx_Argv_free(candidates);
        free(prefix);
        return PMIX_ERR_SILENT;
    }
    PMIx_Argv_free(candidates);

    /* save the connection */
    if (PMIX_SUCCESS
        != pmix_ifindextoaddr(saveindex, (struct sockaddr *) conn,
                              sizeof(struct sockaddr_storage))) {
        pmix_output(0, "ptl:tcp: problems getting address for kernel index %i\n",
                    pmix_ifindextokindex(saveindex));
        return PMIX_ERR_NOT_AVAILABLE;
    }

    // set the ports to scan based on the connection we selected
    if (AF_INET == conn->ss_family) {
        ports = pmix_ptl_base.ipv4_ports;
#if PMIX_ENABLE_IPV6
    } else if (AF_INET6 == conn->ss_family) {
        ports = pmix_ptl_base.ipv6_ports;
#endif
    } else {
        /* unrecognized family type - shouldn't be possible as we only
         * included IPv4 and IPv6 interfaces, but this is needed to
         * silence warnings */
        return PMIX_ERR_NOT_SUPPORTED;
    }

    rc = PMIX_ERR_NOT_AVAILABLE;
    for (n=0; NULL != ports[n]; n++) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:setup_listener - trying port %s", ports[n]);

        /* get the port number - a list or range from the parser can
         * still name one that does not fit in 16 bits, and narrowing it
         * would bind some other port instead */
        errno = 0;
        portnum = strtol(ports[n], &leftover, 10);
        if (0 != errno || leftover == ports[n] || '\0' != *leftover ||
            0 > portnum || 65535 < portnum) {
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "ptl:setup_listener - skipping invalid port %s", ports[n]);
            continue;
        }
        /* convert it to network-byte-order */
        port = htons((uint16_t) portnum);

        /* set the port */
        if (AF_INET == conn->ss_family) {
            ((struct sockaddr_in *) conn)->sin_port = port;
            addrlen = sizeof(struct sockaddr_in);
            if (0 != port) {
                flags = 1;
            }
#if PMIX_ENABLE_IPV6
        } else if (AF_INET6 == conn->ss_family) {
            ((struct sockaddr_in6 *) conn)->sin6_port = port;
            addrlen = sizeof(struct sockaddr_in6);
            if (0 != port) {
                flags = 1;
            }
#endif
        }

        /* create a listen socket for incoming connection attempts */
        lt->socket = socket(conn->ss_family, SOCK_STREAM | PMIX_SOCK_CLOEXEC, 0);
        if (lt->socket < 0) {
            pmix_output(0, "ptl:tcp:create_listen: socket() failed: %s (%d)\n",
                        strerror(pmix_socket_errno), pmix_socket_errno);
            rc = PMIX_ERR_OUT_OF_RESOURCE;
            goto sockerror;
        }

        /* set reusing ports flag */
        if (setsockopt(lt->socket, SOL_SOCKET, SO_REUSEADDR, (const char *) &flags, sizeof(flags)) < 0) {
            pmix_output(0,
                        "ptl:tcp:create_listen: unable to set the "
                        "SO_REUSEADDR option (%s:%d)\n",
                        strerror(pmix_socket_errno), pmix_socket_errno);
            rc = PMIX_ERROR;
            goto sockerror;
        }

        /* Set the socket to close-on-exec so that no children inherit
         * this FD */
        if (pmix_fd_set_cloexec(lt->socket) != PMIX_SUCCESS) {
            rc = PMIX_ERROR;
            goto sockerror;
        }

        if (bind(lt->socket, (struct sockaddr *) conn, addrlen) < 0) {
            if ((EADDRINUSE == pmix_socket_errno) || (EADDRNOTAVAIL == pmix_socket_errno)) {
                /* this port is taken - drop the socket we opened for it
                 * before moving on, or we leak one descriptor per busy
                 * port in the range we were given */
                CLOSE_THE_SOCKET(lt->socket);
                continue;
            }
            pmix_output(0, "ptl:tcp:create_listen: bind() failed for socket %d "
                        "storage size %u: %s (%d)\n", lt->socket, (unsigned) addrlen,
                        strerror(pmix_socket_errno), pmix_socket_errno);
            rc = PMIX_ERROR;
            goto sockerror;
        }
        rc = PMIX_SUCCESS;
        break;  // got one that works
    }
    if (PMIX_SUCCESS != rc) {
        /* every port we were given is in use - say so instead of falling
         * thru and advertising an unbound socket */
        pmix_show_help("help-ptl-base.txt", "no-available-ports", true,
                       pmix_globals.hostname);
        rc = PMIX_ERR_SILENT;
        goto sockerror;
    }

    /* resolve assigned port */
    if (getsockname(lt->socket, (struct sockaddr *) conn, &addrlen) < 0) {
        pmix_output(0, "ptl:tcp:create_listen: getsockname(): %s (%d)",
                    strerror(pmix_socket_errno), pmix_socket_errno);
        rc = PMIX_ERROR;
        goto sockerror;
    }

    /* setup listen backlog to maximum allowed by kernel */
    if (listen(lt->socket, SOMAXCONN) < 0) {
        pmix_output(0, "ptl:tcp:create_listen: listen() failed: %s (%d)\n",
                    strerror(pmix_socket_errno), pmix_socket_errno);
        rc = PMIX_ERROR;
        goto sockerror;
    }

    /* set socket up to be non-blocking, otherwise accept could block */
    if ((flags = fcntl(lt->socket, F_GETFL, 0)) < 0) {
        pmix_output(0, "ptl:tcp:create_listen: fcntl(F_GETFL) failed: %s (%d)\n",
                    strerror(pmix_socket_errno), pmix_socket_errno);
        rc = PMIX_ERROR;
        goto sockerror;
    }
    flags |= O_NONBLOCK;
    if (fcntl(lt->socket, F_SETFL, flags) < 0) {
        pmix_output(0, "ptl:tcp:create_listen: fcntl(F_SETFL) failed: %s (%d)\n",
                    strerror(pmix_socket_errno), pmix_socket_errno);
        rc = PMIX_ERROR;
        goto sockerror;
    }

    if (AF_INET == conn->ss_family) {
        prefix = "tcp4://";
        myport = ntohs(((struct sockaddr_in *) conn)->sin_port);
        inet_ntop(AF_INET, &((struct sockaddr_in *) conn)->sin_addr,
                  myconnhost, PMIX_MAXHOSTNAMELEN - 1);
    } else if (AF_INET6 == conn->ss_family) {
        prefix = "tcp6://";
        myport = ntohs(((struct sockaddr_in6 *) conn)->sin6_port);
        inet_ntop(AF_INET6, &((struct sockaddr_in6 *) conn)->sin6_addr,
                  myconnhost, PMIX_MAXHOSTNAMELEN - 1);
    } else {
        rc = PMIX_ERR_NOT_SUPPORTED;
        goto sockerror;
    }

    if (0 > pmix_asprintf(&lt->uri, "%s.%u;%s%s:%d", pmix_globals.myid.nspace,
                          pmix_globals.myid.rank, prefix, myconnhost, myport)
        || NULL == lt->uri) {
        rc = PMIX_ERR_NOMEM;
        goto sockerror;
    }
    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "ptl:tcp URI %s", lt->uri);

    return PMIX_SUCCESS;

sockerror:
    CLOSE_THE_SOCKET(lt->socket);
    return rc;
}
