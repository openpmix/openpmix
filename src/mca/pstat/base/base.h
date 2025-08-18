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
 * Copyright (c) 2021-2025 Nanook Consulting  All rights reserved.
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


typedef struct {
	pmix_list_item_t super;
    pmix_proc_t requestor;
	char *id;
	pmix_event_t ev;
	struct timeval tv;
	bool active;
	uint32_t rate;
	pmix_status_t eventcode;
	pmix_list_t peers;
	char **disks;
	char **nets;
	pmix_procstats_t pstats;
	pmix_dkstats_t dkstats;
	pmix_netstats_t netstats;
	pmix_ndstats_t ndstats;
    pmix_cb_t *cb;
} pmix_pstat_op_t;
PMIX_CLASS_DECLARATION(pmix_pstat_op_t);

// p - pmix_pstat_op_t*
// s - time in seconds
// cb - callback function that will execute the collection
#define PMIX_PSTAT_OP_START(p, s, cb)                                           \
    do {                                                                        \
        pmix_event_evtimer_set(pmix_pstat_base.evbase, &(p)->ev, (cb), (p));    \
        (p)->tv.tv_sec = (s);                                                   \
        (p)->tv.tv_usec = 0;                                                    \
        PMIX_OUTPUT_VERBOSE((1, pmix_pstat_base_framework.framework_output,     \
                             "defining pstat event: %ld sec at %s:%d",          \
                             (long) (p)->tv.tv_sec, __FILE__, __LINE__));       \
        PMIX_POST_OBJECT(p);                                                    \
        (p)->active = true;                                                     \
        pmix_event_evtimer_add(&(p)->ev, &(p)->tv);                             \
    } while (0);

#define PMIX_PSTAT_APPEND_PEER_UNIQUE(pl, pr) 					\
    do {														\
		bool f = false;											\
		pmix_peerlist_t *_p;									\
		PMIX_LIST_FOREACH(_p, pl, pmix_peerlist_t) {			\
			if (_p->peer == (pr)) {								\
				f = true;  										\
				break;  										\
			}  													\
		}  														\
		if (!f) { 												\
			_p = PMIX_NEW(pmix_peerlist_t);  					\
			_p->peer = (pr);									\
			pmix_list_append(pl, &(_p->super));  				\
		} 														\
	} while(0)

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
