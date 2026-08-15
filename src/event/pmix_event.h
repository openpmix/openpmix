/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
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
 * Copyright (c) 2020      IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#ifndef PMIX_EVENT_H
#define PMIX_EVENT_H

#include "src/include/pmix_config.h"
#include "src/include/pmix_types.h"
#include <event.h>

#include "pmix_common.h"
#include "src/class/pmix_list.h"
#include "src/mca/bfrops/bfrops_types.h"
#include "src/threads/pmix_threads.h"
#include "src/util/pmix_output.h"

BEGIN_C_DECLS

#define PMIX_EVENT_ORDER_NONE          0x00
#define PMIX_EVENT_ORDER_FIRST         0x01
#define PMIX_EVENT_ORDER_LAST          0x02
#define PMIX_EVENT_ORDER_BEFORE        0x04
#define PMIX_EVENT_ORDER_AFTER         0x08
#define PMIX_EVENT_ORDER_PREPEND       0x10
#define PMIX_EVENT_ORDER_APPEND        0x20
#define PMIX_EVENT_ORDER_FIRST_OVERALL 0x40
#define PMIX_EVENT_ORDER_LAST_OVERALL  0x80

/* define an internal attribute for marking that the
 * server processed an event before passing it up
 * to its host in case it comes back down - avoids
 * infinite loop */
#define PMIX_SERVER_INTERNAL_NOTIFY "pmix.srvr.internal.notify"

/* Callback signature for a library-internal event observer.
 *
 * An observer is how PMIx itself watches an event it requires for its own
 * correctness. It is deliberately NOT an event handler: it has no return
 * value and no completion callback, so it cannot end the event chain, cannot
 * defer, and cannot alter the results the chain accumulates. Observers are
 * run - all of them, in registration order - before the application-visible
 * chain begins, so nothing an application handler does can suppress one.
 * See openpmix#4059 for why this exists.
 *
 * Four rules bind an observer, all of them because it runs inline on the
 * progress thread ahead of the chain:
 *
 * 1. It must not block. Bookkeeping, waking a lock, firing a caller's
 *    callback, posting a PMIX_THREADSHIFT, or issuing a *non-blocking*
 *    PMIx_Notify_event are all fine; PMIX_WAIT_THREAD and the blocking
 *    public APIs are not.
 * 2. It must not synchronously drive another event chain - that would
 *    re-enter the sweep.
 * 3. It must be idempotent. A cached notification replayed by
 *    check_cached_events() runs the whole delivery path again, so an
 *    observer can legitimately see the same event twice.
 * 4. It may deregister itself. Deregistration threadshifts, so the removal
 *    lands after the sweep has finished with the list.
 */
typedef void (*pmix_event_observer_fn_t)(pmix_status_t status,
                                         const pmix_proc_t *source,
                                         const pmix_info_t info[], size_t ninfo,
                                         const pmix_proc_t *affected, size_t naffected,
                                         void *cbobject);

/* define a struct for tracking registration ranges */
typedef struct {
    pmix_data_range_t range;
    pmix_proc_t *procs;
    size_t nprocs;
} pmix_range_trkr_t;

#define PMIX_RANGE_TRKR_STATIC_INIT     \
{                                       \
    .range = PMIX_RANGE_UNDEF,          \
    .procs = NULL,                      \
    .nprocs = 0                         \
}


/* define a common struct for tracking event handlers */
typedef struct {
    pmix_list_item_t super;
    char *name;
    size_t index;
    uint8_t precedence;
    bool oneshot;
    char *locator;
    pmix_proc_t source; // who generated this event
    /* When registering for events, callers can specify
     * the range of sources from which they are willing
     * to receive notifications - e.g., for callers to
     * define different handlers for events coming from
     * the RM vs those coming from their peers. We use
     * the rng field to track these values upon registration.
     */
    pmix_range_trkr_t rng;
    /* For registration, we use the affected field to track
     * the range of procs that, if affected by the event,
     * should cause the handler to be called (subject, of
     * course, to any rng constraints).
     */
    pmix_proc_t *affected;
    size_t naffected;
    pmix_notification_fn_t evhdlr;
    /* If obsfn is non-NULL this registration is a library-internal
     * observer rather than an event handler: it lives on the observers
     * list, is run ahead of the chain, and evhdlr is unused. relfn, if
     * given, releases cbobject when the registration goes away - which
     * lets the registry own the observer's tracker rather than making
     * every subsystem keep its own survivor list for finalize.
     */
    pmix_event_observer_fn_t obsfn;
    pmix_release_cbfunc_t relfn;
    void *cbobject;
    pmix_status_t *codes;
    size_t ncodes;
} pmix_event_hdlr_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_event_hdlr_t);

#define PMIX_EVENT_HDLR_STATIC_INIT         \
{                                           \
    .super = PMIX_LIST_ITEM_STATIC_INIT,    \
    .name = NULL,                           \
    .index = SIZE_MAX,                      \
    .precedence = UINT8_MAX,                \
    .oneshot = false,                       \
    .locator = NULL,                        \
    .source = PMIX_PROC_STATIC_INIT,        \
    .rng = PMIX_RANGE_TRKR_STATIC_INIT,     \
    .affected = NULL,                       \
    .naffected = 0,                         \
    .evhdlr = NULL,                         \
    .obsfn = NULL,                          \
    .relfn = NULL,                          \
    .cbobject = NULL,                       \
    .codes = NULL,                          \
    .ncodes = 0                             \
}

/* define an object for tracking status codes we are actively
 * registered to receive */
typedef struct {
    pmix_list_item_t super;
    pmix_status_t code;
    size_t nregs;
    void *peer; // (pmix_peer_t *)
} pmix_active_code_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_active_code_t);

/* define an object for housing the different lists of events
 * we have registered so we can easily scan them in precedent
 * order when we get an event */
typedef struct {
    pmix_object_t super;
    size_t nhdlrs;
    pmix_event_hdlr_t *first;
    pmix_event_hdlr_t *last;
    pmix_list_t actives;
    pmix_list_t single_events;
    pmix_list_t multi_events;
    pmix_list_t default_events;
    /* library-internal observers - not part of the chain, and not
     * reachable by anything the application can register */
    pmix_list_t observers;
} pmix_events_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_events_t);

#define PMIX_EVENTS_STATIC_INIT                     \
{                                                   \
    .super = PMIX_OBJ_STATIC_INIT(pmix_object_t),   \
    .nhdlrs = 0,                                    \
    .first = NULL,                                  \
    .last = NULL,                                   \
    .actives = PMIX_LIST_STATIC_INIT,               \
    .single_events = PMIX_LIST_STATIC_INIT,         \
    .multi_events = PMIX_LIST_STATIC_INIT,          \
    .default_events = PMIX_LIST_STATIC_INIT,        \
    .observers = PMIX_LIST_STATIC_INIT              \
}

/* define an object for chaining event notifications thru
 * the local state machine. Each registered event handler
 * that asked to be notified for a given code is given a
 * chance to "see" the reported event, starting with
 * single-code handlers, then multi-code handlers, and
 * finally default handlers. This object provides a
 * means for us to relay the event across that chain
 */
typedef struct pmix_event_chain_t {
    pmix_list_item_t super;
    pmix_status_t status;
    pmix_event_t ev;
    bool timer_active;
    bool nondefault;
    bool endchain;
    bool cached;
    pmix_proc_t source;
    pmix_data_range_t range;
    /* When generating events, callers can specify
     * the range of targets to receive notifications.
     */
    pmix_proc_t *targets;
    size_t ntargets;
    /* the processes that we affected by the event */
    pmix_proc_t *affected;
    size_t naffected;
    /* any info provided by the event generator */
    pmix_info_t *info;
    size_t ninfo;
    size_t nallocated;
    pmix_status_t interim_status;
    pmix_info_t *results;
    size_t nresults;
    pmix_info_t *interim;
    size_t ninterim;
    pmix_event_hdlr_t *evhdlr;
    pmix_op_cbfunc_t opcbfunc;
    void *cbdata;
    pmix_op_cbfunc_t final_cbfunc;
    void *final_cbdata;
} pmix_event_chain_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_event_chain_t);

typedef struct {
    pmix_object_t super;
    volatile bool active;
    pmix_event_t ev;
    pmix_lock_t lock;
    pmix_status_t status;
    size_t index;
    bool firstoverall;
    bool enviro;
    pmix_list_t *list;
    pmix_event_hdlr_t *hdlr;
    void *cd;
    pmix_status_t *codes;
    size_t ncodes;
    pmix_info_t *info;
    size_t ninfo;
    pmix_proc_t *affected;
    size_t naffected;
    pmix_notification_fn_t evhdlr;
    /* Internal-observer registrations pass no info array - there is no
     * caller to hold one alive across the threadshift - so the name and
     * return object they would otherwise carry as directives ride here
     * instead. A non-NULL obsfn is what marks the caddy as an observer.
     */
    pmix_event_observer_fn_t obsfn;
    pmix_release_cbfunc_t relfn;
    void *cbobject;
    char *name;
    pmix_hdlr_reg_cbfunc_t evregcbfn;
    void *cbdata;
} pmix_rshift_caddy_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_rshift_caddy_t);

/* prepare a chain for processing by cycling across provided
 * info structs and translating those supported by the event
 * system into the chain object*/
PMIX_EXPORT pmix_status_t pmix_prep_event_chain(pmix_event_chain_t *chain, const pmix_info_t *info,
                                                size_t ninfo, bool xfer);

/* invoke the error handler that is registered against the given
 * status, passing it the provided info on the procs that were
 * affected, plus any additional info provided by the server */
PMIX_EXPORT void pmix_invoke_local_event_hdlr(pmix_event_chain_t *chain);

/* The completion of a chain built from an event our server forwarded to
 * us: park the event if nothing accepted it, then release the chain.
 * pmix_client_notify_recv hangs it on chain->final_cbfunc with the chain
 * itself as final_cbdata.
 *
 * It lives here rather than in src/client because parking an event is
 * event-layer business, and because the tool's receive path used to
 * carry its own copy of it - reachable from nowhere, since a tool hands
 * its forwarded events to pmix_server_notify_client_of_event and never
 * walks a handler list against the chain it built (see the comment on
 * release_chain in src/tool/pmix_tool.c, and openpmix#4101). */
PMIX_EXPORT void pmix_event_notify_complete(pmix_status_t status, void *cbdata);

PMIX_EXPORT bool pmix_notify_check_range(pmix_range_trkr_t *rng, const pmix_proc_t *proc);

PMIX_EXPORT bool pmix_notify_check_affected(pmix_proc_t *interested, size_t ninterested,
                                            pmix_proc_t *affected, size_t naffected);

PMIX_EXPORT pmix_status_t pmix_deregister_event_hdlr(size_t event_hdlr_ref,
                                                     pmix_buffer_t *msg);

/* Register a library-internal observer for the given codes. Read the
 * contract on pmix_event_observer_fn_t before writing one.
 *
 * At least one code must be given - there is no "default" observer, as an
 * observer that wanted every event would be watching for something it has
 * no business deciding. The name is used only in debug output. cbobject is
 * handed back to the observer on every callback; if relfn is non-NULL the
 * registry calls it on cbobject when the registration is torn down, either
 * by deregistration or at finalize.
 *
 * This is non-blocking by design: it may be called from the progress
 * thread (registering a watch from inside an event handler is the common
 * case), where waiting for the registration to complete would deadlock.
 * cbfunc reports the outcome and the observer id used to deregister.
 */
PMIX_EXPORT pmix_status_t pmix_event_register_observer(const char *name,
                                                       const pmix_status_t codes[], size_t ncodes,
                                                       pmix_event_observer_fn_t obsfn,
                                                       void *cbobject,
                                                       pmix_release_cbfunc_t relfn,
                                                       pmix_hdlr_reg_cbfunc_t cbfunc,
                                                       void *cbdata);

/* Remove an observer registration. Non-blocking for the same reason;
 * cbfunc may be NULL if the caller does not need to know when it is gone. */
PMIX_EXPORT pmix_status_t pmix_event_deregister_observer(size_t obsid,
                                                         pmix_op_cbfunc_t cbfunc,
                                                         void *cbdata);

/* invoke the server event notification handler */
PMIX_EXPORT pmix_status_t pmix_server_notify_client_of_event(pmix_status_t status,
                                                             const pmix_proc_t *source,
                                                             pmix_data_range_t range,
                                                             const pmix_info_t info[], size_t ninfo,
                                                             pmix_op_cbfunc_t cbfunc, void *cbdata);
PMIX_EXPORT pmix_status_t pmix_notify_server_of_event(pmix_status_t status, const pmix_proc_t *source,
                                                      pmix_data_range_t range, const pmix_info_t info[],
                                                      size_t ninfo, pmix_op_cbfunc_t cbfunc, void *cbdata,
                                                      bool dolocal);

PMIX_EXPORT void pmix_event_timeout_cb(int sd, short args, void *cbdata);

PMIX_EXPORT void pmix_internal_notify_event(int sd, short args, void *cbdata);

PMIX_EXPORT void pmix_internal_reg_event_hdlr(int sd, short args, void *cbdata);

/* The deregistration counterpart, exposed for the same reason its sibling
 * is: PMIx_Init registers the debugger-wait handler by threadshifting to
 * pmix_internal_reg_event_hdlr directly, because it runs before
 * pmix_globals.initialized is set and the public entry points gate on it.
 * Anything that has to take such a registration back before init completes
 * must threadshift to this the same way. The caddy is a
 * pmix_shift_caddy_t whose ref carries the handler index; this releases it
 * after invoking cbfunc.opcbfn, so the poster must not. */
PMIX_EXPORT void pmix_internal_dereg_event_hdlr(int sd, short args, void *cbdata);

#define PMIX_REPORT_EVENT(e, p, r, f)                                                          \
    do {                                                                                       \
        pmix_event_chain_t *ch, *cp;                                                           \
        size_t _n;                                                                             \
                                                                                               \
        ch = NULL;                                                                             \
        /* see if we already have this event cached */                                         \
        PMIX_LIST_FOREACH (cp, &pmix_globals.cached_events, pmix_event_chain_t) {              \
            if (cp->status == (e)) {                                                           \
                ch = cp;                                                                       \
                break;                                                                         \
            }                                                                                  \
        }                                                                                      \
        if (NULL == ch) {                                                                      \
            /* nope - need to add it */                                                        \
            ch = PMIX_NEW(pmix_event_chain_t);                                                 \
            ch->status = (e);                                                                  \
            ch->range = (r);                                                                   \
            PMIX_LOAD_PROCID(&ch->source, (p)->nptr->nspace, (p)->info->pname.rank);           \
            PMIX_PROC_CREATE(ch->affected, 1);                                                 \
            ch->naffected = 1;                                                                 \
            PMIX_LOAD_PROCID(ch->affected, (p)->nptr->nspace, (p)->info->pname.rank);          \
            /* if this is lost-connection-to-server, then we let it go to */                   \
            /* the default event handler - otherwise, we don't */                              \
            if (PMIX_ERR_LOST_CONNECTION != (e) && PMIX_ERR_UNREACH != (e)) {                  \
                ch->ninfo = 1;                                                                 \
                ch->nallocated = 3;                                                            \
                PMIX_INFO_CREATE(ch->info, ch->nallocated);                                    \
                /* mark for non-default handlers only */                                       \
                PMIX_INFO_LOAD(&ch->info[0], PMIX_EVENT_NON_DEFAULT, NULL, PMIX_BOOL);         \
            } else {                                                                           \
                ch->nallocated = 2;                                                            \
                PMIX_INFO_CREATE(ch->info, ch->nallocated);                                    \
            }                                                                                  \
            ch->final_cbfunc = (f);                                                            \
            ch->final_cbdata = ch;                                                             \
            /* cache it */                                                                     \
            pmix_list_append(&pmix_globals.cached_events, &ch->super);                         \
            ch->timer_active = true;                                                           \
            pmix_event_assign(&ch->ev, pmix_globals.evbase, -1, 0, pmix_event_timeout_cb, ch); \
            PMIX_POST_OBJECT(ch);                                                              \
            pmix_event_add(&ch->ev, &pmix_globals.event_window);                               \
        } else {                                                                               \
            /* add this peer to the array of sources */                                        \
            pmix_proc_t proc_tmp;                                                              \
            pmix_info_t *info_tmp;                                                             \
            size_t ninfo_tmp;                                                                  \
            pmix_strncpy(proc_tmp.nspace, (p)->nptr->nspace, PMIX_MAX_NSLEN);                  \
            proc_tmp.rank = (p)->info->pname.rank;                                             \
            ninfo_tmp = ch->nallocated + 1;                                                    \
            PMIX_INFO_CREATE(info_tmp, ninfo_tmp);                                             \
            /* must keep the hdlr name and return object at the end, so prepend */             \
            PMIX_INFO_LOAD(&info_tmp[0], PMIX_PROCID, &proc_tmp, PMIX_PROC);                   \
            for (_n = 0; _n < ch->ninfo; _n++) {                                               \
                PMIX_INFO_XFER(&info_tmp[_n + 1], &ch->info[_n]);                              \
            }                                                                                  \
            PMIX_INFO_FREE(ch->info, ch->nallocated);                                          \
            ch->nallocated = ninfo_tmp;                                                        \
            ch->info = info_tmp;                                                               \
            ch->ninfo = ninfo_tmp - 2;                                                         \
            /* reset the timer */                                                              \
            if (ch->timer_active) {                                                            \
                pmix_event_del(&ch->ev);                                                       \
            }                                                                                  \
            PMIX_POST_OBJECT(ch);                                                              \
            ch->timer_active = true;                                                           \
            pmix_event_add(&ch->ev, &pmix_globals.event_window);                               \
        }                                                                                      \
    } while (0)

END_C_DECLS

#endif /* PMIX_EVENT_H */
