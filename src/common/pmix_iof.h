/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2011 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2008      Cisco Systems, Inc.  All rights reserved.
 * Copyright (c) 2012-2013 Los Alamos National Security, LLC.
 *                         All rights reserved.
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2017      IBM Corporation.  All rights reserved.
 * Copyright (c) 2017      Mellanox Technologies. All rights reserved.
 * Copyright (c) 2018      Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */
/**
 * @file
 *
 * I/O Forwarding Service
 */

#ifndef PMIX_IOF_H
#define PMIX_IOF_H

#include "src/include/pmix_config.h"

#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_UIO_H
#    include <sys/uio.h>
#endif
#ifdef HAVE_NET_UIO_H
#    include <net/uio.h>
#endif
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#include <signal.h>

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/util/pmix_fd.h"

BEGIN_C_DECLS

/*
 * Maximum size of single msg
 */
#define PMIX_IOF_BASE_MSG_MAX        8192
#define PMIX_IOF_BASE_TAG_MAX        1024
#define PMIX_IOF_MAX_INPUT_BUFFERS   50
#define PMIX_IOF_MAX_RETRIES         4

typedef struct {
    pmix_list_item_t super;
    bool pending;
    bool always_writable;
    int numtries;
    pmix_event_t *ev;
    struct timeval tv;
    int fd;
    pmix_list_t outputs;
} pmix_iof_write_event_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_iof_write_event_t);
#define PMIX_IOF_WRITE_EVENT_STATIC_INIT(w) \
{                                           \
    .super = PMIX_LIST_ITEM_STATIC_INIT,    \
    .pending = false,                       \
    .always_writable = false,               \
    .numtries = 0,                          \
    .ev = NULL,                             \
    .tv = {0, 0},                           \
    .fd = 0,                                \
    .outputs = PMIX_LIST_STATIC_INIT((w).outputs) \
}

typedef struct {
    pmix_list_item_t super;
    pmix_proc_t name;
    pmix_iof_channel_t tag;
    pmix_iof_write_event_t wev;
    bool xoff;
    bool exclusive;
    bool closed;
} pmix_iof_sink_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_iof_sink_t);
#define PMIX_IOF_SINK_STATIC_INIT(s)            \
{                                               \
    .super = PMIX_LIST_ITEM_STATIC_INIT,        \
    .name = {{0}, 0},                           \
    .tag = PMIX_FWD_NO_CHANNELS,                \
    .wev = PMIX_IOF_WRITE_EVENT_STATIC_INIT((s).wev), \
    .xoff = false,                              \
    .exclusive = false,                         \
    .closed = false                             \
}

typedef struct {
    pmix_list_item_t super;
    char *data;
    int numbytes;
} pmix_iof_write_output_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_iof_write_output_t);

typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    struct timeval tv;
    int fd;
    bool active;
    void *childproc;
    bool always_readable;
    pmix_proc_t name;
    pmix_iof_channel_t channel;
    pmix_proc_t *targets;
    size_t ntargets;
    pmix_info_t *directives;
    size_t ndirs;
    /* flow control: while xoff is set the event is left un-armed, so
     * the bytes we would have read stay in the input stream and the
     * OS applies the back-pressure for us. Cleared by an XON, which
     * re-arms the event */
    bool xoff;
} pmix_iof_read_event_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_iof_read_event_t);

typedef struct {
    pmix_list_item_t super;
    pmix_proc_t name;
    pmix_iof_write_event_t *channel;
    pmix_iof_flags_t flags;
    pmix_iof_channel_t stream;
    bool copystdout;
    bool copystderr;
    pmix_byte_object_t bo;
} pmix_iof_residual_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_iof_residual_t);

/* One chunk of output being held because nothing yet says how to format
 * it - see the pending-cache comment on pmix_globals_t and the entry
 * points below. Deliberately unlike pmix_iof_residual_t above, this
 * carries no flags and no channel: the whole point is that neither is
 * known yet, and both are worked out when the chunk is finally written.
 * A zero-size bo is a legitimate entry - it is the end-of-stream marker,
 * which has to keep its place in the sequence. */
typedef struct {
    pmix_list_item_t super;
    pmix_proc_t name;
    pmix_iof_channel_t stream;
    pmix_byte_object_t bo;
} pmix_iof_pending_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_iof_pending_t);

/* Write event macro's */

static inline bool pmix_iof_fd_always_ready(int fd)
{
    return pmix_fd_is_regular(fd) || (pmix_fd_is_chardev(fd) && !isatty(fd))
           || pmix_fd_is_blkdev(fd);
}

#define PMIX_IOF_SINK_BLOCKSIZE (1024)

#define PMIX_IOF_SINK_ACTIVATE(w)                                      \
    do {                                                               \
        struct timeval *tv = NULL;                                     \
        (w)->pending = true;                                           \
        PMIX_POST_OBJECT((w));                                         \
        if ((w)->always_writable) {                                    \
            /* Regular is always write ready. Use timer to activate */ \
            tv = &(w)->tv;                                             \
        }                                                              \
        if (pmix_event_add((w)->ev, tv)) {                             \
            PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);                        \
        }                                                              \
    } while (0);

/* define an output "sink", adding it to the provided
 * endpoint list for this proc */
#define PMIX_IOF_SINK_DEFINE(snk, nm, fid, tg, wrthndlr)                                           \
    do {                                                                                           \
        pmix_output_verbose(1, pmix_client_globals.iof_output,                                     \
                             "defining endpt: file %s line %d fd %d", __FILE__, __LINE__, (fid));  \
        pmix_strncpy((snk)->name.nspace, (nm)->nspace, PMIX_MAX_NSLEN);                            \
        (snk)->name.rank = (nm)->rank;                                                             \
        (snk)->tag = (tg);                                                                         \
        if (0 <= (fid)) {                                                                          \
            (snk)->wev.fd = (fid);                                                                 \
            (snk)->wev.always_writable = pmix_iof_fd_always_ready(fid);                            \
            if ((snk)->wev.always_writable) {                                                      \
                pmix_event_evtimer_set(pmix_globals.evbase, (snk)->wev.ev, wrthndlr, (snk));       \
            } else {                                                                               \
                pmix_event_set(pmix_globals.evbase, (snk)->wev.ev, (snk)->wev.fd, PMIX_EV_WRITE,   \
                               wrthndlr, (snk));                                                   \
            }                                                                                      \
        }                                                                                          \
        PMIX_POST_OBJECT(snk);                                                                     \
    } while (0);

/* Read event macro's */
#define PMIX_IOF_READ_ADDEV(rev)                \
    do {                                        \
        struct timeval *tv = NULL;              \
        if ((rev)->always_readable) {           \
            tv = &(rev)->tv;                    \
        }                                       \
        if (pmix_event_add(&(rev)->ev, tv)) {   \
            PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM); \
        }                                       \
    } while (0);

#define PMIX_IOF_READ_ACTIVATE(rev) \
    do {                            \
        (rev)->active = true;       \
        PMIX_POST_OBJECT(rev);      \
        PMIX_IOF_READ_ADDEV(rev);   \
    } while (0);

#define PMIX_IOF_READ_EVENT(rv, p, np, d, nd, fid, cbfunc, actv)                                 \
    do {                                                                                         \
        size_t _ii;                                                                              \
        pmix_iof_read_event_t *rev;                                                              \
        pmix_output_verbose(1, pmix_client_globals.iof_output, "defining read event at: %s %d", \
                             __FILE__, __LINE__);                                                \
        rev = PMIX_NEW(pmix_iof_read_event_t);                                                   \
        if (NULL != (p)) {                                                                       \
            (rev)->ntargets = (np);                                                              \
            PMIX_PROC_CREATE((rev)->targets, (rev)->ntargets);                                   \
            memcpy((rev)->targets, (p), (np) * sizeof(pmix_proc_t));                             \
        }                                                                                        \
        if (NULL != (d) && 0 < (nd)) {                                                           \
            PMIX_INFO_CREATE((rev)->directives, (nd));                                           \
            (rev)->ndirs = (nd);                                                                 \
            for (_ii = 0; _ii < (size_t) nd; _ii++) {                                            \
                PMIX_INFO_XFER(&((rev)->directives[_ii]), &((d)[_ii]));                          \
            }                                                                                    \
        }                                                                                        \
        rev->fd = (fid);                                                                         \
        rev->always_readable = pmix_iof_fd_always_ready(fid);                                    \
        *(rv) = rev;                                                                             \
        if (rev->always_readable) {                                                              \
            pmix_event_evtimer_set(pmix_globals.evbase, &rev->ev, (cbfunc), rev);                \
        } else {                                                                                 \
            pmix_event_set(pmix_globals.evbase, &rev->ev, (fid), PMIX_EV_READ, (cbfunc), rev);   \
        }                                                                                        \
        if ((actv)) {                                                                            \
            PMIX_IOF_READ_ACTIVATE(rev)                                                          \
        }                                                                                        \
    } while (0);

#define PMIX_IOF_READ_EVENT_LOCAL(rv, fid, cbfunc, actv)                                         \
    do {                                                                                         \
        pmix_iof_read_event_t *rev;                                                              \
        pmix_output_verbose(1, pmix_client_globals.iof_output, "defining read event at: %s %d", \
                             __FILE__, __LINE__);                                                \
        rev = PMIX_NEW(pmix_iof_read_event_t);                                                   \
        rev->fd = (fid);                                                                         \
        rev->always_readable = pmix_iof_fd_always_ready(fid);                                    \
        *(rv) = rev;                                                                             \
        if (rev->always_readable) {                                                              \
            pmix_event_evtimer_set(pmix_globals.evbase, &rev->ev, (cbfunc), rev);                \
        } else {                                                                                 \
            pmix_event_set(pmix_globals.evbase, &rev->ev, (fid), PMIX_EV_READ, (cbfunc), rev);   \
        }                                                                                        \
        if ((actv)) {                                                                            \
            PMIX_IOF_READ_ACTIVATE(rev)                                                          \
        }                                                                                        \
    } while (0);


/* Start reading our own stdin and forwarding it, arming the
 * foreground poll if the descriptor is a terminal we do not currently
 * own. Every role that forwards its
 * own stdin uses this - PMIx_IOF_push with PMIX_IOF_PUSH_STDIN, and a
 * tool given PMIX_FWD_STDIN at init - so that there is exactly one read
 * event on the descriptor and pmix_iof_flow_control can find it. Calling
 * it again once the stream is running is a no-op. "procs"/"directives"
 * may be NULL; they name who the stdin is destined for.
 *
 * pmix_iof_finalize() releases that read event and the foreground poll.
 * It must be called while the event base is still up - i.e. from
 * pmix_rte_finalize, not after it. */
PMIX_EXPORT pmix_status_t pmix_iof_setup_stdin_read(int fd, pmix_proc_t procs[], size_t nprocs,
                                                    pmix_info_t directives[], size_t ndirs);
PMIX_EXPORT void pmix_iof_finalize(void);

PMIX_EXPORT pmix_status_t pmix_iof_write_output(const pmix_proc_t *name, pmix_iof_channel_t stream,
                                                const pmix_byte_object_t *bo);
PMIX_EXPORT void pmix_iof_static_dump_output(pmix_iof_sink_t *sink);
PMIX_EXPORT void pmix_iof_write_handler(int fd, short event, void *cbdata);
PMIX_EXPORT bool pmix_iof_stdin_check(int fd);
PMIX_EXPORT void pmix_iof_read_local_handler(int unusedfd, short event, void *cbdata);
PMIX_EXPORT void pmix_iof_stdin_cb(int fd, short event, void *cbdata);
PMIX_EXPORT pmix_status_t pmix_iof_process_iof(pmix_iof_channel_t channels,
                                               const pmix_proc_t *source,
                                               const pmix_byte_object_t *bo,
                                               const pmix_info_t *info, size_t ninfo,
                                               pmix_iof_req_t *req);
PMIX_EXPORT void pmix_iof_init_flags(pmix_iof_flags_t *flags);
PMIX_EXPORT void pmix_iof_check_flags(pmix_info_t *info, pmix_iof_flags_t *flags);

/* Holding output until a spawn reply says how to format it.
 *
 * A tool receives forwarded output for the jobs it spawns, and that
 * output can beat the spawn reply here - so it can arrive naming a
 * namespace we have never been told about, with nothing in scope to say
 * which of our in-flight spawns it came from. pmix_iof_write_output()
 * holds such output rather than formatting it on a guess, and these are
 * the two ends of that arrangement.
 *
 * pmix_iof_spawn_begin() records a spawn as issued and unanswered; it
 * must be called before the request goes out, because the reply - and
 * the output - can arrive as soon as it has. pmix_iof_spawn_end() is its
 * partner and is owed by every path that ends the spawn, the failed send
 * included; when the last one is answered it releases anything still
 * held, since output left over belongs to no spawn of ours.
 *
 * pmix_iof_release_pending() writes out what was held for one namespace.
 * Call it once that namespace's formatting flags are in place and before
 * pmix_iof_spawn_end(), so the held output is formatted with the
 * directives its own spawn was issued with.
 *
 * begin() may be called from any thread; the other two are progress-
 * thread only, as is everything they touch. */
PMIX_EXPORT void pmix_iof_spawn_begin(void);
PMIX_EXPORT void pmix_iof_spawn_end(void);
PMIX_EXPORT void pmix_iof_release_pending(const char *nspace);

/* IOF flow control.
 *
 * pmix_iof_flow_control() is the one place that applies an XON/XOFF,
 * whatever asked for it - the host through PMIx_server_IOF_flow_control,
 * a push_stdin that completed with PMIX_ERR_IOF_XOFF, or an upstream
 * server through PMIX_PTL_TAG_IOF_CONTROL. It suspends or resumes any
 * stdin we are reading ourselves and, if we are a server, relays the
 * request to every peer that has pushed stdin to us. A launcher is both,
 * so a chain of them passes the request all the way back to whoever is
 * actually holding the input stream.
 *
 * "source" names the producer to control; NULL, or a wildcard rank
 * and/or nspace, means every producer feeding us. Only the stdin bit of
 * "channel" is meaningful - PMIX_ERR_NOT_SUPPORTED is returned if it is
 * not set. Nothing is buffered on behalf of a suspended stream.
 *
 * pmix_iof_flow_control_handler() is the PMIX_PTL_TAG_IOF_CONTROL recv
 * callback; it unpacks such a request and hands it to the above.
 */
PMIX_EXPORT pmix_status_t pmix_iof_flow_control(const pmix_proc_t *source,
                                                pmix_iof_channel_t channel,
                                                bool xoff,
                                                const pmix_info_t directives[], size_t ndirs);
PMIX_EXPORT void pmix_iof_flow_control_handler(struct pmix_peer_t *peer, pmix_ptl_hdr_t *hdr,
                                               pmix_buffer_t *buf, void *cbdata);

/* PMIX_IOF_FILE_PATTERN support.
 *
 * Without it, PMIX_IOF_OUTPUT_TO_FILE annotates the name it was given with
 * the namespace and rank: "<file>.<nspace>.<rank>.out".  With it, the name
 * is a pattern the caller controls, and these '%' conversions are expanded
 * in it (anything else beginning with '%' is an error):
 *
 *   %n  the job's namespace
 *   %r  the process's rank
 *   %R  the rank, zero-padded to the width of the job's largest rank
 *   %h  the hostname of the node writing the file
 *   %%  a literal '%'
 *
 * The stream suffix (".out"/".err") is still appended, so stdout and stderr
 * can never land on the same file. A pattern containing no '%' at all is
 * therefore simply a fixed name - which is what "not annotated" means, and
 * is the behavior this attribute had before the conversions existed.
 *
 * pmix_iof_check_pattern() reports whether a pattern is well formed without
 * expanding it, so a launcher can reject a bad one while the user is still
 * looking at their command line rather than at a failed job. On
 * PMIX_ERR_BAD_PARAM it sets *bad (if non-NULL) to a malloc'd copy of the
 * offending conversion for the diagnostic; the caller frees it.
 *
 * pmix_iof_expand_pattern() requires a non-NULL result; it returns the
 * expanded name there as a malloc'd string the caller frees, and
 * PMIX_ERR_BAD_PARAM if given no place to put it.
 */
PMIX_EXPORT pmix_status_t pmix_iof_check_pattern(const char *pattern, char **bad);
PMIX_EXPORT pmix_status_t pmix_iof_expand_pattern(const char *pattern,
                                                  const char *nspace,
                                                  pmix_rank_t rank,
                                                  int numdigs,
                                                  const char *suffix,
                                                  char **result);
PMIX_EXPORT void pmix_iof_flush_residuals(void);
PMIX_EXPORT pmix_byte_object_t* pmix_iof_prep_output(const pmix_proc_t *name,
                                                     pmix_iof_flags_t *myflags,
                                                     pmix_iof_channel_t stream,
                                                     const pmix_byte_object_t *bo);

END_C_DECLS

#endif /* PMIX_IOF_H */
