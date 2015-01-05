/*
 * Copyright (c) 2013-2014 Intel, Inc. All rights reserved
 * $COPYRIGHT$
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
 * $HEADER$
 *
 * PMIx provides a "function-shipping" approach to support for
 * implementing the server-side of the protocol. This method allows
 * resource managers to implement the server without being burdened
 * with PMIx internal details. Accordingly, each PMIx API is mirrored
 * here in a function call to be provided by the server. When a
 * request is received from the client, the corresponding server function
 * will be called with the information.
 *
 * Any functions not supported by the RM can be indicated by a NULL for
 * the function pointer. Client calls to such functions will have a
 * "not supported" error returned.
 */

#ifndef PMIx_SERVER_API_H
#define PMIx_SERVER_API_H

#include <stdint.h>
#include <string.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h> /* for struct timeval */
#endif
#include <event.h>

#include "pmix_common.h"

BEGIN_C_DECLS

/****    SERVER CALLBACK FUNCTIONS FOR ASYNC OPERATIONS    ****/
typedef void (*pmix_fence_cbfunc_t)(void *cbdata);
typedef void (*pmix_modex_cbfunc_t)(int status, pmix_modex_data_t data[],
                                    size_t ndata, void *cbdata);
typedef void (*pmix_spawn_cbfunc_t)(int status, char namespace[]);

/****    SERVER FUNCTION-SHIPPED APIs    ****/
typedef int (*pmix_server_terminated_fn_t)(const char namespace[], int rank);
typedef int (*pmix_server_abort_fn_t)(int status, const char msg[]);
typedef int (*pmix_server_fence_fn_t)(const pmix_range_t ranges[], size_t nranges,
                                      pmix_modex_data_t *data[], size_t *ndata);
typedef int (*pmix_server_fencenb_fn_t)(const pmix_range_t ranges[], size_t nranges, int barrier,
                                        pmix_fence_cbfunc_t cbfunc, void *cbdata);
typedef int (*pmix_server_store_modex_fn_t)(pmix_scope_t scope, pmix_modex_data_t *data);
typedef int (*pmix_server_get_modex_fn_t)(const char namespace[], int rank,
                                          pmix_modex_data_t *data[], size_t *ndata);
typedef int (*pmix_server_get_modexnb_fn_t)(const char namespace[], int rank,
                                            pmix_modex_cbfunc_t cbfunc, void *cbdata);
typedef int (*pmix_server_get_job_info_fn_t)(const char namespace[], int rank,
                                             pmix_info_t *info[], int *ninfo);
typedef int (*pmix_server_publish_fn_t)(pmix_scope_t scope, const pmix_info_t info[], size_t ninfo);
typedef int (*pmix_server_lookup_fn_t)(pmix_info_t info[], size_t ninfo,
                                       char namespace[]);
typedef int (*pmix_server_unpublish_fn_t)(const pmix_info_t info[], size_t ninfo);
typedef int (*pmix_server_spawn_fn_t)(const pmix_app_t apps[],
                                      size_t napps,
                                      pmix_spawn_cbfunc_t cbfunc, void *cbdata);
typedef int (*pmix_server_connect_fn_t)(const pmix_range_t ranges[], size_t nranges);
typedef int (*pmix_server_disconnect_fn_t)(const pmix_range_t ranges[], size_t nranges);

/****    SERVER SECURITY CREDENTIAL FUNCTIONS    ****/
typedef int (*pmix_server_authenticate_fn_t)(char *credential);

typedef struct pmix_server_module_1_0_0_t {
    pmix_server_authenticate_fn_t     authenticate;
    pmix_server_terminated_fn_t       terminated;
    pmix_server_abort_fn_t            abort;
    pmix_server_fence_fn_t            fence;
    pmix_server_fencenb_fn_t          fence_nb;
    pmix_server_store_modex_fn_t      store_modex;
    pmix_server_get_modex_fn_t        get_modex;
    pmix_server_get_modexnb_fn_t      get_modex_nb;
    pmix_server_get_job_info_fn_t     get_job_info;
    pmix_server_publish_fn_t          publish;
    pmix_server_lookup_fn_t           lookup;
    pmix_server_unpublish_fn_t        unpublish;
    pmix_server_spawn_fn_t            spawn;
    pmix_server_connect_fn_t          connect;
    pmix_server_disconnect_fn_t       disconnect;
} pmix_server_module_t;

/****    SERVER SUPPORT INIT/FINALIZE FUNCTIONS    ****/
int PMIx_server_init(pmix_server_module_t *module,
                     struct event_base *evbase,
                     char *tmpdir, char *credential);
int PMIx_server_finalize(void);
int PMIx_server_setup_fork(const char namespace[], int rank, char ***env);

END_C_DECLS

#endif
