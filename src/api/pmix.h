/*
 * Copyright (c) 2013-2014 Intel, Inc. All rights reserved
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer listed
 *   in this license in the documentation and/or other materials
 *   provided with the distribution.
 *
 * - Neither the name of the copyright holders nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 * The copyright holders provide no reassurances that the source code
 * provided does not infringe any patent, copyright, or any other
 * intellectual property rights of third parties.  The copyright holders
 * disclaim any liability to any recipient for claims brought against
 * recipient by any third party for infringement of that parties
 * intellectual property rights.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 * 
 * $COPYRIGHT$
 * 
 * Additional copyrights may follow
 * 
 * $HEADER$
 */

#ifndef PMIx_H
#define PMIx_H

#include <stdint.h>
#include <string.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h> /* for struct timeval */
#endif
#include <event.h>

#include "pmix_common.h"

BEGIN_C_DECLS

/****    PMIX API    ****/

/* NOTE: calls to these APIs must be thread-protected as there
 * currently is NO internal thread safety. */

/* Init */
pmix_status_t PMIx_Init(char nspace[], int *rank);

/* Finalize */
pmix_status_t PMIx_Finalize(void);

/* Initialized */
int PMIx_Initialized(void);

/* Abort */
pmix_status_t PMIx_Abort(int status, const char msg[]);

/* Fence */
/* collect_data - false (0) indicates that the callback is just used as
 * a release and no data is to be returned at that time, true (1) indicates
 * that all modex data is to be included in the callback. Returned data
 * is locally cached so that subsequent calls to PMIx_Get can be serviced
 * without communicating to/from the server, but at the cost of increased
 * memory footprint */
pmix_status_t PMIx_Fence(const pmix_range_t ranges[],
                         size_t nranges, int collect_data);

/* Fence_nb */
/* barrier - false (0) indicates that we are to be called back immediately,
 * true (non-zero) indicates that the callback is to occur once all participants
 * have reached the fence_nb call
 *
 * collect_data - false (0) indicates that the callback is just used as
 * a release and no data is to be returned at that time, true (1) indicates
 * that all modex data is to be locally cached in the process upon completion
 * of the fence operation. Locally cached data means that subsequent calls to
 * PMIx_Get can be serviced without communicating to/from the server, but at
 * the cost of increased memory footprint */
pmix_status_t PMIx_Fence_nb(const pmix_range_t ranges[], size_t nranges,
                            int barrier, int collect_data,
                            pmix_op_cbfunc_t cbfunc, void *cbdata);

/* Put */
pmix_status_t PMIx_Put(pmix_scope_t scope, const char key[], pmix_value_t *val);

/* Get */
pmix_status_t PMIx_Get(const char nspace[], int rank,
                       const char key[], pmix_value_t **val);

/* Get_nb */
pmix_status_t PMIx_Get_nb(const char nspace[], int rank,
                          const char key[],
                          pmix_value_cbfunc_t cbfunc, void *cbdata);

/* Publish - the "info" parameter
 * consists of an array of pmix_info_t objects that
 * are to be pushed to the "universal" nspace
 * for lookup by others subject to the provided scope.
 * Note that the keys must be unique within the specified
 * scope or else an error will be returned (first published
 * wins). */
pmix_status_t PMIx_Publish(pmix_scope_t scope,
                           const pmix_info_t info[],
                           size_t ninfo);
pmix_status_t PMIx_Publish_nb(pmix_scope_t scope,
                              const pmix_info_t info[],
                              size_t ninfo,
                              pmix_op_cbfunc_t cbfunc, void *cbdata);

/* Lookup - the "info" parameter consists of an array of
 * pmix_info_t objects that specify the requested
 * information. Info will be returned in the provided objects.
 * The nspace param will contain the nspace of
 * the process that published the service_name. Passing
 * a NULL for the nspace param indicates that the
 * nspace information need not be returned */
pmix_status_t PMIx_Lookup(pmix_scope_t scope,
                          pmix_info_t info[], size_t ninfo,
                          char nspace[]);
pmix_status_t PMIx_Lookup_nb(pmix_scope_t scope, char **keys,
                             pmix_lookup_cbfunc_t cbfunc, void *cbdata);

/* Unpublish - the "info" parameter
 * consists of an array of pmix_info_t objects */
pmix_status_t PMIx_Unpublish(pmix_scope_t scope,
                             const pmix_info_t info[],
                             size_t ninfo);
pmix_status_t PMIx_Unpublish_nb(pmix_scope_t scope,
                                const pmix_info_t info[],
                                size_t ninfo,
                                pmix_op_cbfunc_t cbfunc, void *cbdata);

/* Spawn a new job */
pmix_status_t PMIx_Spawn(const pmix_app_t apps[],
                         size_t napps, char nspace[]);
pmix_status_t PMIx_Spawn_nb(const pmix_app_t apps[], size_t napps,
                            pmix_spawn_cbfunc_t cbfunc, void *cbdata);

/* Record the specified processes as "connected". Both blocking and non-blocking
 * versions are provided. This means that the resource
 * manager should treat the failure of any process in the specified group as
 * a reportable event, and take appropriate action. The callback function is
 * to be called once all participating processes have called connect. Note that
 * a process can only engage in *one* connect operation involving the identical
 * set of ranges at a time. However, a process *can* be simultaneously engaged
 * in multiple connect operations, each involving a different set of ranges */
pmix_status_t PMIx_Connect(const pmix_range_t ranges[], size_t nranges);

pmix_status_t PMIx_Connect_nb(const pmix_range_t ranges[], size_t nranges,
                              pmix_op_cbfunc_t cbfunc, void *cbdata);
                              
/* Disconnect a previously connected set of processes. An error will be returned
 * if the specified set of ranges was not previously "connected". As above, a process
 * may be involved in multiple simultaneous disconnect operations. However, a process
 * is not allowed to reconnect to a set of ranges that has not fully completed
 * disconnect - i.e., you have to fully disconnect before you can reconnect to the
 * same group of processes. */
pmix_status_t PMIx_Disconnect(const pmix_range_t ranges[], size_t nranges);

pmix_status_t PMIx_Disconnect_nb(const pmix_range_t ranges[], size_t nranges,
                                 pmix_op_cbfunc_t cbfunc, void *cbdata);

END_C_DECLS
#endif
