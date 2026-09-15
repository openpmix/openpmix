/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2006 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <stdio.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif
#ifdef HAVE_DIRENT_H
#    include <dirent.h>
#endif
#ifdef HAVE_FCNTL_H
#    include <fcntl.h>
#endif
#include <ctype.h>
#include <errno.h>
#include <time.h>

#include "src/include/pmix_globals.h"
#include "src/include/pmix_socket_errno.h"
#include "src/runtime/pmix_progress_threads.h"
#include "src/mca/bfrops/base/base.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_net.h"
#include "src/util/pmix_os_path.h"
#include "src/util/pmix_if.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"
#include "src/util/pmix_string_copy.h"

#include "src/mca/ptl/base/base.h"
#include "src/mca/ptl/base/ptl_base_handshake.h"

/****    SUPPORTING FUNCTIONS    ****/
static void timeout(int sd, short args, void *cbdata);
static void retry_wait(const struct timeval *tv);
static pmix_status_t construct_message(pmix_peer_t *peer, char **msgout, size_t *sz,
                                       pmix_info_t *iptr, size_t niptr);
static pmix_status_t parse_conn_file(char *filename, bool optional, bool found_by_search,
                                     pmix_list_t *connections);
static bool search_candidate(const char *path, bool *isdir);

pmix_status_t pmix_ptl_base_set_peer(pmix_peer_t *peer, char **evar)
{
    pmix_status_t rc;
    char *vrs;
    pmix_bfrops_base_active_module_t *mod;
    char *ptr, *tmp, check[2], *eval;
    int major, minor;
    bool evalgiven;

    vrs = getenv("PMIX_VERSION");

    if (NULL == evar) {
        return PMIX_ERR_BAD_PARAM;
    }

    if (NULL == *evar) {
        eval = NULL;
        evalgiven = false;
    } else {
        // they are passing in the bfrops module to use
        eval = *evar;
        evalgiven = true;
    }

    PMIX_LIST_FOREACH(mod, &pmix_bfrops_globals.actives, pmix_bfrops_base_active_module_t) {
        // these are in priority order, so take the highest priority that we find
        /* every bfrops component is named for the wire version it
         * speaks - "v41", "v21" - so the digits follow the last 'v'.
         * A component that does not follow that convention cannot be
         * matched to a PMIX_SERVER_URIvNN variable at all */
        ptr = strrchr(mod->component->base.pmix_mca_component_name, 'v');
        if (NULL == ptr) {
            continue;
        }
        ++ptr;
        if (0 > pmix_asprintf(&tmp, "PMIX_SERVER_URI%s", ptr)) {
            return PMIX_ERR_NOMEM;
        }
        if (evalgiven) {
            if (0 != strcmp(tmp, eval)) {
                free(tmp);
                continue;
            }
        } else {
            if (NULL == (eval = getenv(tmp))) {
                free(tmp);
                continue;
            }
        }
        free(tmp);

        /* must use the v<ptr> bfrops module */
        if (0 > pmix_asprintf(&tmp, "v%s", ptr)) {
            return PMIX_ERR_NOMEM;
        }
        PMIX_BFROPS_SET_MODULE(rc, pmix_globals.mypeer, peer, tmp);
        free(tmp);
        if (PMIX_SUCCESS != rc) {
            continue;
        }

        check[1] = '\0';
        // must be at least one number in the version
        check[0] = ptr[0];
        major = strtoul(check, NULL, 10);
        // might not be a second - e.g., v4
        if (2 == strlen(ptr)) {
            check[0] = ptr[1];
            minor = strtoul(check, NULL, 10);
        } else {
            minor = 0;
        }
        /* we are talking to a vmajor.minor server */
        PMIX_SET_PEER_TYPE(peer, PMIX_PROC_SERVER);
        PMIX_SET_PEER_VERSION(peer, vrs, major, minor);

        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "Version %s SERVER DETECTED - BFROPS %s",
                            vrs, mod->component->base.pmix_mca_component_name);

        if (!evalgiven) {
            // return the uri
            *evar = eval;
        }
        return PMIX_SUCCESS;
    }

    return PMIX_ERR_UNREACH;
}

pmix_status_t pmix_ptl_base_setup_fork(const pmix_proc_t *proc, char ***env)
{
    pmix_status_t rc;

    PMIX_HIDE_UNUSED_PARAMS(proc);

    /* a child that does not get these still starts, and then searches
     * the default tmpdir for its server - where it may find a different
     * one - so a failure to set them has to fail the fork setup */
    rc = PMIx_Setenv("PMIX_SERVER_TMPDIR", pmix_ptl_base.session_tmpdir, true, env);
    if (PMIX_SUCCESS == rc) {
        rc = PMIx_Setenv("PMIX_SYSTEM_TMPDIR", pmix_ptl_base.system_tmpdir, true, env);
    }
    return rc;
}

pmix_status_t pmix_ptl_base_parse_uri(const char *evar, char **nspace, pmix_rank_t *rank,
                                      char **suri)
{
    char **uri;
    char *p;

    uri = PMIx_Argv_split(evar, ';');
    if (2 != PMIx_Argv_count(uri)) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        PMIx_Argv_free(uri);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* set the server nspace - the rank is appended
     * to the end with a '.' separator. NOTE: we
     * cannot search from the FRONT as that would
     * stop on any FQDN or IPv4 separations */
    if (NULL == (p = strrchr(uri[0], '.'))) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        PMIx_Argv_free(uri);
        return PMIX_ERR_NOT_SUPPORTED;
    }
    *p = '\0';
    ++p;
    *nspace = strdup(uri[0]);
    if (NULL == *nspace) {
        PMIx_Argv_free(uri);
        return PMIX_ERR_NOMEM;
    }
    /* set the server rank. This is deliberately lenient: v3.2 servers
     * print the rank with "%d", so a wildcard or invalid rank arrives as
     * a negative number, and strtoull() wraps it back onto the same
     * pmix_rank_t. A digits-only parse would refuse those servers */
    *rank = strtoull(p, NULL, 10);
    if (NULL != suri) {
        *suri = strdup(uri[1]);
        if (NULL == *suri) {
            free(*nspace);
            *nspace = NULL;
            PMIx_Argv_free(uri);
            return PMIX_ERR_NOMEM;
        }
    }

    PMIx_Argv_free(uri);
    return PMIX_SUCCESS;
}

/* Split a "major.minor.release" version string into its components.
 *
 * The strings handed to this come off the wire, so they need not carry
 * all three components - or any of them. Every component we cannot read
 * is reported as zero, and we step over a separator only after
 * confirming the string has not already ended. */
void pmix_ptl_base_parse_version(const char *vers, uint8_t *major,
                                 uint8_t *minor, uint8_t *release)
{
    char *p;

    *major = 0;
    *minor = 0;
    *release = 0;
    if (NULL == vers) {
        return;
    }
    *major = (uint8_t) strtoul(vers, &p, 10);
    if ('\0' == *p) {
        return;
    }
    *minor = (uint8_t) strtoul(&p[1], &p, 10);
    if ('\0' == *p) {
        return;
    }
    *release = (uint8_t) strtoul(&p[1], NULL, 10);
}

/* Open a connection file for reading.
 *
 * A file the caller named is opened as it always was. One we came across
 * while walking a directory is another matter: the directories searched
 * default to $TMPDIR or /tmp, which any local user can write to, and
 * fopen() on a FIFO with no writer never returns - so one "pmix.*" FIFO
 * planted there hung every tool that went looking for a server. Such a
 * file is opened non-blocking and kept only if it is a regular file.
 * Checking the open descriptor rather than the name leaves no window in
 * which the entry can be swapped for a FIFO after it was checked. */
static FILE *open_conn_file(const char *filename, bool found_by_search)
{
    int fd, flags;
    struct stat st;
    FILE *fp;

    if (!found_by_search) {
        return fopen(filename, "r");
    }
    fd = open(filename, O_RDONLY | O_NONBLOCK);
    if (0 > fd) {
        return NULL;
    }
    if (0 != fstat(fd, &st) || !S_ISREG(st.st_mode)) {
        close(fd);
        errno = ENOENT;
        return NULL;
    }
    flags = fcntl(fd, F_GETFL);
    if (0 <= flags) {
        (void) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
    fp = fdopen(fd, "r");
    if (NULL == fp) {
        close(fd);
    }
    return fp;
}

/* Decide what a directory-walk entry is. Only a real directory is
 * descended into: a symbolic link is never followed to one, because a
 * link back up the tree ("ln -s . a") makes the walk revisit everything
 * beneath it at every level, and two such links make it exponential -
 * again, in a directory anyone can write to. A link to a regular file is
 * still read. Returns false for anything that is neither. */
static bool search_candidate(const char *path, bool *isdir)
{
    struct stat st;

    *isdir = false;
    if (0 != lstat(path, &st)) {
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        *isdir = true;
        return true;
    }
    if (S_ISLNK(st.st_mode) && 0 != stat(path, &st)) {
        return false;
    }
    return S_ISREG(st.st_mode);
}

pmix_status_t pmix_ptl_base_parse_uri_file(char *filename,
                                           bool optional,
                                           pmix_list_t *connections)
{
    return parse_conn_file(filename, optional, false, connections);
}

static pmix_status_t parse_conn_file(char *filename, bool optional, bool found_by_search,
                                     pmix_list_t *connections)
{
    FILE *fp;
    char *srvr, *p = NULL;
    struct timeval tv;
    int retries;
    pmix_status_t rc;
    pmix_connection_t *cn;
    char *nspace = NULL;
    pmix_rank_t rank;
    char *uri = NULL;

    /* if we cannot open the file, then the server must not
     * be configured to support tool connections, or this
     * user isn't authorized to access it - or it may just
     * not exist yet! Check for existence */
    /* coverity[TOCTOU] */
    if (0 != access(filename, R_OK)) {
        if (ENOENT == errno && !optional && !found_by_search) {
            /* the file does not exist, so give it
             * a little time to see if the server
             * is still starting up. Not for a file a directory walk
             * just listed - that one is not late, it is gone */
            retries = 0;
            do {
                ++retries;
                pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                    "WAITING FOR CONNECTION FILE %s", filename);
                if (0 < pmix_ptl_base.wait_to_connect) {
                    tv.tv_sec = pmix_ptl_base.wait_to_connect;
                    tv.tv_usec = 0;
                } else {
                    tv.tv_sec = 0;
                    tv.tv_usec = 10000; // use 0.01 sec as default
                }
                retry_wait(&tv);
                /* coverity[TOCTOU] */
                if (0 == access(filename, R_OK)) {
                    goto process;
                }
            } while (retries < pmix_ptl_base.max_retries);
            /* otherwise, mark it as unreachable */
        }
        if (!optional) {
            if (EACCES == errno) {
                pmix_show_help("help-ptl-base.txt", "file-not-found", true,
                               filename, "access denied");
            } else {
                pmix_show_help("help-ptl-base.txt", "file-not-found", true,
                               filename, "could not be found");
            }
        }
        return PMIX_ERR_UNREACH;
    }

process:
    fp = open_conn_file(filename, found_by_search);
    if (NULL == fp) {
        if (!optional) {
            if (EACCES == errno) {
                pmix_show_help("help-ptl-base.txt", "file-not-found", true,
                               filename, "access denied");
            } else {
                pmix_show_help("help-ptl-base.txt", "file-not-found", true,
                               filename, "could not be found");
            }
        }
        return PMIX_ERR_UNREACH;
    }
    /* get the URI - might seem crazy, but there is actually
     * a race condition here where the server may have created
     * the file but not yet finished writing into it. So give
     * us a chance to get the required info */
    for (retries = 0; retries < 3; retries++) {
        srvr = pmix_getline(fp);
        if (NULL != srvr) {
            break;
        }
        fclose(fp);
        tv.tv_sec = 0;
        tv.tv_usec = 10000; // use 0.01 sec as default
        retry_wait(&tv);
        fp = open_conn_file(filename, found_by_search);
        if (NULL == fp) {
            return PMIX_ERR_UNREACH;
        }
    }
    if (NULL == srvr) {
        if (!optional) {
            pmix_show_help("help-ptl-base.txt", "file-not-found", true,
                           filename, "could not be read");
        }
        fclose(fp);
        return PMIX_ERR_UNREACH;
    }

    /* see if this file contains the server's version */
    p = pmix_getline(fp);
    fclose(fp);

    /* parse the URI */
    rc = pmix_ptl_base_parse_uri(srvr, &nspace, &rank, &uri);
    free(srvr);
    if (PMIX_SUCCESS == rc) {
        cn = PMIX_NEW(pmix_connection_t);
        if (NULL == cn) {
            free(nspace);
            free(uri);
            if (NULL != p) {
                free(p);
            }
            return PMIX_ERR_NOMEM;
        }
        cn->nspace = nspace;
        cn->rank = rank;
        cn->uri = uri;
        cn->version = p;
        pmix_list_append(connections, &cn->super);
    } else {
        if (NULL != nspace) {
            free(nspace);
        }
        if (NULL != uri) {
            free(uri);
        }
        if (NULL != p) {
            free(p);
        }
    }
    return rc;
}

pmix_status_t pmix_ptl_base_df_search(char *dirname, char *prefix, pmix_info_t info[], size_t ninfo,
                                      bool optional, pmix_list_t *connections)
{
    char *newdir;
    DIR *cur_dirp;
    struct dirent *dir_entry;
    pmix_status_t rc;
    bool isdir;

    if (NULL == (cur_dirp = opendir(dirname))) {
        return PMIX_ERR_NOT_FOUND;
    }

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "pmix:ptl: searching directory %s", dirname);

    /* search the entries for something that starts with the provided prefix */
    while (NULL != (dir_entry = readdir(cur_dirp))) {
        /* ignore the . and .. entries */
        if (0 == strcmp(dir_entry->d_name, ".") || 0 == strcmp(dir_entry->d_name, "..")) {
            continue;
        }
        newdir = pmix_os_path(false, dirname, dir_entry->d_name, NULL);
        if (NULL == newdir) {
            /* nesting has taken the assembled name past PMIX_PATH_MAX,
             * or we are out of memory - either way there is nothing to
             * hand opendir() but a NULL */
            continue;
        }
        if (!search_candidate(newdir, &isdir)) {
            free(newdir);
            continue;
        }
        /* if it is a directory, down search */
        if (isdir) {
            pmix_ptl_base_df_search(newdir, prefix, info, ninfo, optional, connections);
            free(newdir);
            continue;
        }
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "pmix:tool: checking %s vs %s", dir_entry->d_name, prefix);
        /* see if it starts with our prefix */
        if (0 == strncmp(dir_entry->d_name, prefix, strlen(prefix))) {
            /* try to read this file. One we cannot read or parse is
             * passed over rather than ending the search: a server killed
             * partway thru writing its file leaves exactly that behind,
             * and stopping at it hid any live server that readdir()
             * happened to list after it */
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "pmix:tool: reading file %s", newdir);
            rc = parse_conn_file(newdir, optional, true, connections);
            if (PMIX_ERR_NOMEM == rc) {
                free(newdir);
                closedir(cur_dirp);
                return rc;
            }
        }
        free(newdir);
    }
    closedir(cur_dirp);
    if (0 == pmix_list_get_size(connections)) {
        return PMIX_ERR_NOT_FOUND;
    }
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

pmix_status_t pmix_ptl_base_setup_connection(char *uri, struct sockaddr_storage *connection,
                                             size_t *len)
{
    char *p = NULL, *p2, *host;
    struct sockaddr_in *in;
    struct sockaddr_in6 *in6;

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "pmix:base setup connection to %s", uri);

    memset(connection, 0, sizeof(struct sockaddr_storage));
    /* every URI we accept begins with a 7-character scheme prefix
     * ("tcp4://" or "tcp6://") followed by an address - anything
     * shorter cannot be one, and indexing past it would read off the
     * end of a string that reached us from a rendezvous file or an
     * environment variable */
    if (NULL == uri || strlen(uri) <= strlen("tcp4://")) {
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
        PMIX_ERROR_LOG(PMIX_ERR_NOT_SUPPORTED);
        return PMIX_ERR_NOT_SUPPORTED;
    }
    if (NULL != p) {
        free(p);
    }

    return PMIX_SUCCESS;
}

static pmix_status_t send_connect_ack(pmix_peer_t *peer,
                                      pmix_info_t iptr[], size_t niptr)
{
    char *msg;
    size_t sdsize = 0;
    pmix_status_t rc;

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "pmix:ptl SEND CONNECT ACK");

    /* set our ID flag and compute the required handshake size */
    peer->proc_type.flag = pmix_ptl_base_set_flag(&sdsize);

    /* construct the contact message */
    rc = construct_message(peer, &msg, &sdsize, iptr, niptr);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        return rc;
    }

    /* send the entire message across */
    if (PMIX_SUCCESS != pmix_ptl_base_send_blocking(peer->sd, msg, sdsize)) {
        free(msg);
        return PMIX_ERR_UNREACH;
    }
    free(msg);
    return PMIX_SUCCESS;
}

/* we receive a connection acknowledgment from the server,
 * consisting of nothing more than a status report. If success,
 * then we initiate authentication method */
static pmix_status_t recv_connect_ack(pmix_peer_t *peer)
{
    pmix_status_t reply;
    pmix_status_t rc;
    struct timeval save;
    pmix_socklen_t sz = sizeof(save);
    bool sockopt = true;
    uint32_t u32;

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "pmix: RECV CONNECT ACK FROM SERVER");

    /* set the socket timeout so we don't hang on blocking recv */
    rc = pmix_ptl_base_set_timeout(peer, &save, &sz, &sockopt);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    /* receive the status reply */
    rc = pmix_ptl_base_recv_blocking(peer->sd, (char *) &u32, sizeof(uint32_t));
    if (PMIX_SUCCESS != rc) {
        if (sockopt) {
            /* return the socket to normal */
            if (0 != setsockopt(peer->sd, SOL_SOCKET, SO_RCVTIMEO, &save, sz)) {
                pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                    "pmix: could not reset setsockopt SO_RCVTIMEO");
            }
        }
        return rc;
    }
    reply = ntohl(u32);

    if ((PMIX_PEER_IS_CLIENT(pmix_globals.mypeer) ||
         PMIX_PEER_IS_SINGLETON(pmix_globals.mypeer)) &&
        !PMIX_PEER_IS_TOOL(pmix_globals.mypeer)) {
        rc = pmix_ptl_base_client_handshake(peer, reply);
    } else { // we are a tool
        rc = pmix_ptl_base_tool_handshake(peer, reply);
    }

    if (sockopt) {
        /* return the socket to normal */
        if (0 != setsockopt(peer->sd, SOL_SOCKET, SO_RCVTIMEO, &save, sz)) {
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "pmix: could not reset setsockopt SO_RCVTIMEO");
        }
    }

    return rc;
}

/* The oldest server we will talk to is v3.2. Its version is known by
 * now on every path that can know it - the PMIX_SERVER_URI variable and
 * PMIX_VERSION for a client, or the version line of a rendezvous file,
 * whose absence marks a v2.0 server. A server reached through a URI handed
 * to us directly has none recorded, and is not refused. Refuse before
 * connecting: an older server accepts the connection and then leaves us
 * waiting on answers it never sends. */
static pmix_status_t refuse_outdated(pmix_peer_t *peer)
{
    uint8_t major = PMIX_PEER_MAJOR_VERSION(peer);
    uint8_t minor = PMIX_PEER_MINOR_VERSION(peer);

    if (0 != major && PMIX_MAJOR_WILDCARD != major &&
        (3 > major || (3 == major && PMIX_MINOR_WILDCARD != minor && 2 > minor))) {
        pmix_show_help("help-ptl-base.txt", "unsupported-server-version", true,
                       (int) major, (int) minor);
        return PMIX_ERR_OUTDATED;
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix_ptl_base_make_connection(pmix_peer_t *peer, char *suri,
                                            pmix_info_t *iptr, size_t niptr)
{
    struct sockaddr_storage myconnection;
    pmix_status_t rc;
    size_t len;
    int retries = 0;

    rc = refuse_outdated(peer);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    /* setup the connection */
    if (PMIX_SUCCESS != (rc = pmix_ptl_base_setup_connection(suri, &myconnection, &len))) {
        return rc;
    }

retry:
    /* try to connect */
    if (PMIX_SUCCESS != (rc = pmix_ptl_base_connect(&myconnection, len, &peer->sd))) {
        /* do not error log - might just be a stale connection point */
        return rc;
    }

    /* send our identity and any authentication credentials to the server */
    if (PMIX_SUCCESS != (rc = send_connect_ack(peer, iptr, niptr))) {
        PMIX_ERROR_LOG(rc);
        CLOSE_THE_SOCKET(peer->sd);
        return rc;
    }

    /* do whatever handshake is required */
    if (PMIX_SUCCESS != (rc = recv_connect_ack(peer))) {
        CLOSE_THE_SOCKET(peer->sd);
        if (PMIX_ERR_TEMP_UNAVAILABLE == rc) {
            ++retries;
            if (retries < pmix_ptl_base.handshake_max_retries) {
                goto retry;
            }
        }
        return rc;
    }
    /* Assign the lower half of the tag space for sendrecvs */
    peer->dyn_tags_start    = PMIX_PTL_TAG_DYNAMIC;
    peer->dyn_tags_current  = PMIX_PTL_TAG_DYNAMIC;
    peer->dyn_tags_end      = PMIX_PTL_TAG_DYNAMIC + (UINT32_MAX - PMIX_PTL_TAG_DYNAMIC)/2;

    return PMIX_SUCCESS;
}

/* Record the identity of the server a peer object stands for. Both copies
 * of the name are made before either is installed, so a failure leaves
 * the peer's previous identity - if it had one - intact */
static pmix_status_t set_server_id(pmix_peer_t *peer, const char *nspace, pmix_rank_t rank)
{
    char *ns1, *ns2;

    if (NULL == peer->info) {
        peer->info = PMIX_NEW(pmix_rank_info_t);
        if (NULL == peer->info) {
            return PMIX_ERR_NOMEM;
        }
    }
    if (NULL == peer->nptr) {
        peer->nptr = PMIX_NEW(pmix_namespace_t);
        if (NULL == peer->nptr) {
            return PMIX_ERR_NOMEM;
        }
    }
    ns1 = strdup(nspace);
    ns2 = strdup(nspace);
    if (NULL == ns1 || NULL == ns2) {
        free(ns1);
        free(ns2);
        return PMIX_ERR_NOMEM;
    }
    if (NULL != peer->nptr->nspace) {
        free(peer->nptr->nspace);
    }
    peer->nptr->nspace = ns1;
    if (NULL != peer->info->pname.nspace) {
        free(peer->info->pname.nspace);
    }
    peer->info->pname.nspace = ns2;
    peer->info->pname.rank = rank;
    return PMIX_SUCCESS;
}

/* Everything that can fail is done before the process is marked
 * connected and the socket's events are armed. On failure the socket the
 * handshake just completed on is closed: nothing will ever service it */
pmix_status_t pmix_ptl_base_complete_connection(pmix_peer_t *peer, char *nspace,
                                                pmix_rank_t rank)
{
    pmix_status_t rc;

    /* setup the server info */
    rc = set_server_id(peer, nspace, rank);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        CLOSE_THE_SOCKET(peer->sd);
        return rc;
    }

    pmix_atomic_set_bool(&pmix_globals.connected);

    pmix_ptl_base_set_nonblocking(peer->sd);

    /* setup recv event */
    pmix_event_assign(&peer->recv_event, pmix_globals.evbase, peer->sd, EV_READ | EV_PERSIST,
                      pmix_ptl_base_recv_handler, peer);
    peer->recv_ev_active = true;
    PMIX_POST_OBJECT(peer);
    pmix_event_add(&peer->recv_event, 0);

    /* setup send event */
    pmix_event_assign(&peer->send_event, pmix_globals.evbase, peer->sd, EV_WRITE | EV_PERSIST,
                      pmix_ptl_base_send_handler, peer);
    peer->send_ev_active = false;
    /* A send may have been queued while this event had no base - see
     * pmix_ptl_base_send(). It could not be activated then; activate it
     * now, or it waits for a send that may never come. */
    if (NULL != peer->send_msg) {
        if (0 == pmix_event_add(&peer->send_event, 0)) {
            peer->send_ev_active = true;
        }
    }
    return PMIX_SUCCESS;
}

/****    EVENT-DRIVEN CONNECT    ****
 *
 * pmix_ptl_base_make_connection() and the handshake after it wait on the
 * server with blocking calls. That is fine for a caller whose own thread
 * it is - PMIx_Init, PMIx_tool_init - but a tool attaching to a server
 * does it from a thread-shift handler, and then the whole progress thread
 * waits: every other connection, event and callback in the process, for
 * as long as the server takes, or forever if it accepts and never answers.
 *
 * This is the same exchange driven from socket events instead. It sends
 * the identical connect-ack and reads the identical replies - nothing on
 * the wire changes - but never waits for bytes that have not arrived: each
 * reply field is collected as it comes in, the operation keeps its place,
 * and the progress thread goes back to its other work in between. One
 * timer, ptl_base_handshake_wait_time, bounds the whole connect.
 *
 * What it cannot make event-driven is a psec handshake: that interface is
 * server_handshake(int sd)/client_handshake(int sd), a blocking exchange
 * by definition, and it is run as one here, bounded by the same wait. No
 * production psec module has a handshake - only the opt-in
 * psec/dummy_handshake test module does. */

typedef enum {
    PMIX_CNCT_CONNECTING,
    PMIX_CNCT_SENDING,
    PMIX_CNCT_STATUS,
    PMIX_CNCT_MY_NSPACE,
    PMIX_CNCT_MY_RANK,
    PMIX_CNCT_SRV_NSPACE,
    PMIX_CNCT_SRV_RANK,
    PMIX_CNCT_SEC_STATUS,
    PMIX_CNCT_PINDEX
} pmix_cnct_state_t;

typedef struct {
    pmix_list_item_t super;
    pmix_peer_t *peer;          // borrowed - the caller owns it until cbfunc
    char *nspace;               // the server's identity from locating it
    pmix_rank_t rank;
    char *suri;
    pmix_info_t *iptr;          // the info blob for the connect-ack
    size_t niptr;
    struct sockaddr_storage addr;
    size_t addrlen;
    int sd;
    int connect_tries;
    int handshake_tries;
    pmix_cnct_state_t state;
    char *msg;                  // the connect-ack being sent
    size_t msglen;
    size_t sent;
    char field[PMIX_MAX_NSLEN + 1]; // the reply field being read
    size_t fieldlen;
    size_t got;
    pmix_event_t ev;
    bool ev_active;
    pmix_event_t timer;
    bool timer_active;
    pmix_ptl_connect_nb_cbfunc_t cbfunc;
    void *cbdata;
} pmix_ptl_connect_op_t;

static void cnopcon(pmix_ptl_connect_op_t *p)
{
    p->peer = NULL;
    p->nspace = NULL;
    p->rank = PMIX_RANK_UNDEF;
    p->suri = NULL;
    p->iptr = NULL;
    p->niptr = 0;
    memset(&p->addr, 0, sizeof(p->addr));
    p->addrlen = 0;
    p->sd = -1;
    p->connect_tries = 0;
    p->handshake_tries = 0;
    p->state = PMIX_CNCT_CONNECTING;
    p->msg = NULL;
    p->msglen = 0;
    p->sent = 0;
    memset(p->field, 0, sizeof(p->field));
    p->fieldlen = 0;
    p->got = 0;
    memset(&p->ev, 0, sizeof(p->ev));
    p->ev_active = false;
    memset(&p->timer, 0, sizeof(p->timer));
    p->timer_active = false;
    p->cbfunc = NULL;
    p->cbdata = NULL;
}
static void cnopdes(pmix_ptl_connect_op_t *p)
{
    if (p->ev_active) {
        pmix_event_del(&p->ev);
    }
    if (p->timer_active) {
        pmix_event_evtimer_del(&p->timer);
    }
    if (0 <= p->sd) {
        CLOSE_THE_SOCKET(p->sd);
    }
    free(p->nspace);
    free(p->suri);
    if (NULL != p->iptr) {
        PMIX_INFO_FREE(p->iptr, p->niptr);
    }
    free(p->msg);
}
static PMIX_CLASS_INSTANCE(pmix_ptl_connect_op_t,
                           pmix_list_item_t,
                           cnopcon, cnopdes);

static void cnct_begin(int sd, short args, void *cbdata);
static void cnct_connected(int sd, short args, void *cbdata);
static void cnct_send(int sd, short args, void *cbdata);
static void cnct_recv(int sd, short args, void *cbdata);

/* Report the outcome and retire the operation. Everything it holds but the
 * URI - which goes to the callback - is released here, and on failure the
 * socket with it */
static void cnct_finish(pmix_ptl_connect_op_t *op, pmix_status_t status)
{
    char *suri;

    if (op->ev_active) {
        pmix_event_del(&op->ev);
        op->ev_active = false;
    }
    if (op->timer_active) {
        pmix_event_evtimer_del(&op->timer);
        op->timer_active = false;
    }
    pmix_list_remove_item(&pmix_ptl_base.connecting, &op->super);
    if (PMIX_SUCCESS == status) {
        /* the socket is the peer's now */
        op->sd = -1;
    } else if (NULL != op->peer) {
        op->peer->sd = -1;
    }
    suri = op->suri;
    op->suri = NULL;
    op->cbfunc(status, (struct pmix_peer_t *) op->peer, suri, op->cbdata);
    PMIX_RELEASE(op);
}

static void cnct_expired(int sd, short args, void *cbdata)
{
    pmix_ptl_connect_op_t *op = (pmix_ptl_connect_op_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(op);
    op->timer_active = false;
    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "ptl:base:connect: no answer from %s within %d seconds",
                        (NULL == op->suri) ? "server" : op->suri,
                        pmix_ptl_base.handshake_wait_time);
    cnct_finish(op, PMIX_ERR_TIMEOUT);
}

/* wait for the socket to become readable or writable, then run fn */
static pmix_status_t cnct_wait(pmix_ptl_connect_op_t *op, short what,
                               event_callback_fn fn)
{
    pmix_event_assign(&op->ev, pmix_globals.evbase, op->sd, what, fn, op);
    PMIX_POST_OBJECT(op);
    if (0 != pmix_event_add(&op->ev, NULL)) {
        return PMIX_ERROR;
    }
    op->ev_active = true;
    return PMIX_SUCCESS;
}

/* expect a reply field of this many bytes next */
static pmix_status_t cnct_expect(pmix_ptl_connect_op_t *op, pmix_cnct_state_t state,
                                 size_t len)
{
    op->state = state;
    op->fieldlen = len;
    op->got = 0;
    memset(op->field, 0, sizeof(op->field));
    return cnct_wait(op, EV_READ, cnct_recv);
}

/* Start over with a fresh socket - a failed attempt to connect, or a
 * server that asked us to retry the handshake */
static void cnct_restart(pmix_ptl_connect_op_t *op)
{
    if (0 <= op->sd) {
        CLOSE_THE_SOCKET(op->sd);
        op->sd = -1;
    }
    op->peer->sd = -1;
    free(op->msg);
    op->msg = NULL;
    cnct_begin(-1, 0, op);
}

/* A psec module that authenticates by exchange rather than by credential.
 * Its interface is blocking - see the note above - so run it on a blocking
 * socket, bounded by the handshake wait, and put the socket back */
static pmix_status_t cnct_psec_handshake(pmix_ptl_connect_op_t *op)
{
    pmix_status_t rc;
    struct timeval tv;

    pmix_ptl_base_set_blocking(op->sd);
    tv.tv_sec = pmix_ptl_base.handshake_wait_time;
    tv.tv_usec = 0;
    (void) setsockopt(op->sd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    PMIX_PSEC_CLIENT_HANDSHAKE(rc, op->peer, op->sd);
    tv.tv_sec = 0;
    (void) setsockopt(op->sd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    pmix_ptl_base_set_nonblocking(op->sd);
    return rc;
}

static void cnct_begin(int sd, short args, void *cbdata)
{
    pmix_ptl_connect_op_t *op = (pmix_ptl_connect_op_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(op);
    op->ev_active = false;

    /* the same attempts pmix_ptl_base_connect makes, without waiting */
    while (PMIX_MAX_RETRIES >= op->connect_tries++) {
        op->sd = socket(op->addr.ss_family, SOCK_STREAM, 0);
        if (0 > op->sd) {
            continue;
        }
        pmix_ptl_base_set_nonblocking(op->sd);
        if (0 == connect(op->sd, (struct sockaddr *) &op->addr, op->addrlen)) {
            cnct_connected(op->sd, EV_WRITE, op);
            return;
        }
        if (EINPROGRESS == pmix_socket_errno) {
            if (PMIX_SUCCESS == cnct_wait(op, EV_WRITE, cnct_connected)) {
                return;
            }
            cnct_finish(op, PMIX_ERROR);
            return;
        }
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base:connect: connect failed: %s (%d)",
                            strerror(pmix_socket_errno), pmix_socket_errno);
        CLOSE_THE_SOCKET(op->sd);
        op->sd = -1;
    }
    cnct_finish(op, PMIX_ERR_UNREACH);
}

static void cnct_connected(int sd, short args, void *cbdata)
{
    pmix_ptl_connect_op_t *op = (pmix_ptl_connect_op_t *) cbdata;
    pmix_status_t rc;
    int err = 0;
    pmix_socklen_t errlen = sizeof(err);
    size_t sdsize = 0;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(op);
    op->ev_active = false;

    if (0 != getsockopt(op->sd, SOL_SOCKET, SO_ERROR, (char *) &err, &errlen) || 0 != err) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base:connect: connect failed: %s (%d)",
                            strerror(0 != err ? err : pmix_socket_errno),
                            0 != err ? err : pmix_socket_errno);
        CLOSE_THE_SOCKET(op->sd);
        op->sd = -1;
        cnct_begin(-1, 0, op);
        return;
    }

    /* connected - build the same connect-ack send_connect_ack sends */
    op->peer->sd = op->sd;
    op->peer->proc_type.flag = pmix_ptl_base_set_flag(&sdsize);
    rc = construct_message(op->peer, &op->msg, &sdsize, op->iptr, op->niptr);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        cnct_finish(op, rc);
        return;
    }
    op->msglen = sdsize;
    op->sent = 0;
    op->state = PMIX_CNCT_SENDING;
    cnct_send(op->sd, EV_WRITE, op);
}

static void cnct_send(int sd, short args, void *cbdata)
{
    pmix_ptl_connect_op_t *op = (pmix_ptl_connect_op_t *) cbdata;
    ssize_t n;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(op);
    op->ev_active = false;

    while (op->sent < op->msglen) {
        n = send(op->sd, op->msg + op->sent, op->msglen - op->sent, 0);
        if (0 < n) {
            op->sent += (size_t) n;
            continue;
        }
        if (0 > n && EINTR == pmix_socket_errno) {
            continue;
        }
        if (0 > n && (EAGAIN == pmix_socket_errno || EWOULDBLOCK == pmix_socket_errno)) {
            if (PMIX_SUCCESS != cnct_wait(op, EV_WRITE, cnct_send)) {
                cnct_finish(op, PMIX_ERROR);
            }
            return;
        }
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base:connect: send of connect-ack failed: %s (%d)",
                            strerror(pmix_socket_errno), pmix_socket_errno);
        cnct_finish(op, PMIX_ERR_UNREACH);
        return;
    }
    free(op->msg);
    op->msg = NULL;

    /* every connector's answer starts with a status */
    if (PMIX_SUCCESS != cnct_expect(op, PMIX_CNCT_STATUS, sizeof(uint32_t))) {
        cnct_finish(op, PMIX_ERROR);
    }
}

/* The field just read is complete - act on it and say what comes next.
 * Returns PMIX_SUCCESS when there is more to read, PMIX_OPERATION_SUCCEEDED
 * when the handshake is done, or the failure. The order below is the one
 * pmix_ptl_base_client_handshake and pmix_ptl_base_tool_handshake read in. */
static pmix_status_t cnct_field(pmix_ptl_connect_op_t *op)
{
    pmix_status_t rc, reply;
    uint32_t u32 = 0;
    bool client;

    if (sizeof(uint32_t) == op->fieldlen) {
        memcpy(&u32, op->field, sizeof(uint32_t));
        u32 = ntohl(u32);
    }
    client = (PMIX_PEER_IS_CLIENT(pmix_globals.mypeer) ||
              PMIX_PEER_IS_SINGLETON(pmix_globals.mypeer)) &&
             !PMIX_PEER_IS_TOOL(pmix_globals.mypeer);

    switch (op->state) {
    case PMIX_CNCT_STATUS:
        reply = (pmix_status_t) u32;
        if (PMIX_ERR_TEMP_UNAVAILABLE == reply &&
            ++op->handshake_tries < pmix_ptl_base.handshake_max_retries) {
            /* the server asked us to try again - make_connection does so
             * from the connect on, and so do we */
            op->connect_tries = 0;
            cnct_restart(op);
            return PMIX_OPERATION_IN_PROGRESS;
        }
        if (client) {
            if (PMIX_ERR_READY_FOR_HANDSHAKE == reply) {
                rc = cnct_psec_handshake(op);
                if (PMIX_SUCCESS != rc) {
                    return rc;
                }
            } else if (PMIX_SUCCESS != reply) {
                return reply;
            }
            return cnct_expect(op, PMIX_CNCT_PINDEX, sizeof(uint32_t));
        }
        if (PMIX_SUCCESS != reply) {
            return reply;
        }
        if (PMIX_TOOL_NEEDS_ID == op->peer->proc_type.flag ||
            PMIX_LAUNCHER_NEEDS_ID == op->peer->proc_type.flag) {
            return cnct_expect(op, PMIX_CNCT_MY_NSPACE, PMIX_MAX_NSLEN + 1);
        }
        return cnct_expect(op, PMIX_CNCT_SRV_NSPACE, PMIX_MAX_NSLEN + 1);

    case PMIX_CNCT_MY_NSPACE:
        op->field[PMIX_MAX_NSLEN] = '\0';
        PMIX_LOAD_NSPACE(pmix_globals.myid.nspace, op->field);
        return cnct_expect(op, PMIX_CNCT_MY_RANK, sizeof(uint32_t));

    case PMIX_CNCT_MY_RANK:
        pmix_globals.myid.rank = u32;
        return cnct_expect(op, PMIX_CNCT_SRV_NSPACE, PMIX_MAX_NSLEN + 1);

    case PMIX_CNCT_SRV_NSPACE:
        op->field[PMIX_MAX_NSLEN] = '\0';
        free(op->nspace);
        op->nspace = strdup(op->field);
        if (NULL == op->nspace) {
            return PMIX_ERR_NOMEM;
        }
        return cnct_expect(op, PMIX_CNCT_SRV_RANK, sizeof(uint32_t));

    case PMIX_CNCT_SRV_RANK:
        op->rank = u32;
        rc = set_server_id(op->peer, op->nspace, op->rank);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "pmix: RECV CONNECT CONFIRMATION FOR TOOL %s:%d FROM SERVER %s:%d",
                            pmix_globals.myid.nspace, pmix_globals.myid.rank,
                            op->peer->info->pname.nspace, op->peer->info->pname.rank);
        return cnct_expect(op, PMIX_CNCT_SEC_STATUS, sizeof(uint32_t));

    case PMIX_CNCT_SEC_STATUS:
        reply = (pmix_status_t) u32;
        if (PMIX_ERR_READY_FOR_HANDSHAKE == reply) {
            rc = cnct_psec_handshake(op);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
        } else if (PMIX_SUCCESS != reply) {
            return reply;
        }
        return PMIX_OPERATION_SUCCEEDED;

    case PMIX_CNCT_PINDEX:
        pmix_globals.pindex = u32;
        return PMIX_OPERATION_SUCCEEDED;

    default:
        return PMIX_ERR_BAD_PARAM;
    }
}

static void cnct_recv(int sd, short args, void *cbdata)
{
    pmix_ptl_connect_op_t *op = (pmix_ptl_connect_op_t *) cbdata;
    pmix_status_t rc;
    ssize_t n;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_ACQUIRE_OBJECT(op);
    op->ev_active = false;

    while (op->got < op->fieldlen) {
        n = recv(op->sd, op->field + op->got, op->fieldlen - op->got, 0);
        if (0 < n) {
            op->got += (size_t) n;
            continue;
        }
        if (0 == n) {
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "ptl:base:connect: server closed the connection");
            cnct_finish(op, PMIX_ERR_UNREACH);
            return;
        }
        if (EINTR == pmix_socket_errno) {
            continue;
        }
        if (EAGAIN == pmix_socket_errno || EWOULDBLOCK == pmix_socket_errno) {
            if (PMIX_SUCCESS != cnct_wait(op, EV_READ, cnct_recv)) {
                cnct_finish(op, PMIX_ERROR);
            }
            return;
        }
        cnct_finish(op, PMIX_ERR_UNREACH);
        return;
    }

    rc = cnct_field(op);
    if (PMIX_SUCCESS == rc || PMIX_OPERATION_IN_PROGRESS == rc) {
        /* waiting on the next field, or started over */
        return;
    }
    if (PMIX_OPERATION_SUCCEEDED != rc) {
        cnct_finish(op, rc);
        return;
    }

    /* the handshake is complete - finish exactly as make_connection and
     * connect_to_peer do */
    op->peer->dyn_tags_start = PMIX_PTL_TAG_DYNAMIC;
    op->peer->dyn_tags_current = PMIX_PTL_TAG_DYNAMIC;
    op->peer->dyn_tags_end = PMIX_PTL_TAG_DYNAMIC + (UINT32_MAX - PMIX_PTL_TAG_DYNAMIC) / 2;
    rc = pmix_ptl_base_complete_connection(op->peer, op->nspace, op->rank);
    if (PMIX_SUCCESS != rc) {
        /* complete_connection has already closed the socket */
        op->sd = -1;
    }
    cnct_finish(op, rc);
}

pmix_status_t pmix_ptl_base_start_connection(pmix_peer_t *peer, char *nspace,
                                             pmix_rank_t rank, char *suri,
                                             pmix_info_t *iptr, size_t niptr,
                                             pmix_ptl_connect_nb_cbfunc_t cbfunc,
                                             void *cbdata)
{
    pmix_ptl_connect_op_t *op;
    pmix_status_t rc;
    struct timeval tv = {0, 0};

    rc = refuse_outdated(peer);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    op = PMIX_NEW(pmix_ptl_connect_op_t);
    if (NULL == op) {
        return PMIX_ERR_NOMEM;
    }
    rc = pmix_ptl_base_setup_connection(suri, &op->addr, &op->addrlen);
    if (PMIX_SUCCESS != rc) {
        PMIX_RELEASE(op);
        return rc;
    }
    op->peer = peer;
    op->cbfunc = cbfunc;
    op->cbdata = cbdata;
    pmix_list_append(&pmix_ptl_base.connecting, &op->super);

    if (0 < pmix_ptl_base.handshake_wait_time) {
        tv.tv_sec = pmix_ptl_base.handshake_wait_time;
        pmix_event_evtimer_set(pmix_globals.evbase, &op->timer, cnct_expired, op);
        if (0 == pmix_event_evtimer_add(&op->timer, &tv)) {
            op->timer_active = true;
        }
    }

    /* Begin on the next pass of the event loop rather than here: the
     * contract is that the callback never runs inside this call, and a
     * connect() refused outright would otherwise report it at once */
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    pmix_event_evtimer_set(pmix_globals.evbase, &op->ev, cnct_begin, op);
    if (0 != pmix_event_evtimer_add(&op->ev, &tv)) {
        pmix_list_remove_item(&pmix_ptl_base.connecting, &op->super);
        PMIX_RELEASE(op);
        return PMIX_ERROR;
    }
    op->ev_active = true;

    /* taken last, so a failure above leaves them with the caller */
    op->nspace = nspace;
    op->rank = rank;
    op->suri = suri;
    op->iptr = iptr;
    op->niptr = niptr;
    return PMIX_SUCCESS;
}

void pmix_ptl_base_abandon_connects(void)
{
    pmix_ptl_connect_op_t *op, *next;

    PMIX_LIST_FOREACH_SAFE (op, next, &pmix_ptl_base.connecting, pmix_ptl_connect_op_t) {
        cnct_finish(op, PMIX_ERR_NOT_AVAILABLE);
    }
}

pmix_rnd_flag_t pmix_ptl_base_set_flag(size_t *sz)
{
    pmix_rnd_flag_t flag;
    size_t sdsize = 0;

    /* Defined marker values:
     *
     */
    if (PMIX_PEER_IS_LAUNCHER(pmix_globals.mypeer)) {
        if (PMIX_PEER_IS_CLIENT(pmix_globals.mypeer)) {
            /* if we are both launcher and client, then we need
             * to tell the server we are both */
            flag = PMIX_LAUNCHER_CLIENT;
            /* add space for our uid/gid for ACL purposes */
            sdsize += 2 * sizeof(uint32_t);
            /* add space for our identifier */
            sdsize += strlen(pmix_globals.myid.nspace) + 1 + sizeof(uint32_t);
        } else {
            /* add space for our uid/gid for ACL purposes */
            sdsize += 2 * sizeof(uint32_t);
            /* if they gave us an identifier, we need to pass it */
            if (0 < strlen(pmix_globals.myid.nspace)
                && PMIX_RANK_INVALID != pmix_globals.myid.rank) {
                flag = PMIX_LAUNCHER_GIVEN_ID;
                sdsize += strlen(pmix_globals.myid.nspace) + 1 + sizeof(uint32_t);
            } else {
                flag = PMIX_LAUNCHER_NEEDS_ID;
            }
        }

    } else if (PMIX_PEER_IS_SCHEDULER(pmix_globals.mypeer)) {
        /* add space for our uid/gid for ACL purposes */
        sdsize += 2 * sizeof(uint32_t);
        flag = PMIX_SCHEDULER_WITH_ID;
        sdsize += strlen(pmix_globals.myid.nspace) + 1 + sizeof(uint32_t);

    } else if (PMIX_PEER_IS_CLIENT(pmix_globals.mypeer)
               && !PMIX_PEER_IS_TOOL(pmix_globals.mypeer)) {
        if (PMIX_PEER_IS_SINGLETON(pmix_globals.mypeer)) {
            flag = PMIX_SINGLETON_CLIENT;
            /* reserve space for our nspace and rank info */
            sdsize += strlen(pmix_globals.myid.nspace) + 1 + sizeof(uint32_t);
            /* add space for our uid/gid for ACL purposes */
            sdsize += 2 * sizeof(uint32_t);
        } else {
            /* we are a simple client */
            flag = PMIX_SIMPLE_CLIENT;
            /* reserve space for our nspace and rank info */
            sdsize += strlen(pmix_globals.myid.nspace) + 1 + sizeof(uint32_t);
        }
    } else { // must be a tool of some sort
        /* add space for our uid/gid for ACL purposes */
        sdsize += 2 * sizeof(uint32_t);
        if (PMIX_PEER_IS_CLIENT(pmix_globals.mypeer)) {
            /* if we are both tool and client, then we need
             * to tell the server we are both */
            flag = PMIX_TOOL_CLIENT;
            /* add space for our identifier */
            sdsize += strlen(pmix_globals.myid.nspace) + 1 + sizeof(uint32_t);
        } else if (0 < strlen(pmix_globals.myid.nspace)
                   && PMIX_RANK_INVALID != pmix_globals.myid.rank) {
            /* we were given an identifier by the caller, pass it */
            sdsize += strlen(pmix_globals.myid.nspace) + 1 + sizeof(uint32_t);
            flag = PMIX_TOOL_GIVEN_ID;
        } else {
            /* we are a self-started tool that needs an identifier */
            flag = PMIX_TOOL_NEEDS_ID;
        }
    }

    *sz += sdsize;
    return flag;
}

static pmix_status_t construct_message(pmix_peer_t *peer, char **msgout, size_t *sz,
                                       pmix_info_t *iptr, size_t niptr)
{
    char *msg;
    char *sec, *bfrops, *gds;
    pmix_bfrop_buffer_type_t bftype;
    uid_t euid;
    gid_t egid;
    pmix_buffer_t buf;
    pmix_status_t rc;
    pmix_ptl_hdr_t hdr;
    size_t sdsize, csize;
    pmix_byte_object_t cred;

    sdsize = *sz;

    /* setup the header */
    memset(&hdr, 0, sizeof(pmix_ptl_hdr_t));
    hdr.pindex = -1;
    hdr.tag = UINT32_MAX;

    /* add the name of our active sec module - we selected it
     * in pmix_client.c prior to entering here */
    sec = pmix_globals.mypeer->nptr->compat.psec->name;
    sdsize += strlen(sec) + 1;

    /* a security module was assigned to us during rte_init based
     * on a list of available security modules provided by our
     * local PMIx server, if known. Now use that module to
     * get a credential, if the security system provides one. Not
     * every psec module will do so, thus we must first check */
    PMIX_BYTE_OBJECT_CONSTRUCT(&cred);
    PMIX_PSEC_CREATE_CRED(rc, pmix_globals.mypeer, NULL, 0, NULL, 0, &cred);
    if (PMIX_SUCCESS != rc) {
        PMIX_BYTE_OBJECT_DESTRUCT(&cred);
        return rc;
    }
    sdsize += sizeof(uint32_t); // need to pass the number of bytes
    sdsize += cred.size;        // account for the payload itself

    /* add our type flag */
    sdsize += 1;

    /* add our version string */
    sdsize += strlen(PMIX_VERSION) + 1;

    /* add our active bfrops module name */
    bfrops = pmix_globals.mypeer->nptr->compat.bfrops->version;
    sdsize += strlen(bfrops) + 1;
    /* and the type of buffer we are using */
    bftype = pmix_globals.mypeer->nptr->compat.type;
    sdsize += sizeof(bftype);

    /* add our active gds module for working with the server */
    gds = (char *) peer->nptr->compat.gds->name;
    sdsize += strlen(gds) + 1;

    /* if we were given info structs to pass to the server, pack them */
    if (NULL != iptr) {
        PMIX_CONSTRUCT(&buf, pmix_buffer_t);
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, &niptr, 1, PMIX_SIZE);
        if (PMIX_SUCCESS == rc) {
            PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &buf, iptr, niptr, PMIX_INFO);
        }
        if (PMIX_SUCCESS != rc) {
            /* we cannot send a partial blob - the far end computes the
             * length of the info section from what is left in the
             * message, so a short one would be read as garbage */
            PMIX_ERROR_LOG(rc);
            PMIX_BYTE_OBJECT_DESTRUCT(&cred);
            PMIX_DESTRUCT(&buf);
            return rc;
        }
        sdsize += buf.bytes_used;
    }

    /* set the number of bytes to be read beyond the header */
    hdr.nbytes = sdsize;

    /* create a space for our message */
    sdsize = sizeof(hdr) + hdr.nbytes;
    if (NULL == (msg = (char *) malloc(sdsize))) {
        /* "sec", "bfrops" and "gds" all point into the compat modules
         * we were assigned - they are not ours to free */
        PMIX_BYTE_OBJECT_DESTRUCT(&cred);
        if (NULL != iptr) {
            PMIX_DESTRUCT(&buf);
        }
        return PMIX_ERR_OUT_OF_RESOURCE;
    }
    memset(msg, 0, sdsize);

    /* load the header */
    csize = 0;
    memcpy(msg, &hdr, sizeof(pmix_ptl_hdr_t));
    csize += sizeof(pmix_ptl_hdr_t);

    /* provide our active psec module */
    PMIX_PTL_PUT_STRING(sec);

    /* load the length of the credential */
    PMIX_PTL_PUT_U32(cred.size);

    /* load the credential */
    PMIX_PTL_PUT_BLOB(cred.bytes, cred.size);
    PMIX_BYTE_OBJECT_DESTRUCT(&cred);

    /* load our process type - this is a single byte,
     * so no worry about heterogeneity here */
    PMIX_PTL_PUT_U8(peer->proc_type.flag);

    switch (peer->proc_type.flag) {
    case PMIX_SIMPLE_CLIENT:
        /* simple client process */
        PMIX_PTL_PUT_PROCID(pmix_globals.myid);
        break;

        /* we cannot have cases 1 or 2 because those are only
         * for legacy processes */

    case PMIX_TOOL_NEEDS_ID:
    case PMIX_LAUNCHER_NEEDS_ID:
        /* self-started tool/launcher process that needs an identifier */
        euid = geteuid();
        PMIX_PTL_PUT_U32(euid);
        egid = getegid();
        PMIX_PTL_PUT_U32(egid);
        break;

    case PMIX_TOOL_GIVEN_ID:
    case PMIX_LAUNCHER_GIVEN_ID:
    case PMIX_SCHEDULER_WITH_ID:
    case PMIX_SINGLETON_CLIENT:
        /* self-started tool/launcher/singleton process that was given an identifier by caller */
        euid = geteuid();
        PMIX_PTL_PUT_U32(euid);
        egid = getegid();
        PMIX_PTL_PUT_U32(egid);
        /* add our identifier */
        PMIX_PTL_PUT_PROCID(pmix_globals.myid);
        break;

    case PMIX_TOOL_CLIENT:
    case PMIX_LAUNCHER_CLIENT:
        /* tool/launcher that was started by a PMIx server - identifier specified by server */
        euid = geteuid();
        PMIX_PTL_PUT_U32(euid);
        egid = getegid();
        PMIX_PTL_PUT_U32(egid);
        /* add our identifier */
        PMIX_PTL_PUT_PROCID(pmix_globals.myid);
        break;

    default:
        /* we don't know what they are! */
        if (NULL != iptr) {
            PMIX_DESTRUCT(&buf);
        }
        free(msg);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* provide our version */
    PMIX_PTL_PUT_STRING(PMIX_VERSION);

    /* provide our active bfrops module */
    PMIX_PTL_PUT_STRING(bfrops);

    /* provide the bfrops type */
    PMIX_PTL_PUT_U8(bftype);

    /* provide the gds module */
    PMIX_PTL_PUT_STRING(gds);

    /* provide the info struct bytes */
    if (NULL != iptr) {
        PMIX_PTL_PUT_BLOB(buf.base_ptr, buf.bytes_used);
        PMIX_DESTRUCT(&buf);
    }

    *msgout = msg;
    *sz = sdsize;
    return PMIX_SUCCESS;
}

pmix_status_t pmix_ptl_base_set_timeout(pmix_peer_t *peer, struct timeval *save,
                                        pmix_socklen_t *sz, bool *sockopt)
{
    struct timeval tv;

    /* get the current timeout value so we can reset to it */
    if (0 != getsockopt(peer->sd, SOL_SOCKET, SO_RCVTIMEO, (void *) save, sz)) {
        *sockopt = false;
    } else {
        /* set a timeout on the blocking recv so we don't hang */
        tv.tv_sec = pmix_ptl_base.handshake_wait_time;
        tv.tv_usec = 0;
        if (0 != setsockopt(peer->sd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv))) {
            *sockopt = false;
        }
    }

    return PMIX_SUCCESS;
}

pmix_status_t pmix_ptl_base_client_handshake(pmix_peer_t *peer, pmix_status_t reply)
{
    pmix_status_t rc;

    /* see if they want us to do the handshake */
    if (PMIX_ERR_READY_FOR_HANDSHAKE == reply) {
        PMIX_PSEC_CLIENT_HANDSHAKE(rc, peer, peer->sd);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    } else if (PMIX_SUCCESS != reply) {
        return reply;
    }
    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "pmix: RECV CONNECT CONFIRMATION");

    /* receive our index into the peer's client array */
    PMIX_PTL_RECV_U32(peer->sd, pmix_globals.pindex);
    return PMIX_SUCCESS;
}

/* the "peer" object passed into this function is that of the SERVER
 * to which the tool is connecting - it is NOT the peer of the tool itself*/
pmix_status_t pmix_ptl_base_tool_handshake(pmix_peer_t *peer, pmix_status_t rp)
{
    pmix_nspace_t nspace;
    pmix_rank_t rank;
    pmix_status_t reply, rc;

    /* if the status indicates an error, then we are done */
    if (PMIX_SUCCESS != rp) {
        return rp;
    }

    /* if we need an identifier, it comes next */
    if (PMIX_TOOL_NEEDS_ID == peer->proc_type.flag ||
        PMIX_LAUNCHER_NEEDS_ID == peer->proc_type.flag) {
        PMIX_PTL_RECV_NSPACE(peer->sd, pmix_globals.myid.nspace);
        PMIX_PTL_RECV_U32(peer->sd, pmix_globals.myid.rank);
    }

    /* get the server's nspace and rank so we can send to it */
    PMIX_PTL_RECV_NSPACE(peer->sd, nspace);
    PMIX_PTL_RECV_U32(peer->sd, rank);
    rc = set_server_id(peer, nspace, rank);
    if (PMIX_SUCCESS != rc) {
        return rc;
    }

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "pmix: RECV CONNECT CONFIRMATION FOR TOOL %s:%d FROM SERVER %s:%d",
                        pmix_globals.myid.nspace, pmix_globals.myid.rank, peer->info->pname.nspace,
                        peer->info->pname.rank);

    /* get the returned status from the security handshake */
    PMIX_PTL_RECV_U32(peer->sd, reply);
    if (PMIX_SUCCESS != reply) {
        /* see if they want us to do the handshake */
        if (PMIX_ERR_READY_FOR_HANDSHAKE == reply) {
            PMIX_PSEC_CLIENT_HANDSHAKE(reply, peer, peer->sd);
            if (PMIX_SUCCESS != reply) {
                return reply;
            }
            /* if the handshake succeeded, then fall thru to the next step */
        } else {
            return reply;
        }
    }

    return PMIX_SUCCESS;
}

/* Append one attribute to a list under construction. Returns false if it
 * could not be allocated or loaded - nothing is appended in that case */
static bool add_info(pmix_list_t *list, const char *key, const void *val,
                     pmix_data_type_t type)
{
    pmix_infolist_t *ip;

    ip = PMIX_NEW(pmix_infolist_t);
    if (NULL == ip) {
        return false;
    }
    if (PMIX_SUCCESS != PMIx_Info_load(&ip->info, key, val, type)) {
        PMIX_RELEASE(ip);
        return false;
    }
    pmix_list_append(list, &ip->super);
    return true;
}

static void check_server(char *filename, pmix_list_t *servers)
{
    FILE *fp;
    char *srvr, *p, *p2;
    struct timeval tv;
    int retries;
    pmix_info_t *sdata;
    size_t ndata, n;
    pmix_infolist_t *iptr, *ians;
    char *nspace = NULL;
    pmix_rank_t rank;
    pmix_list_t mylist;
    uint32_t u32;
    pmix_status_t rc;
    bool ok;

    /* this file was just listed by the directory walk, so it cannot be
     * a server that is "still starting up" - if it is gone or unreadable
     * now, its server went away in between or it is not ours to read.
     * Waiting for it here would only stall the progress thread, which is
     * where a PMIX_QUERY_AVAIL_SERVERS walk runs */
    /* coverity[TOCTOU] */
    if (0 != access(filename, R_OK)) {
        return;
    }

    fp = open_conn_file(filename, true);
    if (NULL == fp) {
        return;
    }
    /* get the URI - might seem crazy, but there is actually
     * a race condition here where the server may have created
     * the file but not yet finished writing into it. So give
     * us a chance to get the required info */
    for (retries = 0; retries < 3; retries++) {
        srvr = pmix_getline(fp);
        if (NULL != srvr) {
            break;
        }
        fclose(fp);
        tv.tv_sec = 0;
        tv.tv_usec = 10000; // use 0.01 sec as default
        retry_wait(&tv);
        fp = open_conn_file(filename, true);
        if (NULL == fp) {
            return;
        }
    }
    /* an empty or malformed "pmix.*" file is to be expected in a
     * directory anyone can write to - not an error worth reporting */
    if (NULL == srvr) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "pmix:tcp: %s is empty", filename);
        fclose(fp);
        return;
    }
    rc = pmix_ptl_base_parse_uri(srvr, &nspace, &rank, NULL);
    if (PMIX_SUCCESS != rc) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "pmix:tcp: %s holds no server URI", filename);
        fclose(fp);
        free(srvr);
        return;
    }

    /* see if we already have this server in our list */
    PMIX_LIST_FOREACH (iptr, servers, pmix_infolist_t) {
        /* each item contains an array starting with the server nspace */
        sdata = (pmix_info_t *) iptr->info.value.data.darray->array;
        if (0 == strcmp(sdata[0].value.data.string, nspace) && sdata[1].value.data.rank == rank) {
            /* already have this one */
            fclose(fp);
            free(srvr);
            free(nspace);
            return;
        }
    }

    /* begin collecting data for the new entry */
    PMIX_CONSTRUCT(&mylist, pmix_list_t);
    ok = add_info(&mylist, PMIX_SERVER_NSPACE, nspace, PMIX_STRING) &&
         add_info(&mylist, PMIX_SERVER_RANK, &rank, PMIX_PROC_RANK);
    free(srvr);
    free(nspace);
    if (!ok) {
        goto nomem;
    }

    /* see if this file contains the server's version - every server
     * since v2.1 writes one, so a file without it came from a v2.0
     * server, which the connection will then refuse */
    p2 = pmix_getline(fp);
    if (NULL == p2) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output, "V20 SERVER DETECTED");
        ok = add_info(&mylist, PMIX_VERSION_INFO, "v2.0", PMIX_STRING);
    } else {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "VERSION %s SERVER DETECTED", p2);
        ok = add_info(&mylist, PMIX_VERSION_INFO, p2, PMIX_STRING);
        free(p2);
    }
    if (!ok) {
        goto nomem;
    }

    /* see if the file contains the pid */
    p2 = pmix_getline(fp);
    if (NULL == p2) {
        goto complete;
    }
    u32 = strtoul(p2, NULL, 10);
    free(p2);
    if (!add_info(&mylist, PMIX_SERVER_PIDINFO, &u32, PMIX_UINT32)) {
        goto nomem;
    }

    /* check for uid:gid */
    p2 = pmix_getline(fp);
    if (NULL == p2) {
        goto complete;
    }
    /* find the colon */
    if (NULL == (p = strchr(p2, ':'))) {
        /* bad format */
        free(p2);
        goto complete;
    }
    *p = '\0';
    ++p;
    u32 = strtoul(p2, NULL, 10);
    ok = add_info(&mylist, PMIX_USERID, &u32, PMIX_UINT32);
    if (ok) {
        u32 = strtoul(p, NULL, 10);
        ok = add_info(&mylist, PMIX_GRPID, &u32, PMIX_UINT32);
    }
    free(p2);
    if (!ok) {
        goto nomem;
    }

    /* check for timestamp */
    p2 = pmix_getline(fp);
    if (NULL == p2) {
        goto complete;
    }
    ok = add_info(&mylist, PMIX_SERVER_START_TIME, p2, PMIX_STRING);
    free(p2);
    if (!ok) {
        goto nomem;
    }

complete:
    fclose(fp);

    /* convert the list to an array */
    ndata = pmix_list_get_size(&mylist);
    ians = PMIX_NEW(pmix_infolist_t);
    if (NULL == ians) {
        PMIX_LIST_DESTRUCT(&mylist);
        return;
    }
    PMIX_LOAD_KEY(ians->info.key, PMIX_SERVER_INFO_ARRAY);
    ians->info.value.type = PMIX_DATA_ARRAY;
    PMIX_DATA_ARRAY_CREATE(ians->info.value.data.darray, ndata, PMIX_INFO);
    if (NULL == ians->info.value.data.darray) {
        ians->info.value.type = PMIX_UNDEF;
        PMIX_RELEASE(ians);
        PMIX_LIST_DESTRUCT(&mylist);
        return;
    }
    sdata = (pmix_info_t *) ians->info.value.data.darray->array;
    n = 0;
    PMIX_LIST_FOREACH (iptr, &mylist, pmix_infolist_t) {
        PMIX_INFO_XFER(&sdata[n], &iptr->info);
        ++n;
    }
    PMIX_LIST_DESTRUCT(&mylist);
    pmix_list_append(servers, &ians->super);
    return;

nomem:
    /* this server is left out of the answer - the rest of the directory
     * walk still reports whatever else it finds */
    PMIX_ERROR_LOG(PMIX_ERR_NOMEM);
    fclose(fp);
    PMIX_LIST_DESTRUCT(&mylist);
}

static void query_servers(char *dirname, pmix_list_t *servers)
{
    char *newdir, *dname;
    DIR *cur_dirp;
    struct dirent *dir_entry;
    bool isdir;

    /* search the system tmpdir directory tree for files
     * beginning with "pmix." as these can be potential
     * servers */

    if (NULL == dirname) {
        /* we first check the system tmpdir to see if a system-level
         * server is present */
        dname = pmix_ptl_base.system_tmpdir;
    } else {
        dname = dirname;
    }
    cur_dirp = opendir(dname);
    if (NULL == cur_dirp) {
        return;
    }

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "pmix:tcp: searching directory %s",
                        (NULL == dirname) ? pmix_ptl_base.system_tmpdir : dirname);

    /* search the entries for something that starts with the "pmix." prefix */
    while (NULL != (dir_entry = readdir(cur_dirp))) {
        /* ignore the . and .. entries */
        if (0 == strcmp(dir_entry->d_name, ".") || 0 == strcmp(dir_entry->d_name, "..")) {
            continue;
        }
        newdir = pmix_os_path(false, dname, dir_entry->d_name, NULL);
        if (NULL == newdir) {
            /* as in pmix_ptl_base_df_search() above */
            continue;
        }
        /* see search_candidate() for why links to directories are
         * not followed and only regular files are read */
        if (!search_candidate(newdir, &isdir)) {
            free(newdir);
            continue;
        }
        /* if it is a directory, down search */
        if (isdir) {
            query_servers(newdir, servers);
            free(newdir);
            continue;
        }
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "pmix:tcp: checking %s",
                            dir_entry->d_name);
        /* see if it starts with our prefix */
        if (0 == strncmp(dir_entry->d_name, "pmix.", strlen("pmix."))) {
            /* try to read this file */
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "pmix:tcp: reading file %s", newdir);
            check_server(newdir, servers);
        }
        free(newdir);
    }
    closedir(cur_dirp);
}

static void _local_relcb(void *cbdata)
{
    pmix_query_caddy_t *cd = (pmix_query_caddy_t *) cbdata;

    if (NULL != cd->info) {
        PMIX_INFO_FREE(cd->info, cd->ninfo);
    }
    PMIX_RELEASE(cd);
}

void pmix_ptl_base_query_servers(int sd, short args, void *cbdata)
{
    pmix_query_caddy_t *cd = (pmix_query_caddy_t *) cbdata;
    pmix_list_t servers;
    size_t n;
    pmix_infolist_t *iptr;
    pmix_status_t rc;

    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_CONSTRUCT(&servers, pmix_list_t);

    query_servers(NULL, &servers);

    /* convert the list to an array of pmix_info_t */
    cd->ninfo = pmix_list_get_size(&servers);
    if (0 == cd->ninfo) {
        rc = PMIX_ERR_NOT_FOUND;
    } else {
        PMIX_INFO_CREATE(cd->info, cd->ninfo);
        if (NULL == cd->info) {
            cd->ninfo = 0;
            rc = PMIX_ERR_NOMEM;
        } else {
            n = 0;
            PMIX_LIST_FOREACH (iptr, &servers, pmix_infolist_t) {
                PMIX_INFO_XFER(&cd->info[n], &iptr->info);
                ++n;
            }
            rc = PMIX_SUCCESS;
        }
    }
    PMIX_LIST_DESTRUCT(&servers);

    /* execute the callback function. PMIx_Query_info_nb accepts a NULL
     * callback - every other completion path in pmix_query.c checks for
     * one - so there may be nobody to hand the answer to, in which case
     * we still have to release what we built */
    if (NULL != cd->cbfunc) {
        cd->cbfunc(rc, cd->info, cd->ninfo, cd->cbdata, _local_relcb, cd);
    } else {
        _local_relcb(cd);
    }
}

static void timeout(int sd, short args, void *cbdata)
{
    pmix_lock_t *lock = (pmix_lock_t *) cbdata;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    PMIX_WAKEUP_THREAD(lock);
}

/* Pause before looking at a connection file again.
 *
 * The pause is normally an evtimer on pmix_globals.evbase with the caller
 * parked on a lock the timer releases, so the waiting thread does not
 * spin. That only works when the caller is some OTHER thread: the timer
 * can fire only when the progress thread gets back to its event loop, and
 * if the caller IS the progress thread, it never will - the wait is
 * forever. These file loops are reached on the progress thread whenever
 * pmix_ptl_base_connect_to_peer is, which includes every
 * PMIx_tool_attach_to_server (pmix_tool_retry_attach is a thread-shift
 * handler): an attachment file that did not exist yet deadlocked the
 * tool's progress thread, and with it the call waiting on the other side.
 *
 * On the progress thread, sleep for the interval instead. That blocks the
 * loop for the length of the pause - but that path already blocks it for
 * the whole TCP handshake that follows, and a bounded pause the caller
 * asked for is what the loop is there to provide. Off the progress thread
 * the timer is used as before, unless it cannot be added - then nothing
 * would ever release the lock, so that case sleeps too. */
static void retry_wait(const struct timeval *tv)
{
    pmix_lock_t lock;
    pmix_event_t ev;
    struct timeval tvc = *tv;
    struct timespec req, rem;

    if (!pmix_progress_thread_is_current()) {
        PMIX_CONSTRUCT_LOCK(&lock);
        pmix_event_evtimer_set(pmix_globals.evbase, &ev, timeout, &lock);
        PMIX_POST_OBJECT(&ev);
        if (0 == pmix_event_evtimer_add(&ev, &tvc)) {
            PMIX_WAIT_THREAD(&lock);
            PMIX_DESTRUCT_LOCK(&lock);
            return;
        }
        /* no timer will ever release the lock - sleep instead */
        PMIX_DESTRUCT_LOCK(&lock);
    }

    req.tv_sec = tv->tv_sec;
    req.tv_nsec = (long) tv->tv_usec * 1000L;
    while (0 != nanosleep(&req, &rem) && EINTR == errno) {
        req = rem;
    }
}

/*
 * Go through a list of argv; if there are any subnet specifications
 * (a.b.c.d/e), resolve them to an interface name (Currently only
 * supporting IPv4).  If unresolvable, warn and remove.
 */
char **pmix_ptl_base_split_and_resolve(const char *orig_str,
                                       const char *name)
{
    int i, ret, if_index;
    char **argv, **interfaces, *str;
    char if_name[PMIX_IF_NAMESIZE];
    struct sockaddr_storage argv_inaddr, if_inaddr;
    uint32_t argv_prefix;
    bool found;

    /* Sanity check */
    if (NULL == orig_str) {
        return NULL;
    }

    /* "" and "," split to nothing at all - a NULL, not an empty array */
    argv = PMIx_Argv_split(orig_str, ',');
    if (NULL == argv) {
        return NULL;
    }
    interfaces = NULL;
    for (i = 0; NULL != argv[i]; ++i) {
        if (isalpha((unsigned char) argv[i][0])) {
            /* This is an interface name. If not already in the interfaces array, add it */
            PMIx_Argv_append_unique_nosize(&interfaces, argv[i]);
            pmix_output_verbose(20,
                                pmix_ptl_base_framework.framework_output,
                                "ptl:tool: Using interface: %s ", argv[i]);
            continue;
        }

        /* Found a subnet notation.  Convert it to an IP
           address/netmask.  Get the prefix first. */
        argv_prefix = 0;
        str = strchr(argv[i], '/');
        if (NULL == str) {
            pmix_show_help("help-ptl-base.txt", "invalid if_inexclude", true,
                           name, pmix_globals.hostname, argv[i],
                           "Invalid specification (missing \"/\")");
            continue;
        }
        *str = '\0';
        argv_prefix = atoi(str + 1);

        /* Now convert the IPv4 address */
        ((struct sockaddr *) &argv_inaddr)->sa_family = AF_INET;
        ret = inet_pton(AF_INET, argv[i], &((struct sockaddr_in *) &argv_inaddr)->sin_addr);
        *str = '/';

        if (1 != ret) {
            pmix_show_help("help-ptl-base.txt", "invalid if_inexclude", true,
                           name, pmix_globals.hostname, argv[i],
                           "Invalid specification (inet_pton() failed)");
            continue;
        }
        pmix_output_verbose(20, pmix_ptl_base_framework.framework_output,
                            "ptl:base: Searching for %s address+prefix: %s / %u", name,
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
                                    "ptl:base: Found match: %s (%s)",
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

    PMIx_Argv_free(argv);
    return interfaces;
}

char *pmix_ptl_base_peer_type(pmix_peer_t *peer)
{
    char **types = NULL;
    char *ans;

    if (PMIX_PEER_IS_CLIENT(peer)) {
        PMIx_Argv_append_nosize(&types, "CLIENT");
    }
    if (PMIX_PEER_IS_SINGLETON(peer)) {
        PMIx_Argv_append_nosize(&types, "SINGLETON");
    }
    if (PMIX_PEER_IS_SERVER(peer)) {
        PMIx_Argv_append_nosize(&types, "SERVER");
    }
    if (PMIX_PEER_IS_TOOL(peer)) {
        PMIx_Argv_append_nosize(&types, "TOOL");
    }
    if (PMIX_PEER_IS_LAUNCHER(peer)) {
        PMIx_Argv_append_nosize(&types, "LAUNCHER");
    }
    if (PMIX_PEER_IS_GATEWAY(peer)) {
        PMIx_Argv_append_nosize(&types, "GATEWAY");
    }
    if (PMIX_PEER_IS_SCHEDULER(peer)) {
        PMIx_Argv_append_nosize(&types, "SCHEDULER");
    }
    if (PMIX_PEER_IS_SYS_CTRLR(peer)) {
        PMIx_Argv_append_nosize(&types, "SYSCTRLR");
    }
    ans = PMIx_Argv_join(types, ',');
    PMIx_Argv_free(types);
    return ans;
}
