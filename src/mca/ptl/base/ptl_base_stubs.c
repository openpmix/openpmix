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
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <stdio.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/include/pmix_globals.h"

#include "src/mca/ptl/base/base.h"

bool pmix_ptl_base_peer_is_earlier(pmix_peer_t *peer, uint8_t major,
                                   uint8_t minor, uint8_t release)
{
    /* if they don't care, then don't check */
    if (PMIX_MAJOR_WILDCARD != major) {
        if (PMIX_PEER_MAJOR_VERSION(peer) == PMIX_MAJOR_WILDCARD) {
            /* we don't know what it is - assume earlier */
            return true;
        }
        if (PMIX_PEER_MAJOR_VERSION(peer) > major) {
            return false;
        }
        if (PMIX_PEER_MAJOR_VERSION(peer) < major) {
            return true;
        }
    }
    /* major value must be equal, so check minor */
    if (PMIX_MINOR_WILDCARD != minor) {
        if (PMIX_PEER_MINOR_VERSION(peer) == PMIX_MINOR_WILDCARD) {
            /* we don't know what it is - assume earlier */
            return true;
        }
        if (PMIX_PEER_MINOR_VERSION(peer) > minor) {
            return false;
        }
        if (PMIX_PEER_MINOR_VERSION(peer) < minor) {
            return true;
        }
    }
    /* major and minor must be equal - check release */
    if (PMIX_RELEASE_WILDCARD != release) {
        if (PMIX_PEER_REL_VERSION(peer) == PMIX_RELEASE_WILDCARD) {
            /* we don't know what it is - assume earlier */
            return true;
        }
        if (PMIX_PEER_REL_VERSION(peer) > release) {
            return false;
        }
        if (PMIX_PEER_REL_VERSION(peer) < release) {
            return true;
        }
    }
    /* must be equal */
    return false;
}

pmix_status_t pmix_ptl_base_setup_fork(const pmix_proc_t *proc, char ***env)
{
    pmix_ptl_base_active_t *active;
    pmix_status_t rc;

    if (!pmix_ptl_globals.initialized) {
        return PMIX_ERR_INIT;
    }

    PMIX_LIST_FOREACH(active, &pmix_ptl_globals.actives, pmix_ptl_base_active_t) {
        if (NULL != active->component->setup_fork) {
            rc = active->component->setup_fork(proc, env);
            if (PMIX_SUCCESS != rc && PMIX_ERR_NOT_AVAILABLE != rc) {
                return rc;
            }
        }
    }

    if (NULL != pmix_ptl_globals.session_tmpdir) {
        pmix_setenv("PMIX_SERVER_TMPDIR", pmix_ptl_globals.session_tmpdir, true, env);
    }

    if (NULL != pmix_ptl_globals.system_tmpdir) {
        pmix_setenv("PMIX_SYSTEM_TMPDIR", pmix_ptl_globals.system_tmpdir, true, env);
    }

    return PMIX_SUCCESS;
}

pmix_status_t pmix_ptl_base_set_notification_cbfunc(pmix_ptl_cbfunc_t cbfunc)
{
    pmix_ptl_posted_recv_t *req;

    /* post a persistent recv for the special 0 tag so the client can recv
     * error notifications from the server */
    req = PMIX_NEW(pmix_ptl_posted_recv_t);
    if (NULL == req) {
        return PMIX_ERR_NOMEM;
    }
    req->tag = 0;
    req->cbfunc = cbfunc;
    pmix_output_verbose(5, pmix_ptl_base_framework.framework_output,
                        "posting notification recv on tag %d", req->tag);
    /* add it to the list of recvs - we cannot have unexpected messages
     * in this subsystem as the server never sends us something that
     * we didn't previously request */
    pmix_list_prepend(&pmix_ptl_globals.posted_recvs, &req->super);
    return PMIX_SUCCESS;
}

char* pmix_ptl_base_get_available_modules(void)
{
    pmix_ptl_base_active_t *active;
    char **tmp=NULL, *reply=NULL;

    if (!pmix_ptl_globals.initialized) {
        return NULL;
    }

    PMIX_LIST_FOREACH(active, &pmix_ptl_globals.actives, pmix_ptl_base_active_t) {
        pmix_argv_append_nosize(&tmp, active->component->base.pmix_mca_component_name);
    }
    if (NULL != tmp) {
        reply = pmix_argv_join(tmp, ',');
        pmix_argv_free(tmp);
    }
    return reply;
}

/* return the highest priority module */
pmix_ptl_module_t* pmix_ptl_base_assign_module(void)
{
    pmix_ptl_base_active_t *active;

    if (!pmix_ptl_globals.initialized) {
        return NULL;
    }

    active = (pmix_ptl_base_active_t*)pmix_list_get_first(&pmix_ptl_globals.actives);
    return active->module;
}

pmix_status_t pmix_ptl_base_connect_to_peer(struct pmix_peer_t *peer,
                                            pmix_info_t info[], size_t ninfo)
{
    pmix_peer_t *pr = (pmix_peer_t*)peer;
    pmix_ptl_base_active_t *active;

    PMIX_LIST_FOREACH(active, &pmix_ptl_globals.actives, pmix_ptl_base_active_t) {
        if (NULL != active->module->connect_to_peer) {
            if (PMIX_SUCCESS == active->module->connect_to_peer(peer, info, ninfo)) {
                pr->nptr->compat.ptl = active->module;
                return PMIX_SUCCESS;
            }
        }
    }

    return PMIX_ERR_UNREACH;
}


static void post_recv(int fd, short args, void *cbdata)
{
    (void)fd;
    (void)args;
    pmix_ptl_posted_recv_t *req = (pmix_ptl_posted_recv_t*)cbdata;
    pmix_ptl_recv_t *msg, *nmsg;
    pmix_buffer_t buf;

    pmix_output_verbose(5, pmix_ptl_base_framework.framework_output,
                        "posting recv on tag %d", req->tag);

    /* add it to the list of recvs */
    pmix_list_append(&pmix_ptl_globals.posted_recvs, &req->super);

    /* now check the unexpected msg queue to see if we already
     * recvd something for it */
    PMIX_LIST_FOREACH_SAFE(msg, nmsg, &pmix_ptl_globals.unexpected_msgs, pmix_ptl_recv_t) {
        if (msg->hdr.tag == req->tag || UINT_MAX == req->tag) {
            if (NULL != req->cbfunc) {
                /* construct and load the buffer */
                PMIX_CONSTRUCT(&buf, pmix_buffer_t);
                if (NULL != msg->data) {
                    buf.base_ptr = (char*)msg->data;
                    buf.bytes_allocated = buf.bytes_used = msg->hdr.nbytes;
                    buf.unpack_ptr = buf.base_ptr;
                    buf.pack_ptr = ((char*)buf.base_ptr) + buf.bytes_used;
                }
                msg->data = NULL;  // protect the data region
                req->cbfunc(msg->peer, &msg->hdr, &buf, req->cbdata);
                PMIX_DESTRUCT(&buf);  // free's the msg data
            }
            pmix_list_remove_item(&pmix_ptl_globals.unexpected_msgs, &msg->super);
            PMIX_RELEASE(msg);
        }
    }
}

pmix_status_t pmix_ptl_base_register_recv(struct pmix_peer_t *peer,
                                          pmix_ptl_cbfunc_t cbfunc,
                                          pmix_ptl_tag_t tag)
{
    (void)peer;
    pmix_ptl_posted_recv_t *req;

    req = PMIX_NEW(pmix_ptl_posted_recv_t);
    if (NULL == req) {
        return PMIX_ERR_NOMEM;
    }
    req->tag = tag;
    req->cbfunc = cbfunc;
    /* have to push this into an event so we can add this
     * to the list of posted recvs */
    pmix_event_assign(&(req->ev), pmix_globals.evbase, -1,
                      EV_WRITE, post_recv, req);
    pmix_event_active(&(req->ev), EV_WRITE, 1);
    return PMIX_SUCCESS;
}

static void cancel_recv(int fd, short args, void *cbdata)
{
    (void)fd;
    (void)args;
    pmix_ptl_posted_recv_t *req = (pmix_ptl_posted_recv_t*)cbdata;
    pmix_ptl_posted_recv_t *rcv;

    PMIX_LIST_FOREACH(rcv, &pmix_ptl_globals.posted_recvs, pmix_ptl_posted_recv_t) {
        if (rcv->tag == req->tag) {
            pmix_list_remove_item(&pmix_ptl_globals.posted_recvs, &rcv->super);
            PMIX_RELEASE(rcv);
            PMIX_RELEASE(req);
            return;
        }
    }
    PMIX_RELEASE(req);
}

pmix_status_t pmix_ptl_base_cancel_recv(struct pmix_peer_t *peer,
                                        pmix_ptl_tag_t tag)
{
    (void)peer;
    pmix_ptl_posted_recv_t *req;

    req = PMIX_NEW(pmix_ptl_posted_recv_t);
    if (NULL == req) {
        return PMIX_ERR_NOMEM;
    }
    req->tag = tag;
    /* have to push this into an event so we can modify
     * the list of posted recvs */
    pmix_event_assign(&(req->ev), pmix_globals.evbase, -1,
                      EV_WRITE, cancel_recv, req);
    pmix_event_active(&(req->ev), EV_WRITE, 1);
    return PMIX_SUCCESS;
}
