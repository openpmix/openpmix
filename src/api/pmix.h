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

/* Initialize the PMIx client, returning the namespace assigned
 * to this client's application in the provided character array 
 * (must be of size PMIX_MAX_NSLEN or greater). Passing a parameter
 * of _NULL_ for either or both parameters is allowed if the user
 * wishes solely to initialize the PMIx system and does not require
 * return of the NULL parameter(s) at that time.
 *
 * When called the PMIx client will check for the required connection
 * information of the local PMIx server and will establish the connection.
 * If the information is not found, or the server connection fails, then
 * an appropriate error constant will be returned.
 *
 * If successful, the function will return PMIX_SUCCESS, will fill the
 * provided namespace array with the server-assigned namespace, and return
 * the rank of the process within the application. Note that the PMIx
 * client library is referenced counted, and so multiple calls to PMIx_Init
 * are allowed. Thus, one way to obtain the namespace and rank of the
 * process is to simply call PMIx_Init with non-NULL parameters. */
pmix_status_t PMIx_Init(char nspace[], int *rank);

/* Finalize the PMIx client, closing the connection to the local server.
 * An error code will be returned if, for some reason, the connection
 * cannot be closed. */
pmix_status_t PMIx_Finalize(void);

/* Returns _true_ if the PMIx client has been successfully initialized,
 * returns _false_ otherwise. Note that the function only reports the
 * internal state of the PMIx client - it does not verify an active
 * connection with the server, nor that the server is functional. */
int PMIx_Initialized(void);

/* Request that the current application be aborted, returning the
 * provided _status_ and printing the provided message. The response
 * to this request is somewhat dependent on the specific resource
 * manager and its configuration (e.g., some resource managers will
 * not abort the application if the provided _status_ is zero unless
 * specifically configured to do so), and thus lies outside the control
 * of PMIx itself. However, the client will inform the local PMIx of
 * the request that the application be aborted, regardless of the
 * value of the provided _status_.
 *
 * Passing a _NULL_ msg parameter is allowed. Note that race conditions
 * caused by multiple processes calling PMIx_Abort are left to the
 * server implementation to resolve with regard to which status is
 * returned and what messages (if any) are printed.
 */
pmix_status_t PMIx_Abort(int status, const char msg[]);

/* Push all previously _PMIx_Put_ values to the local PMIx server and
 * execute a blocking barrier across the processes identified in the
 * specified ranges. Passing a _NULL_ pointer as the _ranges_ parameter
 * indicates that the barrier is to span all processes in the client's
 * namespace. Each provided range struct can pass _NULL_ for its array
 * of ranks to indicate that all processes in the given namespace are
 * participating.
 *
 * The _collect_data_ parameter is passed to the server to indicate whether
 * or not the barrier operation is to return the _put_ data from all
 * participating processes. A value of _false_ indicates that the callback
 * is just used as a release and no data is to be returned at that time. A
 * value of _true_ indicates that all _put_ data is to be collected by the
 * barrier. Returned data is locally cached so that subsequent calls to _PMIx_Get_
 * can be serviced without communicating to/from the server, but at the cost
 * of increased memory footprint
 */
pmix_status_t PMIx_Fence(const pmix_range_t ranges[],
                         size_t nranges, int collect_data);

/* Fence_nb */
/* Non-blocking version of PMIx_Fence. Push all previously _PMIx_Put_ values
 * to the local PMIx server. Passing a _true_ value to the _barrier_ parameter
 * will direct the PMIx server to execute the specified callback function once
 * all participating processes have called _PMIx_Fence_nb_. Passing a value of
 * _false_ for _barrier_ will cause the callback function to be immediately
 * executed upon the server receiving the command.
 *
 * Passing a _NULL_ pointer as the _ranges_ parameter indicates that any
 * requested barrier is to span all processes in the client's namespace. Each
 * provided range struct can pass _NULL_ for its array of ranks to indicate
 * that all processes in the given namespace are participating.
 *
 * The _collect_data_ parameter is passed to the server to indicate whether
 * or not the barrier operation is to return the _put_ data from all participating
 * processes in the callback. A value of _false_ indicates that the callback is
 * just used as a release and no data is to be returned at that time. A value of
 * _true_ indicates that all _put_ data is to be collected by the barrier and
 * returned in the callback function. A _NULL_ callback function can be used to
 * indicate that no callback is desired. */
pmix_status_t PMIx_Fence_nb(const pmix_range_t ranges[], size_t nranges,
                            int barrier, int collect_data,
                            pmix_op_cbfunc_t cbfunc, void *cbdata);

/* Push a value into the client's namespace. The client library will cache
 * the information locally until _PMIx_Fence_ is called. The provided scope
 * value is passed to the local PMIx server, which will distribute the data
 * as directed. */
pmix_status_t PMIx_Put(pmix_scope_t scope, const char key[], pmix_value_t *val);

/* Retrieve information for the specified _key_ as published by the given _rank_
 * within the provided _namespace_, returning a pointer to the value in the
 * given address. This is a blocking operation - the caller will block until
 * the specified data has been _PMIx_Put_ by the specified rank. The caller is
 * responsible for freeing all memory associated with the returned value when
 * no longer required. */
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
