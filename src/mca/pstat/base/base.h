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
 * Copyright (c) 2007-2020 Cisco Systems, Inc.  All rights reserved
 * Copyright (c) 2019      Intel, Inc.  All rights reserved.
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#ifndef PMIX_PSTAT_BASE_H
#define PMIX_PSTAT_BASE_H

#include "pmix_config.h"
#include "src/include/pmix_globals.h"
#include "src/include/pmix_types.h"
#include "src/mca/base/pmix_mca_base_framework.h"
#include "src/mca/pstat/pstat.h"

/*
 * Global functions for MCA overall pstat open and close
 */

BEGIN_C_DECLS

/**
 * Framework structure declaration for this framework
 */
PMIX_EXPORT extern pmix_mca_base_framework_t pmix_pstat_base_framework;

/**
 * Select an available component.
 *
 * @return PMIX_SUCCESS Upon success.
 * @return PMIX_NOT_FOUND If no component can be selected.
 * @return PMIX_ERROR Upon other failure.
 *
 * At the end of this process, we'll either have a single
 * component that is selected and initialized, or no component was
 * selected.  If no component was selected, subsequent invocation
 * of the pstat functions will return an error indicating no data
 * could be obtained
 */
PMIX_EXPORT int pmix_pstat_base_select(void);

PMIX_EXPORT extern pmix_pstat_base_component_t *pmix_pstat_base_component;

typedef struct {
    pmix_event_base_t *evbase;
    pmix_list_t ops;
} pmix_pstat_base_t;

PMIX_EXPORT extern pmix_pstat_base_t pmix_pstat_base;

/* A peer has left this server: the periodic monitors it asked for end
 * with it (once no clone of it under the same name is still connected).
 * Called on the progress thread by the connection teardown; a no-op
 * unless the framework is open. */
PMIX_EXPORT void pmix_pstat_base_peer_lost(struct pmix_peer_t *peer);

typedef struct {
    bool cmdline;
    bool pctcpu;
    bool state;
    bool time;
    bool pri;
    bool nthreads;
    bool cpu;
    bool vsize;
    bool pkvsize;
    bool rss;
    bool pss;
} pmix_procstats_t;
#define PMIX_PROCSTATS_INIT(a) \
    memset(a, 0, sizeof(pmix_procstats_t))

#define PMIX_PROCSTATS_ALL(a) \
    memset(a, 1, sizeof(pmix_procstats_t))

typedef struct {
    bool rdcompleted;
    bool rdmerged;
    bool rdsectors;
    bool rdms;
    bool wrtcompleted;
    bool wrtmerged;
    bool wrtsectors;
    bool wrtms;
    bool ioprog;
    bool ioms;
    bool ioweight;
} pmix_dkstats_t;
#define PMIX_DKSTATS_INIT(a) \
    memset(a, 0, sizeof(pmix_dkstats_t))

#define PMIX_DKSTATS_ALL(a) \
    memset(a, 1, sizeof(pmix_dkstats_t))

typedef struct {
    bool rcvdb;
    bool rcvdp;
    bool rcvde;
    bool sntb;
    bool sntp;
    bool snte;
} pmix_netstats_t;
#define PMIX_NETSTATS_INIT(a) \
    memset(a, 0, sizeof(pmix_netstats_t))

#define PMIX_NETSTATS_ALL(a) \
    memset(a, 1, sizeof(pmix_netstats_t))

typedef struct {
    bool la;
    bool la5;
    bool la15;
    bool mtot;
    bool mfree;
    bool mbuf;
    bool mcached;
    bool mswapcached;
    bool mswaptot;
    bool mswapfree;
    bool mmap;
} pmix_ndstats_t;
#define PMIX_NDSTATS_INIT(a) \
    memset(a, 0, sizeof(pmix_ndstats_t))

#define PMIX_NDSTATS_ALL(a) \
    memset(a, 1, sizeof(pmix_ndstats_t))


/* One process, or set of processes, a monitor samples. A process is known
 * by its pid - the one the host recorded for a PMIx process it launched
 * (PMIX_PROC_PID), or one the requester named - and it is read from the
 * kernel. Nothing the process said about itself is used. */
typedef struct {
    pmix_list_item_t super;
    bool named;         // a PMIx process: name is valid
    pmix_proc_t name;
    pid_t pid;          // -1: every process of owner on this node
    uid_t owner;        // the uid the process must belong to when sampled;
                        // (uid_t) -1: any (a PMIx process whose job has no
                        // recorded owner)
} pmix_pstat_target_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_pstat_target_t);

typedef struct {
    pmix_list_item_t super;
    pmix_proc_t requestor;
    char *id;
    pmix_event_t ev;
    struct timeval tv;
    bool active;
    uint32_t rate;
    pmix_status_t eventcode;
    pmix_list_t targets;  // pmix_pstat_target_t: the processes to sample
    char **disks;
    char **nets;
    pmix_procstats_t pstats;
    pmix_dkstats_t dkstats;
    pmix_netstats_t netstats;
    pmix_ndstats_t ndstats;
    pmix_cb_t *cb;
} pmix_pstat_op_t;
PMIX_EXPORT PMIX_CLASS_DECLARATION(pmix_pstat_op_t);

// p - pmix_pstat_op_t*
// s - time in seconds
// cb - callback function that will execute the collection
/* The timer is PERSISTENT, and that is a correctness requirement rather
 * than a convenience. A repeating timer can be built either way - the
 * sampler re-arming a one-shot on its way out, or libevent re-arming a
 * persistent one - and the two are not equivalent to the thread that
 * wants to tear the op down.
 *
 * opdes() ends an op with pmix_event_del(). Called from a thread that is
 * not the event base's own - which is exactly what a PMIX_MONITOR_CANCEL
 * is when pstat_base_use_separate_thread is set - that waits for a
 * callback already running on the event before it returns, so the op is
 * safe to free afterwards. It waits, but it only removes the event once,
 * up front. With a one-shot timer the sampler's re-arm runs *after* that
 * removal and outside the base's lock, so event_del returned to a freshly
 * armed timer and the op was freed with a live timer pointing at it.
 * With EV_PERSIST the re-arm is event_persist_closure()'s, made while it
 * still holds the base lock and before the callback is entered, so a
 * concurrent event_del either takes the lock first and removes the
 * pending timeout, or takes it afterwards and removes the re-armed one.
 * Every interleaving returns disarmed.
 *
 * So: do not re-arm from a sampler, and do not "simplify" this back to
 * pmix_event_evtimer_set(), which asks for flags 0. */
#define PMIX_PSTAT_OP_START(p, s, cb)                                           \
    do {                                                                        \
        pmix_event_assign(&(p)->ev, pmix_pstat_base.evbase, -1,                 \
                          PMIX_EV_PERSIST, (event_callback_fn) (cb), (p));      \
        (p)->tv.tv_sec = (s);                                                   \
        (p)->tv.tv_usec = 0;                                                    \
        PMIX_OUTPUT_VERBOSE((1, pmix_pstat_base_framework.framework_output,     \
                             "defining pstat event: %ld sec at %s:%d",          \
                             (long) (p)->tv.tv_sec, __FILE__, __LINE__));       \
        PMIX_POST_OBJECT(p);                                                    \
        (p)->active = true;                                                     \
        pmix_event_add(&(p)->ev, &(p)->tv);                                     \
    } while (0)

/* Build the processes a PMIX_MONITOR_PROC_RESOURCE_USAGE request covers,
 * from its directives, applying who may see what:
 *   - PMIX_MONITOR_TARGET_PROCS: PMIx processes of this node, by the pid
 *     the host recorded for each (PMIX_PROC_PID; a process with none is
 *     skipped). The requester must be allowed each job it names;
 *   - PMIX_MONITOR_TARGET_PIDS: processes of this node, which the
 *     requester must own (as the kernel reports it). A pid of -1 is every
 *     process with the requester's uid;
 *   - neither: every PMIx process of this node with a recorded pid, in the
 *     jobs the requester may access.
 * The host is the requester for a request that is not from one of our
 * clients or tools, and is not restricted - see
 * pmix_server_access_requester(). Named targets the requester may not see
 * fail the request with PMIX_ERR_NO_PERMISSIONS. */
PMIX_EXPORT pmix_status_t pmix_pstat_base_targets(const pmix_proc_t *requestor,
                                                  const pmix_info_t directives[], size_t ndirs,
                                                  pmix_list_t *targets);

/* The uid that owns process pid, as the kernel reports it. Returns
 * PMIX_ERR_NOT_FOUND for no such process, PMIX_ERR_NOT_SUPPORTED where
 * this platform offers no way to ask */
PMIX_EXPORT pmix_status_t pmix_pstat_base_pid_owner(pid_t pid, uid_t *owner);

/* A component's sampler for one process: add its statistics to answer.
 * PMIX_ERR_NOT_FOUND means the process is gone, or is not the one the
 * target means - skipped, not an error */
typedef pmix_status_t (*pmix_pstat_base_sample_fn_t)(void *answer,
                                                     const pmix_pstat_target_t *tgt,
                                                     pid_t pid, pmix_procstats_t *pst);

/* Sample every process an op covers, with the component's sampler. A
 * target of every process of a uid is looked up afresh each time, as
 * processes come and go */
PMIX_EXPORT pmix_status_t pmix_pstat_base_sample_targets(void *answer, pmix_pstat_op_t *op,
                                                         pmix_pstat_base_sample_fn_t fn);

/* Every process on this node owned by uid. The caller frees *pids */
PMIX_EXPORT pmix_status_t pmix_pstat_base_pids_of(uid_t uid, pid_t **pids, size_t *npids);

PMIX_EXPORT void pmix_pstat_parse_procstats(pmix_procstats_t *pst,
                                            pmix_info_t *info, size_t sz);

PMIX_EXPORT void pmix_pstat_parse_dkstats(char ***disks, pmix_dkstats_t *dkst,
                                          pmix_info_t *info, size_t sz);

PMIX_EXPORT void pmix_pstat_parse_netstats(char ***nets, pmix_netstats_t *netst,
                                           pmix_info_t *info, size_t sz);

PMIX_EXPORT void pmix_pstat_parse_ndstats(pmix_ndstats_t *ndst,
                                          pmix_info_t *info, size_t sz);

END_C_DECLS

#endif /* PMIX_BASE_PSTAT_H */
