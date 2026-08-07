/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
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

#include "src/include/pmix_config.h"

#include "src/include/pmix_socket_errno.h"
#include "src/include/pmix_stdint.h"

#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"

#ifdef HAVE_STRING_H
#    include <string.h>
#endif
#include <fcntl.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_SOCKET_H
#    include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#    include <sys/un.h>
#endif
#ifdef HAVE_SYS_UIO_H
#    include <sys/uio.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#include <ctype.h>
#include <signal.h>
#include <sys/stat.h>
#include <event.h>
#include <pthread.h>

#include "src/class/pmix_list.h"
#include "src/include/pmix_socket_errno.h"
#include "src/mca/bfrops/base/base.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_basename.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_fd.h"
#include "src/util/pmix_net.h"
#include "src/util/pmix_os_dirpath.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_if.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_parse_options.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_show_help.h"
#include "src/util/pmix_string_copy.h"

#include "src/mca/ptl/base/base.h"

// local connection handler
static void connection_event_handler(int incoming_sd, short flags, void *cbdata);

// local value for connection support
static bool setup_complete = false;

/*
 * start listening event
 */
pmix_status_t pmix_ptl_base_start_listening(pmix_info_t info[], size_t ninfo)
{
    pmix_status_t rc;

    /* setup the listener */
    if (!setup_complete) {
        rc = pmix_ptl.setup_listener(info, ninfo);
        if (PMIX_SUCCESS != rc) {
            return rc;
        }
    }
    setup_complete = true;

    pmix_event_set(pmix_globals.evbase, &pmix_ptl_base.listener.ev,
               pmix_ptl_base.listener.socket,
               PMIX_EV_READ|PMIX_EV_PERSIST,
               connection_event_handler, 0);
    pmix_ptl_base.listener.active = true;
    pmix_event_add(&pmix_ptl_base.listener.ev, 0);

    return PMIX_SUCCESS;
}

void pmix_ptl_base_stop_listening(void)
{
    pmix_listener_t *lt = &pmix_ptl_base.listener;

    pmix_output_verbose(8, pmix_ptl_base_framework.framework_output,
                        "listen_thread: shutdown");

    if (!lt->active) {
        /* nothing we need do */
        return;
    }

    /* mark it as inactive */
    lt->active = false;
    pmix_event_del(&lt->ev);
    /* close the socket to remove the connection points */
    CLOSE_THE_SOCKET(lt->socket);
    lt->socket = -1;
}

/*
 * Handler for accepting connections from the event library
 */
static void connection_event_handler(int incoming_sd, short flags, void *cbdata)
{
    struct sockaddr addr;
    pmix_socklen_t addrlen = sizeof(struct sockaddr);
    int sd;
    pmix_pending_connection_t *pending_connection;
    pmix_listener_t *lt = &pmix_ptl_base.listener;
    PMIX_HIDE_UNUSED_PARAMS(flags, cbdata);

    sd = accept(incoming_sd, (struct sockaddr *) &addr, &addrlen);
    pmix_output_verbose(5, pmix_ptl_base_framework.framework_output,
                        "connection_event_handler: working connection "
                        "(%d, %d) %s:%d\n",
                        sd, pmix_socket_errno,
                        pmix_net_get_hostname((struct sockaddr *) &addr),
                        pmix_net_get_port((struct sockaddr *) &addr));
    if (sd < 0) {
        /* Non-fatal errors */
        if (EINTR == pmix_socket_errno ||
            EAGAIN == pmix_socket_errno ||
            EWOULDBLOCK == pmix_socket_errno) {
            return;
        }

        /* If we run out of file descriptors, log an extra warning (so
           that the user can know to fix this problem) and abandon all
           hope. */
        else if (EMFILE == pmix_socket_errno) {
            CLOSE_THE_SOCKET(incoming_sd);
            PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
            pmix_show_help("help-ptl-base.txt", "accept failed", true,
                           pmix_globals.hostname,
                           pmix_socket_errno, strerror(pmix_socket_errno),
                           "Out of file descriptors");
            return;
        }

        /* For all other cases, close the socket, print a warning but
           try to continue */
        else {
            CLOSE_THE_SOCKET(incoming_sd);
            pmix_show_help("help-ptl-base.txt", "accept failed", true,
                           pmix_globals.hostname,
                           pmix_socket_errno, strerror(pmix_socket_errno),
                           "Unknown cause; job will try to continue");
            return;
        }
    }

    /* this descriptor is ready to be read, which means a connection
     * request has been received - so harvest it. All we want to do
     * here is accept the connection and push the info onto the event
     * library for subsequent processing - we don't want to actually
     * process the connection here as it takes too long, and so the
     * OS might start rejecting connections due to timeout.
     */
    pending_connection = PMIX_NEW(pmix_pending_connection_t);
    pending_connection->protocol = lt->protocol;
    pmix_event_assign(&pending_connection->ev, pmix_globals.evbase,
                      -1, EV_WRITE,
                      lt->cbfunc, pending_connection);
    pending_connection->sd = sd;

    pmix_output_verbose(8, pmix_ptl_base_framework.framework_output,
                        "connection_event_handler: new connection: (%d, %d)", pending_connection->sd,
                        pmix_socket_errno);
    /* post the object */
    PMIX_POST_OBJECT(pending_connection);
    /* activate the event */
    pmix_event_active(&pending_connection->ev, EV_WRITE, 1);
}


/* A rendezvous file is removed by pmix_ptl_close when we finalize, so
 * a server that was killed or that crashed leaves its file behind. As
 * the file records the pid of the process that created it, we can tell
 * a stale file from one belonging to a still-running server - and
 * reclaim the stale one instead of refusing to start.
 *
 * Returns a string describing the live owner of the given file, or
 * NULL if the file has no live owner (i.e., it is stale and can be
 * reclaimed). The caller must free any returned string.
 */
static char *rndz_file_owner(char *filename)
{
    FILE *fp;
    char *line, *endptr, *owner = NULL;
    unsigned long pid = 0;
    bool havepid = false;
    int n;

    /* the file holds the uri, the version, the pid of the process
     * that wrote it, its uid:gid, and a timestamp - one per line.
     * We only care about the pid */
    fp = fopen(filename, "r");
    if (NULL == fp) {
        /* we cannot identify an owner, so treat it as stale - if it
         * truly is in use, then the removal below will fail and we
         * will report that instead */
        return NULL;
    }
    for (n = 0; n < 3; n++) {
        line = pmix_getline(fp);
        if (NULL == line) {
            /* the file was never completely written - e.g., its
             * creator died partway thru doing so */
            break;
        }
        if (2 == n) {
            endptr = NULL;
            errno = 0;
            pid = strtoul(line, &endptr, 10);
            if (0 == errno && NULL != endptr && endptr != line && 0 < pid) {
                havepid = true;
            }
        }
        free(line);
    }
    fclose(fp);

    if (!havepid) {
        return NULL;
    }
    /* if the recorded pid is our own, then the file cannot belong to
     * a live owner - we did not write it, so it is a leftover from an
     * earlier process that happened to be given the same pid */
    if ((pid_t)pid == pmix_globals.pid) {
        return NULL;
    }
    /* if that process is still running, then the file is in use and
     * we must not touch it. Note that a "permission denied" response
     * means the process does exist, but belongs to another user */
    if (0 != kill((pid_t)pid, 0) && EPERM != errno) {
        return NULL;
    }
    pmix_asprintf(&owner, "pid %lu", pid);
    return owner;
}

static pmix_status_t write_rndz_file(char *filename, char *uri, const char *role,
                                     bool *dir_created, bool *file_created)
{
    int fd;
    char *dirname, *tmp, *owner;
    time_t mytime;
    int rc, n;
    mode_t mode = 0;

    dirname = pmix_dirname(filename);
    if (NULL != dirname) {
        mode = S_IRWXU;
        if (pmix_ptl_base.allow_foreign_tools) {
            mode |= S_IXGRP | S_IRGRP | S_IXOTH | S_IROTH;
        }
        rc = pmix_os_dirpath_create(dirname, mode);
        if (PMIX_ERR_SILENT == rc) {
            // error has already been reported
            return rc;
        }
        if (PMIX_SUCCESS != rc && PMIX_ERR_EXISTS != rc) {
            PMIX_ERROR_LOG(rc);
            free(dirname);
            return rc;
        }
        if (PMIX_SUCCESS == rc) {
            // do not change the dir_created flag if the directory
            // already exists as we don't know if we previously
            // created it or it already existed. Success is returned
            // when we were able to both create the directory
            // and change its mode as directed
            *dir_created = true;
        }
        free(dirname);
    }

    /* set the file mode */
    mode = S_IRUSR | S_IWUSR ;
    if (pmix_ptl_base.allow_foreign_tools) {
        mode |= S_IRGRP | S_IROTH;
    }
    /* two passes at most: if the file already exists, we get one
     * chance to reclaim it as stale and try the create again */
    for (n = 0; n < 2; n++) {
        fd = open(filename, O_RDWR | O_CREAT | O_EXCL, mode);
        if (0 <= fd || EEXIST != errno) {
            break;
        }
        /* the file already exists - if a live process owns it, then
         * another server holds this role and we must not disturb it */
        owner = rndz_file_owner(filename);
        if (NULL != owner) {
            pmix_show_help("help-ptl-base.txt", "rndz-file-in-use", true,
                           role, filename, owner);
            free(owner);
            *file_created = false;
            /* we have explained the problem, so don't let our caller
             * add a misleading message on top of it */
            return PMIX_ERR_SILENT;
        }
        /* it was left behind by an instance that is no longer
         * running, so reclaim it. Someone else beating us to the
         * removal is fine - anything else is not */
        if (0 != unlink(filename) && ENOENT != errno) {
            pmix_show_help("help-ptl-base.txt", "rndz-file-stale", true,
                           role, filename, strerror(errno));
            *file_created = false;
            return PMIX_ERR_SILENT;
        }
    }
    if (0 > fd) {
        if (EEXIST == errno) {
            /* another process recreated the file while we were
             * reclaiming it */
            pmix_show_help("help-ptl-base.txt", "rndz-file-in-use", true,
                           role, filename, "another process");
            *file_created = false;
            return PMIX_ERR_SILENT;
        }
        pmix_output(0, "Impossible to open the file %s in write mode\n", filename);
        PMIX_ERROR_LOG(PMIX_ERR_FILE_OPEN_FAILURE);
        *file_created = false;
        return PMIX_ERR_FILE_OPEN_FAILURE;
    }

    /* output the information */
    mytime = time(NULL);
    pmix_asprintf(&tmp, "%s\n%s\n%lu\n%lu:%lu\n%s\n",
                  uri, PMIX_VERSION, (unsigned long)pmix_globals.pid,
                  (unsigned long)pmix_globals.uid,
                  (unsigned long)pmix_globals.gid,
                  ctime(&mytime));
    rc = write(fd, tmp, strlen(tmp));
    if (0 > rc) {
        PMIX_ERROR_LOG(PMIX_ERR_FILE_WRITE_FAILURE);
        *file_created = false;
        pmix_free(tmp);
        close(fd);
        return PMIX_ERR_FILE_WRITE_FAILURE;
    }
    pmix_free(tmp);
    close(fd);
    *file_created = true;
    return PMIX_SUCCESS;
}

/* discover the available
 * interfaces, filter them thru any given directives, and select
 * the one we will listen on for connection requests. This will
 * be a loopback device by default, unless we are asked to support
 * tool connections - in that case, we will take a non-loopback
 * device by default, if one is available after filtering directives
 *
 * If we are a tool and were give a rendezvous file, then we first
 * check to see if it already exists. If it does, then this is the
 * connection info we are to use. If it doesn't, then this is the
 * name of the file we are to use to store our listener info.
 *
 * If we are a server and are given a rendezvous file, then that is
 * is the name of the file we are to use to store our listener info.
 */
pmix_status_t pmix_ptl_base_setup_listener(pmix_info_t info[], size_t ninfo)
{
    int flags = 0;
    pmix_listener_t *lt;
    int i, rc = 0, saveindex = -1, savelpbk = -1;
    char **interfaces = NULL, **candidates = NULL, **ports;
    bool including = false;
    char name[32];
    struct sockaddr_storage my_ss;
    int kindex;
    pmix_socklen_t addrlen;
    char *prefix;
    char myconnhost[PMIX_MAXHOSTNAMELEN] = {0};
    int myport;
    pmix_kval_t *urikv;
    pid_t mypid;
    int outpipe;
    char *leftover;
    size_t n;
    FILE *fptst;
    uint16_t port = 0;

    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "ptl:tool setup_listener");

    for (n = 0; n < ninfo; n++) {
        if (0 == strcmp(info[n].key, PMIX_SERVER_SESSION_SUPPORT)) {
            pmix_ptl_base.session_tool = PMIX_INFO_TRUE(&info[n]);

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_SYSTEM_SUPPORT)) {
            pmix_ptl_base.system_tool = PMIX_INFO_TRUE(&info[n]);

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_ALLOW_FOREIGN_TOOLS)) {
            pmix_ptl_base.allow_foreign_tools = PMIX_INFO_TRUE(&info[n]);

        } else if (0 == strcmp(info[n].key, PMIX_SERVER_TOOL_SUPPORT)) {
            pmix_ptl_base.tool_support = PMIX_INFO_TRUE(&info[n]);

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_REMOTE_CONNECTIONS)) {
            pmix_ptl_base.remote_connections = PMIX_INFO_TRUE(&info[n]);
            pmix_ptl_base.connections_specified = true;

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_TCP_IF_INCLUDE)) {
            if (NULL != pmix_ptl_base.if_include) {
                free(pmix_ptl_base.if_include);
            }
            pmix_ptl_base.if_include = strdup(info[n].value.data.string);

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_TCP_IF_EXCLUDE)) {
            if (NULL != pmix_ptl_base.if_exclude) {
                free(pmix_ptl_base.if_exclude);
            }
            pmix_ptl_base.if_exclude = strdup(info[n].value.data.string);

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_TCP_IPV4_PORT)) {
            if (PMIX_INT == info[n].value.type) {
                if (NULL != pmix_ptl_base.ipv4_ports) {
                    PMIx_Argv_free(pmix_ptl_base.ipv4_ports);
                }
                pmix_ptl_base.ipv4_ports = pmix_malloc(2*sizeof(char*));
                pmix_ptl_base.ipv4_ports[1] = NULL;
                if (0 < info[n].value.data.integer) {
                    pmix_asprintf(&pmix_ptl_base.ipv4_ports[0], "%d", info[n].value.data.integer);
                } else {
                    pmix_ptl_base.ipv4_ports[0] = strdup("0");
                }
            } else if (PMIX_STRING == info[n].value.type) {
                if (NULL != pmix_ptl_base.ipv4_ports) {
                    PMIx_Argv_free(pmix_ptl_base.ipv4_ports);
                }
                if (NULL == info[n].value.data.string) {
                    pmix_ptl_base.ipv4_ports = pmix_malloc(2*sizeof(char*));
                    pmix_ptl_base.ipv4_ports[0] = strdup("0");
                    pmix_ptl_base.ipv4_ports[1] = NULL;
                } else {
                    pmix_util_parse_range_options(info[n].value.data.string, &pmix_ptl_base.ipv4_ports);
                    if (0 == strcmp(pmix_ptl_base.ipv4_ports[0], "-1")) {
                        PMIx_Argv_free(pmix_ptl_base.ipv4_ports);
                        pmix_ptl_base.ipv4_ports = pmix_malloc(2*sizeof(char*));
                        pmix_ptl_base.ipv4_ports[0] = strdup("0");
                        pmix_ptl_base.ipv4_ports[1] = NULL;
                    }
                }
            }

#if PMIX_ENABLE_IPV6
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_TCP_IPV6_PORT)) {
            if (PMIX_INT == info[n].value.type) {
                if (NULL != pmix_ptl_base.ipv6_ports) {
                    PMIx_Argv_free(pmix_ptl_base.ipv6_ports);
                    pmix_ptl_base.ipv6_ports = NULL;
                }
                pmix_ptl_base.ipv6_ports = pmix_malloc(2*sizeof(char*));
                pmix_ptl_base.ipv6_ports[1] = NULL;
                if (0 < info[n].value.data.integer) {
                    pmix_asprintf(&pmix_ptl_base.ipv6_ports[0], "%d", info[n].value.data.integer);
                } else {
                    pmix_ptl_base.ipv6_ports[0] = strdup("0");
                }
            } else if (PMIX_STRING == info[n].value.type) {
                if (NULL != pmix_ptl_base.ipv6_ports) {
                    PMIx_Argv_free(pmix_ptl_base.ipv6_ports);
                }
                if (NULL == info[n].value.data.string) {
                    pmix_ptl_base.ipv6_ports = pmix_malloc(2*sizeof(char*));
                    pmix_ptl_base.ipv6_ports[0] = strdup("0");
                    pmix_ptl_base.ipv6_ports[1] = NULL;
                } else {
                    pmix_util_parse_range_options(info[n].value.data.string, &pmix_ptl_base.ipv6_ports);
                     if (0 == strcmp(pmix_ptl_base.ipv6_ports[0], "-1")) {
                        PMIx_Argv_free(pmix_ptl_base.ipv6_ports);
                        pmix_ptl_base.ipv6_ports = pmix_malloc(2*sizeof(char*));
                        pmix_ptl_base.ipv6_ports[0] = strdup("0");
                        pmix_ptl_base.ipv6_ports[1] = NULL;
                    }
                }
           }
#endif

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_TCP_DISABLE_IPV4)) {
            pmix_ptl_base.disable_ipv4_family = PMIX_INFO_TRUE(&info[n]);

#if PMIX_ENABLE_IPV6
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_TCP_DISABLE_IPV6)) {
            pmix_ptl_base.disable_ipv6_family = PMIX_INFO_TRUE(&info[n]);
#endif

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_TCP_REPORT_URI)) {
            if (NULL != pmix_ptl_base.report_uri) {
                free(pmix_ptl_base.report_uri);
            }
            pmix_ptl_base.report_uri = strdup(info[n].value.data.string);

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SERVER_TMPDIR)) {
            if (NULL != pmix_ptl_base.session_tmpdir) {
                free(pmix_ptl_base.session_tmpdir);
            }
            pmix_ptl_base.session_tmpdir = strdup(info[n].value.data.string);

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SYSTEM_TMPDIR)) {
            if (NULL != pmix_ptl_base.system_tmpdir) {
                free(pmix_ptl_base.system_tmpdir);
            }
            pmix_ptl_base.system_tmpdir = strdup(info[n].value.data.string);
        }
    }

    if (NULL != pmix_ptl_base.if_include && NULL != pmix_ptl_base.if_exclude) {
        pmix_show_help("help-ptl-base.txt", "include-exclude", true, pmix_ptl_base.if_include,
                       pmix_ptl_base.if_exclude);
        return PMIX_ERR_SILENT;
    }

    lt = &pmix_ptl_base.listener;

    /* if interface include was given, construct a list
     * of those interfaces which match the specifications - remember,
     * the includes could be given as named interfaces, IP addrs, or
     * subnet+mask
     */
    if (NULL != pmix_ptl_base.if_include) {
        interfaces = pmix_ptl_base_split_and_resolve(pmix_ptl_base.if_include, "include");
        including = true;
    } else if (NULL != pmix_ptl_base.if_exclude) {
        interfaces = pmix_ptl_base_split_and_resolve(pmix_ptl_base.if_exclude, "exclude");
        including = false;
    }

    /* look at all available interfaces and pick one - we default to a
     * loopback interface if available, but otherwise pick the first
     * available interface since we are only talking locally */
    for (i = pmix_ifbegin(); i >= 0; i = pmix_ifnext(i)) {
        if (PMIX_SUCCESS != pmix_ifindextoaddr(i, (struct sockaddr *) &my_ss, sizeof(my_ss))) {
            pmix_output(0, "ptl_tool: problems getting address for index %i (kernel index %i)\n", i,
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
                        "ptl:tool:init rejecting interface %s (not in include list)", name);
                    continue;
                }
            } else {
                /* we are excluding, so ignore if present */
                if (PMIX_SUCCESS == rc) {
                    pmix_output_verbose(10, pmix_ptl_base_framework.framework_output,
                                        "ptl:tool:init rejecting interface %s (in exclude list)",
                                        name);
                    continue;
                }
            }
        }

        /* if this is the loopback device and they didn't enable
         * remote connections, then we are done */
        if (pmix_ifisloopback(i)) {
            pmix_output_verbose(5, pmix_ptl_base_framework.framework_output,
                                "ptl:tool:init loopback interface %s found", name);
            if (savelpbk < 0) {
                savelpbk = i;
            }

        } else if (saveindex < 0) {
            saveindex = i;
        }
        if (0 <= savelpbk && 0 <= saveindex) {
            break;
        }
    }
    /* cleanup */
    if (NULL != interfaces) {
        PMIx_Argv_free(interfaces);
    }

    // if they enabled remote connections, then we must have a public
    // interface available to us
    if (pmix_ptl_base.remote_connections) {
        if (saveindex < 0) {
            prefix = PMIx_Argv_join(candidates, ',');
            pmix_show_help("help-ptl-base.txt", "no-remote-interfaces", true,
                            (NULL == pmix_ptl_base.if_include) ? "NULL" : pmix_ptl_base.if_include,
                            (NULL == pmix_ptl_base.if_exclude) ? "NULL" : pmix_ptl_base.if_exclude,
                            prefix);
            PMIx_Argv_free(candidates);
            free(prefix);
            return PMIX_ERR_SILENT;
        }
        // if we found a public interface, then just fall through and use it
        PMIx_Argv_free(candidates);

    } else {
        // if there are no loopback interfaces, and if they didn't specify
        // we shouldn't support remote connections, then take a public
        // interface if available
        if (savelpbk < 0) {
            if (!pmix_ptl_base.connections_specified) {
                // fallback to a public interface, if available
                if (saveindex < 0) {
                    // didn't find anything!
                    prefix = PMIx_Argv_join(candidates, ',');
                    pmix_show_help("help-ptl-base.txt", "no-available-interfaces", true,
                                    (NULL == pmix_ptl_base.if_include) ? "NULL" : pmix_ptl_base.if_include,
                                    (NULL == pmix_ptl_base.if_exclude) ? "NULL" : pmix_ptl_base.if_exclude,
                                    prefix);
                    PMIx_Argv_free(candidates);
                    free(prefix);
                    return PMIX_ERR_SILENT;
                } else {
                    // go ahead and use the public interface
                    PMIx_Argv_free(candidates);
                    goto complete;
                }
            } else {
                // they said not to allow remote connections, so we cannot
                // take a public interface
                prefix = PMIx_Argv_join(candidates, ',');
                pmix_show_help("help-ptl-base.txt", "no-loopback-interfaces", true,
                                (NULL == pmix_ptl_base.if_include) ? "NULL" : pmix_ptl_base.if_include,
                                (NULL == pmix_ptl_base.if_exclude) ? "NULL" : pmix_ptl_base.if_exclude,
                                prefix);
                PMIx_Argv_free(candidates);
                free(prefix);
                return PMIX_ERR_SILENT;
            }

        } else {
            // they didn't ask for remote support, and we found a loopback
            // interface, so use it
            saveindex = savelpbk;
            PMIx_Argv_free(candidates);
        }
    }


complete:
    /* save the connection */
    if (PMIX_SUCCESS
        != pmix_ifindextoaddr(saveindex, (struct sockaddr *) pmix_ptl_base.connection,
                              sizeof(struct sockaddr_storage))) {
        pmix_output(0, "ptl:base: problems getting address for kernel index %i\n",
                    pmix_ifindextokindex(saveindex));
        return PMIX_ERR_NOT_AVAILABLE;
    }

    // set the ports to scan based on the connection we selected
    if (AF_INET == pmix_ptl_base.connection->ss_family) {
        ports = pmix_ptl_base.ipv4_ports;
#if PMIX_ENABLE_IPV6
    } else if (AF_INET6 == pmix_ptl_base.connection->ss_family) {
        ports = pmix_ptl_base.ipv6_ports;
#endif
    } else {
        /* unrecognized family type - shouldn't be possible as we only
         * included IPv4 and IPv6 interfaces, but this is needed to
         * silence warnings */
        return PMIX_ERR_NOT_SUPPORTED;
    }

    lt->varname = pmix_bfrops_base_get_components();
    lt->protocol = PMIX_PROTOCOL_V2;
    lt->cbfunc = pmix_ptl_base_connection_handler;

    rc = PMIX_ERR_NOT_AVAILABLE;
    for (n=0; NULL != ports[n]; n++) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:setup_listener - trying port %s", ports[n]);

        /* get the port number */
        port = strtol(ports[n], NULL, 10);
        /* convert it to network-byte-order */
        port = htons(port);

        /* set the port */
        if (AF_INET == pmix_ptl_base.connection->ss_family) {
            ((struct sockaddr_in *) pmix_ptl_base.connection)->sin_port = port;
            addrlen = sizeof(struct sockaddr_in);
            if (0 != port) {
                flags = 1;
            }
#if PMIX_ENABLE_IPV6
        } else if (AF_INET6 == pmix_ptl_base.connection->ss_family) {
            ((struct sockaddr_in6 *) pmix_ptl_base.connection)->sin6_port = port;
            addrlen = sizeof(struct sockaddr_in6);
            if (0 != port) {
                flags = 1;
            }
#endif
        }

        /* create a listen socket for incoming connection attempts */
        lt->socket = socket(pmix_ptl_base.connection->ss_family, SOCK_STREAM, 0);
        if (lt->socket < 0) {
            pmix_output(0, "ptl:base:create_listen: socket() failed: %s (%d)\n",
                        strerror(pmix_socket_errno), pmix_socket_errno);
            rc = PMIX_ERR_OUT_OF_RESOURCE;
            goto sockerror;
        }

        /* set reusing ports flag */
        if (setsockopt(lt->socket, SOL_SOCKET, SO_REUSEADDR, (const char *) &flags, sizeof(flags)) < 0) {
            pmix_output(0,
                        "ptl:base:create_listen: unable to set the "
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

        if (bind(lt->socket, (struct sockaddr *) pmix_ptl_base.connection, addrlen) < 0) {
            if ((EADDRINUSE == pmix_socket_errno) || (EADDRNOTAVAIL == pmix_socket_errno)) {
                /* this port is taken - drop the socket we opened for it
                 * before moving on, or we leak one descriptor per busy
                 * port in the range we were given */
                CLOSE_THE_SOCKET(lt->socket);
                continue;
            }
            pmix_output(0, "ptl:base:create_listen: bind() failed for socket %d "
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
    if (getsockname(lt->socket, (struct sockaddr *) pmix_ptl_base.connection, &addrlen) < 0) {
        pmix_output(0, "ptl:base:create_listen: getsockname(): %s (%d)",
                    strerror(pmix_socket_errno), pmix_socket_errno);
        rc = PMIX_ERROR;
        goto sockerror;
    }

    /* setup listen backlog to maximum allowed by kernel */
    if (listen(lt->socket, SOMAXCONN) < 0) {
        pmix_output(0, "ptl:base:create_listen: listen() failed: %s (%d)\n",
                    strerror(pmix_socket_errno), pmix_socket_errno);
        rc = PMIX_ERROR;
        goto sockerror;
    }

    /* set socket up to be non-blocking, otherwise accept could block */
    if ((flags = fcntl(lt->socket, F_GETFL, 0)) < 0) {
        pmix_output(0, "ptl:base:create_listen: fcntl(F_GETFL) failed: %s (%d)\n",
                    strerror(pmix_socket_errno), pmix_socket_errno);
        rc = PMIX_ERROR;
        goto sockerror;
    }
    flags |= O_NONBLOCK;
    if (fcntl(lt->socket, F_SETFL, flags) < 0) {
        pmix_output(0, "ptl:base:create_listen: fcntl(F_SETFL) failed: %s (%d)\n",
                    strerror(pmix_socket_errno), pmix_socket_errno);
        rc = PMIX_ERROR;
        goto sockerror;
    }

    if (AF_INET == pmix_ptl_base.connection->ss_family) {
        prefix = "tcp4://";
        myport = ntohs(((struct sockaddr_in *) pmix_ptl_base.connection)->sin_port);
        inet_ntop(AF_INET, &((struct sockaddr_in *) pmix_ptl_base.connection)->sin_addr,
                  myconnhost, PMIX_MAXHOSTNAMELEN - 1);
    } else if (AF_INET6 == pmix_ptl_base.connection->ss_family) {
        prefix = "tcp6://";
        myport = ntohs(((struct sockaddr_in6 *) pmix_ptl_base.connection)->sin6_port);
        inet_ntop(AF_INET6, &((struct sockaddr_in6 *) pmix_ptl_base.connection)->sin6_addr,
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
                        "ptl:base URI %s", lt->uri);

    /* save the URI internally so we can report it */
    urikv = PMIX_NEW(pmix_kval_t);
    urikv->key = strdup(PMIX_MYSERVER_URI);
    PMIX_VALUE_CREATE(urikv->value, 1);
    PMIX_VALUE_LOAD(urikv->value, lt->uri, PMIX_STRING);
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, urikv);
    PMIX_RELEASE(urikv); // maintain accounting

    /* save a legacy URI internally so we can report it
     * to older tools */
    urikv = PMIX_NEW(pmix_kval_t);
    urikv->key = strdup(PMIX_SERVER_URI);
    PMIX_VALUE_CREATE(urikv->value, 1);
    PMIX_VALUE_LOAD(urikv->value, lt->uri, PMIX_STRING);
    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &pmix_globals.myid, PMIX_INTERNAL, urikv);
    PMIX_RELEASE(urikv); // maintain accounting

    if (NULL != pmix_ptl_base.report_uri) {
        /* if the string is a "-", then output to stdout */
        if (0 == strcmp(pmix_ptl_base.report_uri, "-")) {
            fprintf(stdout, "%s\n", lt->uri);
        } else if (0 == strcmp(pmix_ptl_base.report_uri, "+")) {
            /* output to stderr */
            fprintf(stderr, "%s\n", lt->uri);
        } else {
            /* see if it is an integer pipe */
            leftover = NULL;
            outpipe = strtol(pmix_ptl_base.report_uri, &leftover, 10);
            if (NULL == leftover || 0 == strlen(leftover)) {
                /* stitch together the var names and URI */
                pmix_asprintf(&leftover, "%s;%s", lt->varname, lt->uri);
                /* output to the pipe */
                rc = pmix_fd_write(outpipe, strlen(leftover) + 1, leftover);
                free(leftover);
                close(outpipe);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    goto sockerror;
                }
            } else {
                /* must be a file */
                FILE *fp;
                fp = fopen(pmix_ptl_base.report_uri, "w");
                if (NULL == fp) {
                    pmix_output(0, "Impossible to open the file %s in write mode\n",
                                pmix_ptl_base.report_uri);
                    PMIX_ERROR_LOG(PMIX_ERR_FILE_OPEN_FAILURE);
                    rc = PMIX_ERR_FILE_OPEN_FAILURE;
                    goto sockerror;
                }
                /* output my nspace and rank plus the URI */
                fprintf(fp, "%s\n", lt->uri);
                /* add a flag that indicates we accept v2.1 protocols */
                fprintf(fp, "v%s\n", PMIX_VERSION);
                fclose(fp);
                pmix_ptl_base.created_urifile = true;
            }
        }
    }

    /* if we were given a rendezvous file, then drop it */
    if (NULL != pmix_ptl_base.rendezvous_filename) {
        /* if we are a tool and the file already exists, then we
         * just use it as providing the rendezvous info for our
         * server */
        if (PMIX_PEER_IS_TOOL(pmix_globals.mypeer)) {
            fptst = fopen(pmix_ptl_base.rendezvous_filename, "r");
            if (NULL != fptst) {
                fclose(fptst);
                goto nextstep;
            }
        }
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "WRITING RENDEZVOUS FILE %s", pmix_ptl_base.rendezvous_filename);
        rc = write_rndz_file(pmix_ptl_base.rendezvous_filename, lt->uri,
                             "server",
                             &pmix_ptl_base.created_rendezvous_dir,
                             &pmix_ptl_base.created_rendezvous_file);
        if (PMIX_SUCCESS != rc) {
            goto sockerror;
        }
    }

nextstep:
    /* if we are the scheduler, then drop an appropriately named
     * contact file so the system's resource manager can find us */
    if (PMIX_PEER_IS_SCHEDULER(pmix_globals.mypeer)) {
        if (0 > pmix_asprintf(&pmix_ptl_base.scheduler_filename, "%s/pmix.sched.%s",
                         pmix_ptl_base.system_tmpdir, pmix_globals.hostname)) {
            rc = PMIX_ERR_NOMEM;
            goto sockerror;
        }
        rc = write_rndz_file(pmix_ptl_base.scheduler_filename, lt->uri,
                             "scheduler",
                             &pmix_ptl_base.created_system_tmpdir,
                             &pmix_ptl_base.created_scheduler_filename);
        if (PMIX_SUCCESS != rc) {
            goto sockerror;
        }

    } else if (PMIX_PEER_IS_SYS_CTRLR(pmix_globals.mypeer)) {
        /* if we are the system controller, then drop an appropriately named
         * contact file so others can find us */
            if (0 > pmix_asprintf(&pmix_ptl_base.sysctrlr_filename, "%s/pmix.sysctrlr.%s",
                         pmix_ptl_base.system_tmpdir, pmix_globals.hostname)) {
            rc = PMIX_ERR_NOMEM;
            goto sockerror;
        }
        rc = write_rndz_file(pmix_ptl_base.sysctrlr_filename, lt->uri,
                             "system controller",
                             &pmix_ptl_base.created_system_tmpdir,
                             &pmix_ptl_base.created_sysctrlr_filename);
        if (PMIX_SUCCESS != rc) {
            goto sockerror;
        }
    } else if (pmix_ptl_base.tool_support) {
        /* if we are going to support tools, then drop contact file(s) */
        if (pmix_ptl_base.system_tool) {
            if (0 > pmix_asprintf(&pmix_ptl_base.system_filename, "%s/pmix.sys.%s",
                             pmix_ptl_base.system_tmpdir, pmix_globals.hostname)) {
                rc = PMIX_ERR_NOMEM;
                goto sockerror;
            }
            rc = write_rndz_file(pmix_ptl_base.system_filename, lt->uri,
                                 "system tool",
                                 &pmix_ptl_base.created_system_tmpdir,
                                 &pmix_ptl_base.created_system_filename);
            if (PMIX_SUCCESS != rc) {
                goto sockerror;
            }
        }

        if (pmix_ptl_base.session_tool) {
            /* first output to a std file */
            if (0 > pmix_asprintf(&pmix_ptl_base.session_filename, "%s/pmix.%s.tool",
                             pmix_ptl_base.session_tmpdir, pmix_globals.hostname)) {
                rc = PMIX_ERR_NOMEM;
                goto sockerror;
            }
            pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                                "WRITING SESSION TOOL FILE %s", pmix_ptl_base.session_filename);
            rc = write_rndz_file(pmix_ptl_base.session_filename, lt->uri,
                                 "session tool",
                                 &pmix_ptl_base.created_session_tmpdir,
                                 &pmix_ptl_base.created_session_filename);
            if (PMIX_SUCCESS != rc) {
                goto sockerror;
            }
        }

        /* now output to a file based on pid */
        mypid = getpid();
        if (0 > pmix_asprintf(&pmix_ptl_base.pid_filename, "%s/pmix.%s.tool.%d",
                         pmix_ptl_base.session_tmpdir, pmix_globals.hostname, mypid)) {
            rc = PMIX_ERR_NOMEM;
            goto sockerror;
        }
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output, "WRITING PID TOOL FILE %s",
                            pmix_ptl_base.pid_filename);
        rc = write_rndz_file(pmix_ptl_base.pid_filename, lt->uri,
                             "tool (by pid)",
                             &pmix_ptl_base.created_session_tmpdir,
                             &pmix_ptl_base.created_pid_filename);
        if (PMIX_SUCCESS != rc) {
            goto sockerror;
        }

        /* now output it into a file based on my nspace */
        if (0 > pmix_asprintf(&pmix_ptl_base.nspace_filename, "%s/pmix.%s.tool.%s",
                         pmix_ptl_base.session_tmpdir, pmix_globals.hostname,
                         pmix_globals.myid.nspace)) {
            rc = PMIX_ERR_NOMEM;
            goto sockerror;
        }
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "WRITING NSPACE TOOL FILE %s", pmix_ptl_base.nspace_filename);
        rc = write_rndz_file(pmix_ptl_base.nspace_filename, lt->uri,
                             "tool (by namespace)",
                             &pmix_ptl_base.created_session_tmpdir,
                             &pmix_ptl_base.created_nspace_filename);
        if (PMIX_SUCCESS != rc) {
            goto sockerror;
        }
    }

    return PMIX_SUCCESS;

sockerror:
    CLOSE_THE_SOCKET(lt->socket);
    return rc;
}
