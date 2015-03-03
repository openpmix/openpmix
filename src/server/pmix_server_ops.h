/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2015      Intel, Inc. All rights reserved
 * Copyright (c) 2015      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * $COPYRIGHT$
 */

#ifndef PMIX_SERVER_OPS_H
#define PMIX_SERVER_OPS_H

#include "pmix_config.h"
#include "src/api/pmix_server.h"
#include "src/usock/usock.h"

typedef struct {
    pmix_object_t super;
    int sd;
    pmix_send_message_cbfunc_t cbfunc;
} pmix_snd_caddy_t;
PMIX_CLASS_DECLARATION(pmix_snd_caddy_t);

typedef struct {
    pmix_list_item_t super;
    pmix_usock_hdr_t hdr;
    pmix_peer_t *peer;
    pmix_snd_caddy_t snd;
} pmix_server_caddy_t;
PMIX_CLASS_DECLARATION(pmix_server_caddy_t);

typedef struct {
    pmix_list_item_t super;
    pmix_nspace_t *nptr;
    char nspace[PMIX_MAX_NSLEN];  // have to cache the nspace in case we don't know about it yet
    int *ranks;
    size_t nranks;
} pmix_range_trkr_t;
PMIX_CLASS_DECLARATION(pmix_range_trkr_t);

typedef struct {
    pmix_list_item_t super;
    volatile bool active;   // flag for waiting for completion
    bool def_complete;      // all ranges have been recorded and the trk definition is complete
    pmix_list_t ranges;     // list of pmix_range_trkr_t identifying the participating ranges
    pmix_list_t locals;     // list of pmix_server_caddy_t identifying the local participants
    uint32_t local_cnt;     // number of local participants
    pmix_list_t *trklist;   // the list this tracker is attached to
} pmix_server_trkr_t;
PMIX_CLASS_DECLARATION(pmix_server_trkr_t);

typedef struct {
    pmix_object_t super;
    pmix_event_t ev;
    volatile bool active;
    char nspace[PMIX_MAX_NSLEN];
    int rank;
    uid_t uid;
    gid_t gid;
    void *server_object;
} pmix_setup_caddy_t;
PMIX_CLASS_DECLARATION(pmix_setup_caddy_t);

typedef struct {
    pmix_list_t nspaces;
    pmix_pointer_array_t clients;
    pmix_list_t fence_ops;
    pmix_list_t connect_ops;
    pmix_list_t disconnect_ops;
} pmix_server_globals_t;

#define PMIX_PEER_CADDY(c, p, t)                \
    do {                                        \
        (c) = PMIX_NEW(pmix_server_caddy_t);    \
        (c)->hdr.tag = (t);                     \
        PMIX_RETAIN((p));                       \
        (c)->peer = (p);                        \
    } while(0);

#define PMIX_SND_CADDY(c, h, s)                                         \
    do {                                                                \
        (c) = PMIX_NEW(pmix_server_caddy_t);                             \
        (void)memcpy(&(c)->hdr, &(h), sizeof(pmix_usock_hdr_t));        \
        PMIX_RETAIN((s));                                                \
        (c)->snd = (s);                                                 \
    } while(0);

pmix_status_t pmix_server_authenticate(int sd, pmix_peer_t **peer,
                                       pmix_buffer_t **reply);

int pmix_server_abort(pmix_peer_t *peer, pmix_buffer_t *buf,
                      pmix_op_cbfunc_t cbfunc, void *cbdata);

int pmix_server_fence(pmix_server_caddy_t *cd,
                      pmix_buffer_t *buf,
                      pmix_modex_cbfunc_t modexcbfunc,
                      pmix_op_cbfunc_t opcbfunc);

int pmix_server_get(pmix_buffer_t *buf,
                    pmix_modex_cbfunc_t cbfunc, void *cbdata);

int pmix_server_publish(pmix_buffer_t *buf,
                        pmix_op_cbfunc_t cbfunc, void *cbdata);

int pmix_server_lookup(pmix_buffer_t *buf,
                       pmix_lookup_cbfunc_t cbfunc, void *cbdata);

int pmix_server_unpublish(pmix_buffer_t *buf,
                          pmix_op_cbfunc_t cbfunc, void *cbdata);

int pmix_server_spawn(pmix_buffer_t *buf,
                      pmix_spawn_cbfunc_t cbfunc,
                      void *cbdata);

int pmix_server_connect(pmix_server_caddy_t *cd,
                        pmix_buffer_t *buf, bool disconnect,
                        pmix_op_cbfunc_t cbfunc);

extern pmix_server_module_t pmix_host_server;
extern pmix_server_globals_t pmix_server_globals;

#endif // PMIX_SERVER_OPS_H
