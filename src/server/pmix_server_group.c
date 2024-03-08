/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2014-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2014-2019 Research Organization for Information Science
 *                         and Technology (RIST).  All rights reserved.
 * Copyright (c) 2014-2015 Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
 * Copyright (c) 2016-2019 Mellanox Technologies, Inc.
 *                         All rights reserved.
 * Copyright (c) 2016-2020 IBM Corporation.  All rights reserved.
 * Copyright (c) 2021-2024 Nanook Consulting  All rights reserved.
 * Copyright (c) 2022-2023 Triad National Security, LLC. All rights reserved.
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
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
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
#ifdef HAVE_TIME_H
#    include <time.h>
#endif
#include <event.h>

#ifndef MAX
#    define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#include "src/class/pmix_hotel.h"
#include "src/class/pmix_list.h"
#include "src/common/pmix_attributes.h"
#include "src/common/pmix_iof.h"
#include "src/hwloc/pmix_hwloc.h"
#include "src/mca/bfrops/bfrops.h"
#include "src/mca/gds/base/base.h"
#include "src/mca/plog/plog.h"
#include "src/mca/pnet/pnet.h"
#include "src/mca/prm/prm.h"
#include "src/mca/psensor/psensor.h"
#include "src/mca/ptl/base/base.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_environ.h"

#include "src/client/pmix_client_ops.h"
#include "pmix_server_ops.h"

static void opcbfunc(pmix_status_t status, void *cbdata)
{
    pmix_server_caddy_t *cd = (pmix_server_caddy_t*)cbdata;
    PMIX_HIDE_UNUSED_PARAMS(status);

    PMIX_INFO_FREE(cd->info, cd->ninfo);
    PMIX_RELEASE(cd);
}


static void _grpcbfunc(int sd, short args, void *cbdata)
{
    pmix_shift_caddy_t *scd = (pmix_shift_caddy_t *) cbdata;
    pmix_server_trkr_t *trk = scd->tracker;
    pmix_server_caddy_t *cd;
    pmix_buffer_t *reply, xfer, dblob, rankblob;
    pmix_status_t ret;
    size_t n, ctxid = SIZE_MAX, ngrpinfo, nmembers = 0, naddmembers = 0;
    pmix_proc_t *members = NULL;
    pmix_proc_t *addmembers = NULL;
    pmix_byte_object_t *bo = NULL, pbo;
    pmix_nspace_caddy_t *nptr;
    pmix_list_t nslist;
    bool found, first;
    bool ctxid_given = false;
    pmix_regattr_input_t *p;
    uint32_t index, endptidx, infoidx;
    int32_t cnt;
    pmix_proc_t procid;
    pmix_info_t *grpinfo, *iptr;
    pmix_kval_t kp;
    pmix_value_t val;
    pmix_data_array_t darray;
    char **nspaces = NULL;
    pmix_cb_t cb;
    pmix_proc_t proc;
    pmix_buffer_t pbkt, jobinfo;
    pmix_kval_t *kptr;
    void *values;

    PMIX_ACQUIRE_OBJECT(scd);
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    if (NULL == trk) {
        /* give them a release if they want it - this should
         * never happen, but protect against the possibility */
        if (NULL != scd->cbfunc.relfn) {
            scd->cbfunc.relfn(scd->cbdata);
        }
        PMIX_RELEASE(scd);
        return;
    }

    pmix_output_verbose(2, pmix_server_globals.group_output,
                        "server:grpcbfunc processing WITH %d CALLBACKS",
                        (int) pmix_list_get_size(&trk->local_cbs));

    // if this is a destruct operation, there is nothing more to do
    if (PMIX_GROUP_DESTRUCT == trk->grpop) {
        goto release;
    }

    /* see if this group was assigned a context ID or collected data */
    for (n = 0; n < scd->ninfo; n++) {
        if (PMIX_CHECK_KEY(&scd->info[n], PMIX_GROUP_CONTEXT_ID)) {
            PMIX_VALUE_GET_NUMBER(ret, &scd->info[n].value, ctxid, size_t);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
            } else {
                ctxid_given = true;
            }

        } else if (PMIX_CHECK_KEY(&scd->info[n], PMIX_GROUP_ENDPT_DATA)) {
            bo = &scd->info[n].value.data.bo;

        } else if (PMIX_CHECK_KEY(&scd->info[n], PMIX_GROUP_MEMBERSHIP)) {
            members = (pmix_proc_t*)scd->info[n].value.data.darray->array;
            nmembers = scd->info[n].value.data.darray->size;

        } else if (PMIX_CHECK_KEY(&scd->info[n], PMIX_GROUP_ADD_MEMBERS)) {
            addmembers = (pmix_proc_t*)scd->info[n].value.data.darray->array;
            naddmembers = scd->info[n].value.data.darray->size;
        }
    }

    /* if data was returned, then we need to have the modex cbfunc
     * store it for us before releasing the group members */
    if (NULL != bo) {
        /* get the indices of the types of data */
        p = pmix_hash_lookup_key(UINT32_MAX, PMIX_GROUP_ENDPT_DATA, NULL);
        endptidx = p->index;
        p = pmix_hash_lookup_key(UINT32_MAX, PMIX_GROUP_INFO, NULL);
        infoidx = p->index;

        PMIX_CONSTRUCT(&xfer, pmix_buffer_t);
        PMIX_LOAD_BUFFER(pmix_globals.mypeer, &xfer, bo->bytes, bo->size);
        PMIX_CONSTRUCT(&nslist, pmix_list_t);
        /* Collect the nptr list with uniq GDS components of all local
         * participants. It does not allow multiple storing to the
         * same GDS if participants have mutual GDS. */
        PMIX_LIST_FOREACH (cd, &trk->local_cbs, pmix_server_caddy_t) {
            // see if we already have this nspace
            found = false;
            PMIX_LIST_FOREACH (nptr, &nslist, pmix_nspace_caddy_t) {
                if (0 == strcmp(nptr->ns->compat.gds->name, cd->peer->nptr->compat.gds->name)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                // add it
                nptr = PMIX_NEW(pmix_nspace_caddy_t);
                PMIX_RETAIN(cd->peer->nptr);
                nptr->ns = cd->peer->nptr;
                pmix_list_append(&nslist, &nptr->super);
            }
        }

        /* unpack each byte object - we get one byte object containing the
         * complete contribution from each participating server. Each byte
         * object contains up to two byte objects - one for the endpt data
         * from the local participants, and the other potentially containing
         * any group info published by those participants */
        /* unpack the index indicating the type of data in the object */
        cnt = 1;
        PMIX_BFROPS_UNPACK(ret, pmix_globals.mypeer, &xfer, &index, &cnt, PMIX_UINT32);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            PMIX_DESTRUCT(&xfer);
            goto release;
        }
        while (PMIX_SUCCESS == ret) {
            /* unpack the data blob for this object */
            cnt = 1;
            PMIX_BFROPS_UNPACK(ret, pmix_globals.mypeer, &xfer, &pbo, &cnt, PMIX_BYTE_OBJECT);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                PMIX_DESTRUCT(&xfer);
                goto release;
            }
            PMIX_CONSTRUCT(&dblob, pmix_buffer_t);
            PMIX_LOAD_BUFFER(pmix_globals.mypeer, &dblob, pbo.bytes, pbo.size);
            PMIX_BYTE_OBJECT_DESTRUCT(&pbo);

            if (index == endptidx) {
                PMIX_LIST_FOREACH (nptr, &nslist, pmix_nspace_caddy_t) {
                    PMIX_GDS_STORE_MODEX(ret, nptr->ns, &dblob, trk);
                    if (PMIX_SUCCESS != ret) {
                        PMIX_ERROR_LOG(ret);
                        break;
                    }
                }
            } else if (index == infoidx) {
                /* this is packed differently, at least for now, so we have
                 * to unpack it and process it directly */
                if (ctxid_given) {
                    /* the blob consists of a set of byte objects, each containing the ID
                     * of the contributing proc followed by the pmix_info_t they
                     * provided */
                    ret = PMIX_SUCCESS;
                    while (PMIX_SUCCESS == ret) {
                        cnt = 1;
                        PMIX_BFROPS_UNPACK(ret, pmix_globals.mypeer, &dblob, &pbo, &cnt, PMIX_BYTE_OBJECT);
                        if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == ret) {
                            /* not an error - we are simply done */
                            break;
                        }
                        if (PMIX_SUCCESS != ret) {
                            PMIX_ERROR_LOG(ret);
                            PMIX_DESTRUCT(&xfer);
                            PMIX_DESTRUCT(&dblob);
                            goto release;
                        }
                        PMIX_CONSTRUCT(&rankblob, pmix_buffer_t);
                        PMIX_LOAD_BUFFER(pmix_globals.mypeer, &rankblob, pbo.bytes, pbo.size);
                        PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
                        cnt = 1;
                        PMIX_BFROPS_UNPACK(ret, pmix_globals.mypeer, &rankblob, &procid, &cnt, PMIX_PROC);
                        if (PMIX_SUCCESS != ret) {
                            PMIX_ERROR_LOG(ret);
                            PMIX_DESTRUCT(&xfer);
                            PMIX_DESTRUCT(&dblob);
                            PMIX_DESTRUCT(&rankblob);
                            goto release;
                        }
                        cnt = 1;
                        PMIX_BFROPS_UNPACK(ret, pmix_globals.mypeer, &rankblob, &ngrpinfo, &cnt, PMIX_SIZE);
                        if (PMIX_SUCCESS != ret) {
                            PMIX_ERROR_LOG(ret);
                            PMIX_DESTRUCT(&xfer);
                            PMIX_DESTRUCT(&dblob);
                            PMIX_DESTRUCT(&rankblob);
                            goto release;
                        }
                        PMIX_INFO_CREATE(grpinfo, ngrpinfo);
                        cnt = ngrpinfo;
                        PMIX_BFROPS_UNPACK(ret, pmix_globals.mypeer, &rankblob, grpinfo, &cnt, PMIX_INFO);
                        if (PMIX_SUCCESS != ret) {
                            PMIX_ERROR_LOG(ret);
                            PMIX_DESTRUCT(&xfer);
                            PMIX_DESTRUCT(&dblob);
                            PMIX_DESTRUCT(&rankblob);
                            PMIX_INFO_FREE(grpinfo, ngrpinfo);
                            goto release;
                        }
                        /* reconstruct each value as a qualified one based
                         * on the ctxid */
                        PMIX_CONSTRUCT(&kp, pmix_kval_t);
                        kp.value = &val;
                        kp.key = PMIX_QUALIFIED_VALUE;
                        val.type = PMIX_DATA_ARRAY;
                        for (n=0; n < ngrpinfo; n++) {
                            PMIX_DATA_ARRAY_CONSTRUCT(&darray, 2, PMIX_INFO);
                            iptr = (pmix_info_t*)darray.array;
                            /* the primary value is in the first position */
                            PMIX_INFO_XFER(&iptr[0], &grpinfo[n]);
                            /* add the context ID qualifier */
                            PMIX_INFO_LOAD(&iptr[1], PMIX_GROUP_CONTEXT_ID, &ctxid, PMIX_SIZE);
                            PMIX_INFO_SET_QUALIFIER(&iptr[1]);
                            /* add it to the kval */
                            val.data.darray = &darray;
                            /* store it */
                            PMIX_GDS_STORE_KV(ret, pmix_globals.mypeer, &procid, PMIX_GLOBAL, &kp);
                            PMIX_DATA_ARRAY_DESTRUCT(&darray);
                            if (PMIX_SUCCESS != ret) {
                                PMIX_ERROR_LOG(ret);
                                PMIX_DESTRUCT(&xfer);
                                PMIX_DESTRUCT(&dblob);
                                PMIX_DESTRUCT(&rankblob);
                                PMIX_INFO_FREE(grpinfo, ngrpinfo);
                                goto release;
                            }
                        }
                        PMIX_INFO_FREE(grpinfo, ngrpinfo);
                        PMIX_DESTRUCT(&rankblob);
                        ret = PMIX_SUCCESS;
                    }
                }
            }
            PMIX_DESTRUCT(&dblob);
            /* get the next blob */
            cnt = 1;
            PMIX_BFROPS_UNPACK(ret, pmix_globals.mypeer, &xfer, &index, &cnt, PMIX_UINT32);
            if (PMIX_SUCCESS != ret && PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER != ret) {
                PMIX_ERROR_LOG(ret);
                break;
            }
        }
        PMIX_DESTRUCT(&xfer);
    }

release:
    /* find the unique nspaces that are participating */
    PMIX_LIST_FOREACH (cd, &trk->local_cbs, pmix_server_caddy_t) {
        if (NULL == nspaces) {
            PMIx_Argv_append_nosize(&nspaces, cd->peer->info->pname.nspace);
        } else {
            found = false;
            for (n = 0; NULL != nspaces[n]; n++) {
                if (0 == strcmp(nspaces[n], cd->peer->info->pname.nspace)) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                PMIx_Argv_append_nosize(&nspaces, cd->peer->info->pname.nspace);
            }
        }
    }

    /* loop across all procs in the tracker, sending them the reply */
    first = true;
    PMIX_LIST_FOREACH (cd, &trk->local_cbs, pmix_server_caddy_t) {
        reply = PMIX_NEW(pmix_buffer_t);
        if (NULL == reply) {
            break;
        }
        /* setup the reply, starting with the returned status */
        PMIX_BFROPS_PACK(ret, cd->peer, reply, &scd->status, 1, PMIX_STATUS);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            PMIX_RELEASE(reply);
            break;
        }
        if (PMIX_SUCCESS == scd->status && !trk->hybrid) {
            /* add the final membership */
            PMIX_BFROPS_PACK(ret, cd->peer, reply, &nmembers, 1, PMIX_SIZE);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                PMIX_RELEASE(reply);
                break;
            }
            if (0 < nmembers) {
                PMIX_BFROPS_PACK(ret, cd->peer, reply, members, nmembers, PMIX_PROC);
                if (PMIX_SUCCESS != ret) {
                    PMIX_ERROR_LOG(ret);
                    PMIX_RELEASE(reply);
                    break;
                }
            }
            /* if a ctxid was provided, pass it along */
            PMIX_BFROPS_PACK(ret, cd->peer, reply, &ctxid, 1, PMIX_SIZE);
            if (PMIX_SUCCESS != ret) {
                PMIX_ERROR_LOG(ret);
                PMIX_RELEASE(reply);
                break;
            }
            /* loop across all participating nspaces and include their
             * job-related info */
            if (first) {
                PMIX_CONSTRUCT(&jobinfo, pmix_buffer_t);
                first = false;
            }
            for (n = 0; NULL != nspaces[n]; n++) {
                /* if this is the local proc's own nspace, then
                 * ignore it - it already has this info */
                if (0 == strncmp(nspaces[n], cd->peer->info->pname.nspace, PMIX_MAX_NSLEN)) {
                    continue;
                }

                /* this is a local request, so give the gds the option
                 * of returning a copy of the data, or a pointer to
                 * local storage */
                /* add the job-level info, if necessary */
                PMIX_LOAD_PROCID(&proc, nspaces[n], PMIX_RANK_WILDCARD);
                PMIX_CONSTRUCT(&cb, pmix_cb_t);
                /* this is for a local client, so give the gds the
                 * option of returning a complete copy of the data,
                 * or returning a pointer to local storage */
                cb.proc = &proc;
                cb.scope = PMIX_SCOPE_UNDEF;
                cb.copy = false;
                PMIX_GDS_FETCH_KV(ret, cd->peer, &cb);
                if (PMIX_SUCCESS != ret) {
                    /* try getting it from our storage */
                    PMIX_GDS_FETCH_KV(ret, pmix_globals.mypeer, &cb);
                    if (PMIX_SUCCESS != ret) {
                        PMIX_ERROR_LOG(ret);
                        PMIX_RELEASE(reply);
                        PMIX_DESTRUCT(&cb);
                        goto done;
                    }
                }
                PMIX_CONSTRUCT(&pbkt, pmix_buffer_t);
                /* pack the nspace name */
                PMIX_BFROPS_PACK(ret, cd->peer, &pbkt, &nspaces[n], 1, PMIX_STRING);
                if (PMIX_SUCCESS != ret) {
                    PMIX_ERROR_LOG(ret);
                    PMIX_RELEASE(reply);
                    PMIX_DESTRUCT(&pbkt);
                    PMIX_DESTRUCT(&cb);
                    goto done;
                }
                PMIX_LIST_FOREACH (kptr, &cb.kvs, pmix_kval_t) {
                    PMIX_BFROPS_PACK(ret, cd->peer, &pbkt, kptr, 1, PMIX_KVAL);
                    if (PMIX_SUCCESS != ret) {
                        PMIX_ERROR_LOG(ret);
                        PMIX_RELEASE(reply);
                        PMIX_DESTRUCT(&pbkt);
                        PMIX_DESTRUCT(&cb);
                        goto done;
                    }
                }
                PMIX_DESTRUCT(&cb);


                PMIX_UNLOAD_BUFFER(&pbkt, pbo.bytes, pbo.size);
                PMIX_BFROPS_PACK(ret, cd->peer, reply, &pbo, 1, PMIX_BYTE_OBJECT);
                // add it to the jobinfo buffer
                PMIX_BFROPS_PACK(ret, cd->peer, &jobinfo, &pbo, 1, PMIX_BYTE_OBJECT);
                PMIX_BYTE_OBJECT_DESTRUCT(&pbo);
                if (PMIX_SUCCESS != ret) {
                    PMIX_ERROR_LOG(ret);
                    PMIX_RELEASE(reply);
                    PMIX_DESTRUCT(&pbkt);
                    PMIX_DESTRUCT(&cb);
                    goto done;
                }

                PMIX_DESTRUCT(&pbkt);
            }
        }

done:
        pmix_output_verbose(2, pmix_server_globals.group_output,
                            "server:grp_cbfunc reply being sent to %s:%u",
                            cd->peer->info->pname.nspace, cd->peer->info->pname.rank);
        PMIX_SERVER_QUEUE_REPLY(ret, cd->peer, cd->hdr.tag, reply);
        if (PMIX_SUCCESS != ret) {
            PMIX_RELEASE(reply);
        }
    }

    // now deal with any add-members - they need to be notified by event
    if (NULL != addmembers) {
        PMIX_INFO_LIST_START(values);
        // mark that this event stays local and does not go up to the host
        PMIX_INFO_LIST_ADD(ret, values, PMIX_EVENT_STAYS_LOCAL, NULL, PMIX_BOOL);
        if (ctxid_given) {
            PMIX_INFO_LIST_ADD(ret, values, PMIX_GROUP_CONTEXT_ID, &ctxid, PMIX_SIZE);
        }
        // load the membership into the info list array - note: this copies the array!
        darray.type = PMIX_PROC;
        darray.array = members;
        darray.size = nmembers;
        PMIX_INFO_LIST_ADD(ret, values, PMIX_GROUP_MEMBERSHIP, &darray, PMIX_DATA_ARRAY);
        // provide the group ID
        PMIX_INFO_LIST_ADD(ret, values, PMIX_GROUP_ID, trk->id, PMIX_STRING);
        // set the range to be only procs that were added
        darray.array = addmembers;
        darray.size = naddmembers;
        // load the array - note: this copies the array!
        PMIX_INFO_LIST_ADD(ret, values, PMIX_EVENT_CUSTOM_RANGE, &darray, PMIX_DATA_ARRAY);
        // add the job-level info
        PMIX_UNLOAD_BUFFER(&jobinfo, pbo.bytes, pbo.size);
        PMIX_INFO_LIST_ADD(ret, values, PMIX_GROUP_JOB_INFO, &pbo, PMIX_BYTE_OBJECT);
        PMIX_BYTE_OBJECT_DESTRUCT(&pbo);

        // convert the list to an array
        PMIX_INFO_LIST_CONVERT(ret, values, &darray);
        cd = PMIX_NEW(pmix_server_caddy_t);
        cd->info = (pmix_info_t*)darray.array;
        cd->ninfo = darray.size;
        PMIX_INFO_LIST_RELEASE(values);
        // notify local procs
        PMIx_Notify_event(PMIX_GROUP_INVITED, &pmix_globals.myid, PMIX_RANGE_CUSTOM,
                          cd->info, cd->ninfo, opcbfunc, cd);
    }

    /* remove the tracker from the list */
    pmix_list_remove_item(&pmix_server_globals.collectives, &trk->super);
    PMIX_RELEASE(trk);

    /* we are done */
    if (NULL != scd->cbfunc.relfn) {
        scd->cbfunc.relfn(scd->cbdata);
    }
    PMIX_RELEASE(scd);
}

static void grpcbfunc(pmix_status_t status,
                      pmix_info_t *info, size_t ninfo, void *cbdata,
                      pmix_release_cbfunc_t relfn, void *relcbd)
{
    pmix_server_trkr_t *tracker = (pmix_server_trkr_t *) cbdata;
    pmix_shift_caddy_t *scd;

    pmix_output_verbose(2, pmix_server_globals.group_output,
                        "server:grpcbfunc called with %d info", (int) ninfo);

    if (NULL == tracker) {
        /* nothing to do - but be sure to give them
         * a release if they want it */
        if (NULL != relfn) {
            relfn(relcbd);
        }
        return;
    }
    /* need to thread-shift this callback as it accesses global data */
    scd = PMIX_NEW(pmix_shift_caddy_t);
    if (NULL == scd) {
        /* nothing we can do */
        if (NULL != relfn) {
            relfn(cbdata);
        }
        return;
    }
    scd->status = status;
    scd->info = info;
    scd->ninfo = ninfo;
    scd->tracker = tracker;
    scd->cbfunc.relfn = relfn;
    scd->cbdata = relcbd;
    PMIX_THREADSHIFT(scd, _grpcbfunc);
}

static void grp_timeout(int sd, short args, void *cbdata)
{
    pmix_server_trkr_t *trk = (pmix_server_trkr_t *) cbdata;
    pmix_server_caddy_t *cd;
    pmix_buffer_t *reply;
    pmix_status_t ret, rc = PMIX_ERR_TIMEOUT;
    PMIX_HIDE_UNUSED_PARAMS(sd, args);

    pmix_output_verbose(2, pmix_server_globals.group_output,
                        "ALERT: grp construct timeout fired");

    /* loop across all procs in the tracker, alerting
     * them to the failure */
    PMIX_LIST_FOREACH (cd, &trk->local_cbs, pmix_server_caddy_t) {
        reply = PMIX_NEW(pmix_buffer_t);
        if (NULL == reply) {
            break;
        }
        /* setup the reply, starting with the returned status */
        PMIX_BFROPS_PACK(ret, cd->peer, reply, &rc, 1, PMIX_STATUS);
        if (PMIX_SUCCESS != ret) {
            PMIX_ERROR_LOG(ret);
            PMIX_RELEASE(reply);
            break;
        }
        pmix_output_verbose(2, pmix_server_globals.group_output,
                            "server:grp_cbfunc TIMEOUT being sent to %s:%u",
                            cd->peer->info->pname.nspace, cd->peer->info->pname.rank);
        PMIX_SERVER_QUEUE_REPLY(ret, cd->peer, cd->hdr.tag, reply);
        if (PMIX_SUCCESS != ret) {
            PMIX_RELEASE(reply);
        }
    }

    trk->event_active = false;
    /* record this group as failed */
    PMIx_Argv_append_nosize(&pmix_server_globals.failedgrps, trk->id);
    /* remove the tracker from the list */
    pmix_list_remove_item(&pmix_server_globals.collectives, &trk->super);
    PMIX_RELEASE(trk);
}

static pmix_status_t aggregate_info(pmix_server_trkr_t *trk,
                                    pmix_info_t *info, size_t ninfo)
{
    pmix_list_t ilist, nmlist;
    size_t n, m, j, k, niptr;
    pmix_info_t *iptr;
    bool found, nmfound;
    pmix_info_caddy_t *icd;
    pmix_proclist_t *nm;
    pmix_proc_t *nmarray, *trkarray;
    size_t nmsize, trksize, bt, bt2;
    pmix_status_t rc;

    if (NULL == trk->info) {
        trk->info = info;
        trk->ninfo = ninfo;
        return PMIX_EXISTS;
    }

    // only keep unique entries
    PMIX_CONSTRUCT(&ilist, pmix_list_t);
    for (m=0; m < ninfo; m++) {
        found = false;
        for (n=0; n < trk->ninfo; n++) {
            if (PMIX_CHECK_KEY(&trk->info[n], info[m].key)) {

                // check a few critical keys
                if (PMIX_CHECK_KEY(&info[m], PMIX_GROUP_ADD_MEMBERS)) {
                    // aggregate the members
                    nmarray = (pmix_proc_t*)info[m].value.data.darray->array;
                    nmsize = info[m].value.data.darray->size;
                    trkarray = (pmix_proc_t*)trk->info[n].value.data.darray->array;
                    trksize = trk->info[n].value.data.darray->size;
                    PMIX_CONSTRUCT(&nmlist, pmix_list_t);
                    // sadly, an exhaustive search
                    for (j=0; j < nmsize; j++) {
                        nmfound = false;
                        for (k=0; k < trksize; k++) {
                            if (PMIX_CHECK_PROCID(&nmarray[j], &trkarray[k])) {
                                // if the new one is rank=WILDCARD, then ensure
                                // we keep it as wildcard
                                if (PMIX_RANK_WILDCARD == nmarray[j].rank) {
                                    trkarray[k].rank = PMIX_RANK_WILDCARD;
                                }
                                nmfound = true;
                                break;
                            }
                        }
                        if (!nmfound) {
                            nm = PMIX_NEW(pmix_proclist_t);
                            memcpy(&nm->proc, &nmarray[j], sizeof(pmix_proc_t));
                            pmix_list_append(&nmlist, &nm->super);
                        }
                    }
                    // create the replacement array, if needed
                    if (0 < pmix_list_get_size(&nmlist)) {
                        nmsize = trksize + pmix_list_get_size(&nmlist);
                        PMIX_PROC_CREATE(nmarray, nmsize);
                        memcpy(nmarray, trkarray, trksize * sizeof(pmix_proc_t));
                        j = trksize;
                        PMIX_LIST_FOREACH(nm, &nmlist, pmix_proclist_t) {
                            memcpy(&nmarray[j], &nm->proc, sizeof(pmix_proc_t));
                            ++j;
                        }
                        PMIX_PROC_FREE(trkarray, trksize);
                        trk->info[n].value.data.darray->array = nmarray;
                        trk->info[n].value.data.darray->size = nmsize;
                    }
                    PMIX_LIST_DESTRUCT(&nmlist);

                } else if (PMIX_CHECK_KEY(&info[m], PMIX_GROUP_BOOTSTRAP)) {
                    // the numbers must match
                    PMIX_VALUE_GET_NUMBER(rc, &info[m].value, bt, size_t);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_LIST_DESTRUCT(&ilist);
                        return rc;
                    }
                    PMIX_VALUE_GET_NUMBER(rc, &trk->info[n].value, bt2, size_t);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_LIST_DESTRUCT(&ilist);
                        return rc;
                    }
                    if (bt != bt2) {
                        PMIX_LIST_DESTRUCT(&ilist);
                        return PMIX_ERR_BAD_PARAM;
                    }
                }
                found = true;
                break;
            }
        }
        if (!found) {
            // add this one in
            icd = PMIX_NEW(pmix_info_caddy_t);
            icd->info = &info[m];
            icd->ninfo = 1;
            pmix_list_append(&ilist, &icd->super);
        }
    }
    if (0 < pmix_list_get_size(&ilist)) {
        niptr = trk->ninfo + pmix_list_get_size(&ilist);
        PMIX_INFO_CREATE(iptr, niptr);
        for (n=0; n < trk->ninfo; n++) {
            PMIX_INFO_XFER(&iptr[n], &trk->info[n]);
        }
        n = trk->ninfo;
        PMIX_LIST_FOREACH(icd, &ilist, pmix_info_caddy_t) {
            PMIX_INFO_XFER(&iptr[n], icd->info);
            ++n;
        }
        PMIX_INFO_FREE(trk->info, trk->ninfo);
        trk->info = iptr;
        trk->ninfo = niptr;
        /* cleanup */
    }
    PMIX_LIST_DESTRUCT(&ilist);
    return PMIX_SUCCESS;
}

/* we are being called from the PMIx server's switchyard function,
 * which means we are in an event and can access global data */
pmix_status_t pmix_server_grpconstruct(pmix_server_caddy_t *cd, pmix_buffer_t *buf)
{
    pmix_peer_t *peer = (pmix_peer_t *) cd->peer;
    pmix_peer_t *pr;
    int32_t cnt, m;
    pmix_status_t rc;
    char *grpid;
    pmix_proc_t *procs;
    pmix_info_t *info = NULL, *iptr = NULL, *grpinfoptr = NULL;
    size_t n, ninfo, ninf, nprocs, n2, ngrpinfo = 0;
    pmix_server_trkr_t *trk;
    bool need_cxtid = false;
    bool match, force_local = false;
    bool embed_barrier = false;
    bool barrier_directive_included = false;
    bool locally_complete = false;
    bool bootstrap = false;
    pmix_buffer_t bucket, bkt;
    pmix_byte_object_t bo;
    pmix_grpinfo_t *g = NULL;
    pmix_regattr_input_t *p;
    struct timeval tv = {0, 0};

    pmix_output_verbose(2, pmix_server_globals.group_output,
                        "recvd grpconstruct cmd from %s",
                        PMIX_PEER_PRINT(cd->peer));

    /* unpack the group ID */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &grpid, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }

    /* is this a failed group? */
    if (NULL != pmix_server_globals.failedgrps) {
        for (m=0; NULL != pmix_server_globals.failedgrps[m]; m++) {
            if (0 == strcmp(grpid, pmix_server_globals.failedgrps[m])) {
                /* yes - reject it */
                free(grpid);
                return PMIX_ERR_TIMEOUT;
            }
        }
    }

    /* unpack the number of procs */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &nprocs, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }
    if (0 == nprocs) {
        PMIX_ERROR_LOG(PMIX_ERR_BAD_PARAM);
        return PMIX_ERR_BAD_PARAM;
    }
    PMIX_PROC_CREATE(procs, nprocs);
    if (NULL == procs) {
        rc = PMIX_ERR_NOMEM;
        goto error;
    }
    cnt = nprocs;
    PMIX_BFROPS_UNPACK(rc, peer, buf, procs, &cnt, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_PROC_FREE(procs, nprocs);
        goto error;
    }
    /* sort the procs */
    qsort(procs, nprocs, sizeof(pmix_proc_t), pmix_util_compare_proc);

    /* unpack the number of directives */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &ninf, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }
    ninfo = ninf + 2;
    PMIX_INFO_CREATE(info, ninfo);
    /* store default response */
    rc = PMIX_SUCCESS;
    PMIX_INFO_LOAD(&info[ninf], PMIX_SORTED_PROC_ARRAY, NULL, PMIX_BOOL);
    PMIX_INFO_LOAD(&info[ninf+1], PMIX_LOCAL_COLLECTIVE_STATUS, &rc, PMIX_STATUS);
    if (0 < ninf) {
        cnt = ninf;
        PMIX_BFROPS_UNPACK(rc, peer, buf, info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto error;
        }
    }

    /* check directives */
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_ASSIGN_CONTEXT_ID)) {
            need_cxtid = PMIX_INFO_TRUE(&info[n]);

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_LOCAL_ONLY)) {
            force_local = PMIX_INFO_TRUE(&info[n]);

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_BOOTSTRAP)) {
            // ignore the actual value - we just need to know it
            // is present
            bootstrap = true;

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_EMBED_BARRIER)) {
            embed_barrier = PMIX_INFO_TRUE(&info[n]);
            barrier_directive_included = true;

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_TIMEOUT)) {
            PMIX_VALUE_GET_NUMBER(rc, &info[n].value, tv.tv_sec, uint32_t);
            if (PMIX_SUCCESS != rc) {
                PMIX_PROC_FREE(procs, nprocs);
                PMIX_INFO_FREE(info, ninfo);
                goto error;
            }

        } else if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_INFO)) {
            grpinfoptr = (pmix_info_t*)info[n].value.data.darray->array;
            ngrpinfo = info[n].value.data.darray->size;
            g = PMIX_NEW(pmix_grpinfo_t);
            PMIX_LOAD_NSPACE(g->proc.nspace, peer->nptr->nspace);
            g->proc.rank = peer->info->pname.rank;
            PMIX_CONSTRUCT(&bucket, pmix_buffer_t);
            PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &bucket, &g->proc, 1, PMIX_PROC);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(g);
                PMIX_DESTRUCT(&bucket);
                PMIX_PROC_FREE(procs, nprocs);
                PMIX_INFO_FREE(info, ninfo);
                goto error;
                goto error;
            }
            PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &bucket, &ngrpinfo, 1, PMIX_SIZE);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(g);
                PMIX_DESTRUCT(&bucket);
                PMIX_PROC_FREE(procs, nprocs);
                PMIX_INFO_FREE(info, ninfo);
                goto error;
                goto error;
            }
            PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &bucket, grpinfoptr, ngrpinfo, PMIX_INFO);
            if (PMIX_SUCCESS != rc) {
                PMIX_ERROR_LOG(rc);
                PMIX_RELEASE(g);
                PMIX_DESTRUCT(&bucket);
                PMIX_PROC_FREE(procs, nprocs);
                PMIX_INFO_FREE(info, ninfo);
                goto error;
                goto error;
            }
            PMIX_UNLOAD_BUFFER(&bucket, g->blob.bytes, g->blob.size);
            PMIX_DESTRUCT(&bucket);
        }
    }

    /* find/create the local tracker for this operation */
    trk = pmix_server_get_tracker(grpid, procs, nprocs, PMIX_GROUP_CONSTRUCT_CMD);
    if (NULL == trk) {
        /* If no tracker was found - create and initialize it once */
        trk = pmix_server_new_tracker(grpid, procs, nprocs, PMIX_GROUP_CONSTRUCT_CMD);
        if (NULL == trk) {
            /* only if a bozo error occurs */
            PMIX_ERROR_LOG(PMIX_ERROR);
            rc = PMIX_ERROR;
            goto error;
        }
        /* group members must have access to all endpoint info
         * upon completion of the construct operation */
        trk->collect_type = PMIX_COLLECT_YES;
        /* mark as being a construct operation */
        trk->grpop = PMIX_GROUP_CONSTRUCT;
        /* it is possible that different participants will
         * provide different attributes, so collect the
         * aggregate of them */
        rc = aggregate_info(trk, info, ninfo);
        if (PMIX_SUCCESS == rc) {
            // we extended the trk's info array
            PMIX_INFO_FREE(info, ninfo);
            info = NULL;
        }
        if (NULL != g) {
            pmix_list_append(&trk->grpinfo, &g->super);
        }
        /* see if this constructor only references local processes and isn't
         * requesting a context ID - if both conditions are met, then we
         * can just locally process the request without bothering the host.
         * This is meant to provide an optimized path for a fairly common
         * operation */
        if (force_local) {
            trk->local = true;
        } else if (need_cxtid) {
            trk->local = false;
        } else {
            trk->local = true;
            for (n = 0; n < nprocs; n++) {
                /* if this entry references the local procs, then
                 * we can skip it */
                if (PMIX_RANK_LOCAL_PEERS == procs[n].rank ||
                    PMIX_RANK_LOCAL_NODE == procs[n].rank) {
                    continue;
                }
                /* see if it references a specific local proc */
                match = false;
                for (m = 0; m < pmix_server_globals.clients.size; m++) {
                    pr = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, m);
                    if (NULL == pr) {
                        continue;
                    }
                    if (PMIX_CHECK_NAMES(&procs[n], &pr->info->pname)) {
                        match = true;
                        break;
                    }
                }
                if (!match) {
                    /* this requires a non_local operation */
                    trk->local = false;
                    break;
                }
            }
        }
    } else {
        /* it is possible that different participants will
         * provide different attributes, so collect the
         * aggregate of them */
        rc = aggregate_info(trk, info, ninfo);
        if (PMIX_SUCCESS == rc) {
            // we extended the trk's info array
            PMIX_INFO_FREE(info, ninfo);
            info = NULL;
        }
        /* add any grpinfo that might have been included */
        if (NULL != g) {
            pmix_list_append(&trk->grpinfo, &g->super);
        }
    }

    /* add this contributor to the tracker so they get
     * notified when we are done */
    pmix_list_append(&trk->local_cbs, &cd->super);

    // if this is a bootstrap, then pass it up
    if (bootstrap) {
        goto proceed;
    }

    /* are we locally complete? */
    if (trk->def_complete && pmix_list_get_size(&trk->local_cbs) == trk->nlocal) {
        locally_complete = true;
    }

    /* if we are not locally complete AND this operation
     * is completely local AND someone specified a timeout,
     * then we will monitor the timeout in this library.
     * Otherwise, any timeout must be done by the host
     * to avoid a race condition whereby we release the
     * tracker object while the host is still using it */
    if (!locally_complete && trk->local &&
        0 < tv.tv_sec && !trk->event_active) {
        PMIX_THREADSHIFT_DELAY(trk, grp_timeout, tv.tv_sec);
        trk->event_active = true;
    }

    /* if we are not locally complete, then we are done */
    if (!locally_complete) {
        return PMIX_SUCCESS;
    }

    /* if all local contributions have been received,
     * shutdown the timeout event if active */
    if (trk->event_active) {
        pmix_event_del(&trk->ev);
    }

    /* let the local host's server know that we are at the
     * "fence" point - they will callback once the barrier
     * across all participants has been completed */

    pmix_output_verbose(2, pmix_server_globals.group_output,
                        "local group op complete with %d procs",
                        (int) trk->npcs);

    if (trk->local) {
        /* we have created the local group, so we technically
         * are done. However, we want to give the host a chance
         * to know about the group to support further operations.
         * For example, a tool might want to query the host to get
         * the IDs of existing groups. So if the host supports
         * group operations, pass this one up to it but indicate
         * it is strictly local */
        if (NULL != pmix_host_server.group) {
            /* we only need to pass the group ID, members, and
             * an info indicating that this is strictly a local
             * operation */
            if (!force_local) {
                /* add the local op flag to the info array */
                ninfo = trk->ninfo + 1;
                PMIX_INFO_CREATE(info, ninfo);
                for (n=0; n < trk->ninfo; n++) {
                    PMIX_INFO_XFER(&info[n], &trk->info[n]);
                }
                PMIX_INFO_LOAD(&info[trk->ninfo], PMIX_GROUP_LOCAL_ONLY, NULL, PMIX_BOOL);
                PMIX_INFO_FREE(trk->info, trk->ninfo);
                trk->info = info;
                trk->ninfo = ninfo;
                info = NULL;
                ninfo = 0;
            }
            rc = pmix_host_server.group(PMIX_GROUP_CONSTRUCT, grpid, trk->pcs, trk->npcs,
                                        trk->info, trk->ninfo, grpcbfunc, trk);
            if (PMIX_SUCCESS != rc) {
                if (PMIX_OPERATION_SUCCEEDED == rc) {
                    /* let the grpcbfunc threadshift the result */
                    grpcbfunc(PMIX_SUCCESS, NULL, 0, trk, NULL, NULL);
                    return PMIX_SUCCESS;
                }
                /* remove the tracker from the list */
                pmix_list_remove_item(&pmix_server_globals.collectives, &trk->super);
                PMIX_RELEASE(trk);
                return rc;
            }
            /* we will take care of the rest of the process when the
             * host returns our call */
            return PMIX_SUCCESS;
        } else {
            /* let the grpcbfunc threadshift the result */
            grpcbfunc(PMIX_SUCCESS, NULL, 0, trk, NULL, NULL);
            return PMIX_SUCCESS;
        }
    }

    /* we don't have to worry about the timeout event being
     * active in the rest of this code because we only come
     * here if the operation is NOT completely local, and
     * we only activate the timeout if it IS local */

proceed:
    /* check if our host supports group operations */
    if (NULL == pmix_host_server.group) {
        /* cannot support it */
        pmix_list_remove_item(&pmix_server_globals.collectives, &trk->super);
        PMIX_RELEASE(trk);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    /* if they direct us to not embed a barrier, then we won't gather
     * the data for distribution */
    if (!barrier_directive_included ||
        (barrier_directive_included && embed_barrier) ||
        0 < pmix_list_get_size(&trk->grpinfo)) {
        /* collect any remote contributions provided by group members */
        PMIX_CONSTRUCT(&bucket, pmix_buffer_t);
        rc = pmix_server_collect_data(trk, &bucket);
        if (PMIX_SUCCESS != rc) {
            /* remove the tracker from the list */
            pmix_list_remove_item(&pmix_server_globals.collectives, &trk->super);
            PMIX_RELEASE(trk);
            PMIX_DESTRUCT(&bucket);
            return rc;
        }
        /* xfer the results to a byte object */
        PMIX_UNLOAD_BUFFER(&bucket, bo.bytes, bo.size);
        PMIX_DESTRUCT(&bucket);
        /* load any results into a data object for inclusion in the
         * fence operation */
        if (0 < bo.size ||
            0 < pmix_list_get_size(&trk->grpinfo)) {
            n2 = trk->ninfo + 1; // include space for endpt data
            PMIX_INFO_CREATE(iptr, n2);
            for (n = 0; n < trk->ninfo; n++) {
                PMIX_INFO_XFER(&iptr[n], &trk->info[n]);
            }
            /* add the endpt data */
            PMIX_CONSTRUCT(&bucket, pmix_buffer_t);
            if (0 < bo.size) {
                p = pmix_hash_lookup_key(UINT32_MAX, PMIX_GROUP_ENDPT_DATA, NULL);
                PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &bucket, &p->index, 1, PMIX_UINT32);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DESTRUCT(&bucket);
                    goto error;
                }
                PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &bucket, &bo, 1, PMIX_BYTE_OBJECT);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DESTRUCT(&bucket);
                    goto error;
                }
                PMIX_BYTE_OBJECT_DESTRUCT(&bo);
            }
            if (0 < pmix_list_get_size(&trk->grpinfo)) {
                p = pmix_hash_lookup_key(UINT32_MAX, PMIX_GROUP_INFO, NULL);
                PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &bucket, &p->index, 1, PMIX_UINT32);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DESTRUCT(&bucket);
                    goto error;
                }
                PMIX_CONSTRUCT(&bkt, pmix_buffer_t);
                PMIX_LIST_FOREACH(g, &trk->grpinfo, pmix_grpinfo_t) {
                    PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &bkt, &g->blob, 1, PMIX_BYTE_OBJECT);
                    if (PMIX_SUCCESS != rc) {
                        PMIX_ERROR_LOG(rc);
                        PMIX_DESTRUCT(&bucket);
                        PMIX_DESTRUCT(&bkt);
                        goto error;
                    }
                }
                PMIX_UNLOAD_BUFFER(&bkt, bo.bytes, bo.size);
                PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &bucket, &bo, 1, PMIX_BYTE_OBJECT);
                if (PMIX_SUCCESS != rc) {
                    PMIX_ERROR_LOG(rc);
                    PMIX_DESTRUCT(&bucket);
                    goto error;
                }
                PMIX_BYTE_OBJECT_DESTRUCT(&bo);
            }
            PMIX_UNLOAD_BUFFER(&bucket, bo.bytes, bo.size);
            PMIX_INFO_LOAD(&iptr[n2-1], PMIX_GROUP_ENDPT_DATA, &bo, PMIX_BYTE_OBJECT);
            PMIX_BYTE_OBJECT_DESTRUCT(&bo);
            /* replace the tracker's info array */
            PMIX_INFO_FREE(trk->info, trk->ninfo);
            trk->info = iptr;
            trk->ninfo = n2;
            iptr = NULL;
        }
    }

    rc = pmix_host_server.group(PMIX_GROUP_CONSTRUCT, grpid, trk->pcs, trk->npcs,
                                trk->info, trk->ninfo, grpcbfunc, trk);
    if (PMIX_SUCCESS != rc) {
        if (PMIX_OPERATION_SUCCEEDED == rc) {
            /* let the grpcbfunc threadshift the result */
            grpcbfunc(PMIX_SUCCESS, NULL, 0, trk, NULL, NULL);
            return PMIX_SUCCESS;
        }
        /* remove the tracker from the list */
        pmix_list_remove_item(&pmix_server_globals.collectives, &trk->super);
        PMIX_RELEASE(trk);
        return rc;
    }

    return PMIX_SUCCESS;

error:
    if (NULL != info) {
        PMIX_INFO_FREE(info, ninfo);
    }
    if (NULL != iptr) {
        PMIX_INFO_FREE(iptr, n2);
    }
    return rc;
}

/* we are being called from the PMIx server's switchyard function,
 * which means we are in an event and can access global data */
pmix_status_t pmix_server_grpdestruct(pmix_server_caddy_t *cd, pmix_buffer_t *buf)
{
    pmix_peer_t *peer = (pmix_peer_t *) cd->peer;
    int32_t cnt, m;
    pmix_status_t rc;
    char *grpid = NULL;
    pmix_info_t *info = NULL, *iptr;
    size_t n, ninfo, ninf, niptr;
    pmix_server_trkr_t *trk;
    pmix_proc_t *members = NULL;
    size_t nmembers = 0;
    bool force_local = false;
    bool match;
    bool locally_complete = false;
    pmix_peer_t *pr;
    struct timeval tv = {0, 0};

    pmix_output_verbose(2, pmix_server_globals.group_output,
                        "recvd grpdestruct cmd");

    /* unpack the group ID */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &grpid, &cnt, PMIX_STRING);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }

    /* is this a failed group? */
    if (NULL != pmix_server_globals.failedgrps) {
        for (m=0; NULL != pmix_server_globals.failedgrps[m]; m++) {
            if (0 == strcmp(grpid, pmix_server_globals.failedgrps[m])) {
                /* yes - reject it */
                free(grpid);
                return PMIX_ERR_TIMEOUT;
            }
        }
    }

    /* unpack the number of members */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &nmembers, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }
    if (0 == nmembers) {
        /* not allowed */
        rc = PMIX_ERR_BAD_PARAM;
        goto error;
    }
    /* unpack the membership */
    PMIX_PROC_CREATE(members, nmembers);
    cnt = nmembers;
    PMIX_BFROPS_UNPACK(rc, peer, buf, members, &cnt, PMIX_PROC);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        PMIX_PROC_FREE(members, nmembers);
        goto error;
    }

    /* unpack the number of directives */
    cnt = 1;
    PMIX_BFROPS_UNPACK(rc, peer, buf, &ninf, &cnt, PMIX_SIZE);
    if (PMIX_SUCCESS != rc) {
        PMIX_ERROR_LOG(rc);
        goto error;
    }
    ninfo = ninf + 1;
    PMIX_INFO_CREATE(info, ninfo);
    /* store default response */
    rc = PMIX_SUCCESS;
    PMIX_INFO_LOAD(&info[ninf], PMIX_LOCAL_COLLECTIVE_STATUS, &rc, PMIX_STATUS);
    if (0 < ninf) {
        cnt = ninf;
        PMIX_BFROPS_UNPACK(rc, peer, buf, info, &cnt, PMIX_INFO);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            goto error;
        }
    }

    /* check directives */
    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_LOCAL_ONLY)) {
            force_local = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_GROUP_LOCAL_ONLY)) {
            force_local = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_TIMEOUT)) {
            PMIX_VALUE_GET_NUMBER(rc, &info[n].value, tv.tv_sec, uint32_t);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
        }
    }

    /* find/create the local tracker for this operation */
    trk = pmix_server_get_tracker(grpid, members, nmembers, PMIX_GROUP_DESTRUCT_CMD);
    if (NULL == trk) {
        /* If no tracker was found - create and initialize it once */
        trk = pmix_server_new_tracker(grpid, members, nmembers, PMIX_GROUP_DESTRUCT_CMD);
        if (NULL == trk) {
            /* only if a bozo error occurs */
            PMIX_ERROR_LOG(PMIX_ERROR);
            rc = PMIX_ERROR;
            goto error;
        }
        trk->collect_type = PMIX_COLLECT_NO;
        /* mark as being a destruct operation */
        trk->grpop = PMIX_GROUP_DESTRUCT;
        /* see if this destructor only references local processes */
        trk->local = true;
        for (n = 0; n < nmembers; n++) {
            /* if this entry references the local procs, then
             * we can skip it */
            if (PMIX_RANK_LOCAL_PEERS == members[n].rank ||
                PMIX_RANK_LOCAL_NODE == members[n].rank) {
                continue;
            }
            /* see if it references a specific local proc - note that
             * the member name could include rank=wildcard */
            match = false;
            for (m = 0; m < pmix_server_globals.clients.size; m++) {
                pr = (pmix_peer_t *) pmix_pointer_array_get_item(&pmix_server_globals.clients, m);
                if (NULL == pr) {
                    continue;
                }
                // cannot use PMIX_CHECK_PROCID here as pmix_peer_t includes a pname field
                // and not a pmix_proc_t
                if (PMIX_RANK_WILDCARD == members[n].rank ||
                    PMIX_RANK_WILDCARD == pr->info->pname.rank) {
                    if (PMIX_CHECK_NSPACE(members[n].nspace, pr->info->pname.nspace)) {
                        match = true;
                        break;
                    }
                }
                if (PMIX_CHECK_NAMES(&members[n], &pr->info->pname)) {
                    match = true;
                    break;
                }
            }
            if (!match) {
                /* this requires a non_local operation */
                trk->local = false;
                break;
            }
        }
    }

    /* it is possible that different participants will
     * provide different attributes, so collect the
     * aggregate of them */
    if (NULL == trk->info) {
        trk->info = info;
        trk->ninfo = ninfo;
    } else {
        niptr = trk->ninfo + ninfo;
        PMIX_INFO_CREATE(iptr, niptr);
        for (n=0; n < trk->ninfo; n++) {
            PMIX_INFO_XFER(&iptr[n], &trk->info[n]);
        }
        for (n=0; n < ninfo; n++) {
            PMIX_INFO_XFER(&iptr[n+trk->ninfo], &info[n]);
        }
        PMIX_INFO_FREE(trk->info, trk->ninfo);
        trk->info = iptr;
        trk->ninfo = niptr;
        /* cleanup */
        PMIX_INFO_FREE(info, ninfo);
        info = NULL;
    }

    /* add this contributor to the tracker so they get
     * notified when we are done */
    pmix_list_append(&trk->local_cbs, &cd->super);

    /* are we locally complete? */
    if (trk->def_complete && pmix_list_get_size(&trk->local_cbs) == trk->nlocal) {
        locally_complete = true;
    }

    /* if we are not locally complete AND this operation
     * is completely local AND someone specified a timeout,
     * then we will monitor the timeout in this library.
     * Otherwise, any timeout must be done by the host
     * to avoid a race condition whereby we release the
     * tracker object while the host is still using it */
    if (!locally_complete && trk->local &&
        0 < tv.tv_sec && !trk->event_active) {
        PMIX_THREADSHIFT_DELAY(trk, grp_timeout, tv.tv_sec);
        trk->event_active = true;
    }

    /* if we are not locally complete, then we are done */
    if (!locally_complete) {
        return PMIX_SUCCESS;
    }

    /* if all local contributions have been received,
     * shutdown the timeout event if active */
    if (trk->event_active) {
        pmix_event_del(&trk->ev);
    }

    /* let the local host's server know that we are at the
     * "fence" point - they will callback once the barrier
     * across all participants has been completed */
    pmix_output_verbose(2, pmix_server_globals.group_output,
                        "local group destruct complete %d",
                        (int) trk->nlocal);
    if (trk->local) {
        /* we have removed the local group, so we technically
         * are done. However, we want to give the host a chance
         * to know remove the group to support further operations.
         * For example, a tool might want to query the host to get
         * the IDs of existing groups. So if the host supports
         * group operations, pass this one up to it but indicate
         * it is strictly local */
        if (NULL != pmix_host_server.group) {
            /* we only need to pass the group ID, members, and
             * an info indicating that this is strictly a local
             * operation */
            if (!force_local) {
                /* add the local op flag to the info array */
                ninfo = trk->ninfo + 1;
                PMIX_INFO_CREATE(info, ninfo);
                for (n=0; n < trk->ninfo; n++) {
                    PMIX_INFO_XFER(&info[n], &trk->info[n]);
                }
                PMIX_INFO_LOAD(&info[trk->ninfo], PMIX_GROUP_LOCAL_ONLY, NULL, PMIX_BOOL);
                PMIX_INFO_FREE(trk->info, trk->ninfo);
                trk->info = info;
                trk->ninfo = ninfo;
                info = NULL;
                ninfo = 0;
            }
            rc = pmix_host_server.group(PMIX_GROUP_DESTRUCT, grpid,
                                        members, nmembers,
                                        trk->info, trk->ninfo, grpcbfunc, trk);
            if (PMIX_SUCCESS != rc) {
                if (PMIX_OPERATION_SUCCEEDED == rc) {
                    /* let the grpcbfunc threadshift the result */
                    grpcbfunc(PMIX_SUCCESS, NULL, 0, trk, NULL, NULL);
                    PMIX_PROC_FREE(members, nmembers);
                    free(grpid);
                    return PMIX_SUCCESS;
                }
                /* remove the tracker from the list */
                pmix_list_remove_item(&pmix_server_globals.collectives, &trk->super);
                PMIX_RELEASE(trk);
                PMIX_PROC_FREE(members, nmembers);
                free(grpid);
                return rc;
            }
            /* we will take care of the rest of the process when the
             * host returns our call */
        } else {
            /* let the grpcbfunc threadshift the result and remove
             * the group from our list */
            grpcbfunc(PMIX_SUCCESS, NULL, 0, trk, NULL, NULL);
            PMIX_PROC_FREE(members, nmembers);
            free(grpid);
            return PMIX_SUCCESS;
        }
    }

    /* this operation requires global support, so check if our host
     * supports group operations */
    if (NULL == pmix_host_server.group) {
        /* cannot support it */
        pmix_list_remove_item(&pmix_server_globals.collectives, &trk->super);
        PMIX_RELEASE(trk);
        PMIX_PROC_FREE(members, nmembers);
        free(grpid);
        return PMIX_ERR_NOT_SUPPORTED;
    }

    rc = pmix_host_server.group(PMIX_GROUP_DESTRUCT, grpid,
                                members, nmembers,
                                trk->info, trk->ninfo, grpcbfunc, trk);
    if (PMIX_SUCCESS != rc) {
        if (PMIX_OPERATION_SUCCEEDED == rc) {
            /* let the grpcbfunc threadshift the result */
            grpcbfunc(PMIX_SUCCESS, NULL, 0, trk, NULL, NULL);
            PMIX_PROC_FREE(members, nmembers);
            free(grpid);
            return PMIX_SUCCESS;
        }
        /* remove the tracker from the list */
        pmix_list_remove_item(&pmix_server_globals.collectives, &trk->super);
        PMIX_RELEASE(trk);
        PMIX_PROC_FREE(members, nmembers);
        free(grpid);
        return rc;
    }

    PMIX_PROC_FREE(members, nmembers);
    free(grpid);
    return PMIX_SUCCESS;

error:
    if (NULL != info) {
        PMIX_INFO_FREE(info, ninfo);
    }
    if (NULL != members) {
        PMIX_PROC_FREE(members, nmembers);
    }
    if (NULL != grpid) {
        free(grpid);
    }
    return rc;
}
