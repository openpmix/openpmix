/*
 * Copyright (c) 2013-2015 Intel, Inc. All rights reserved
 * Copyright (c) 2015      Artem Y. Polyakov <artpol84@gmail.com>.
 *                         All rights reserved.
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
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif
#ifdef HAVE_NET_UIO_H
#include <net/uio.h>
#endif
#include <event.h>

#include "pmix_common.h"

BEGIN_C_DECLS

/****    SERVER FUNCTION-SHIPPED APIs    ****/
/* NOTE: for performance purposes, the host server is required to
 * return as quickly as possible from all functions. Execution of
 * the function is thus to be done asynchronously so as to allow
 * the PMIx server support library to handle multiple client requests
 * as quickly and scalably as possible.
 *
 * ALL data passed to the host server functions is "owned" by the
 * PMIX server support library and MUST NOT be free'd. Data returned
 * by the host server via callback function is owned by the host
 * server, which is free to release it upon return from the callback */


/* Notify the host server that a client has terminated, as detected
 * when the PMIx server library receives a socket closure notification */
typedef int (*pmix_server_terminated_fn_t)(const char nspace[], int rank);

/* Client called PMIx_Abort - note that the client will be in a blocked
 * state until the host server executes the callback function, thus
 * allowing the PMIx server support library to release the client */
typedef int (*pmix_server_abort_fn_t)(int status, const char msg[],
                                      pmix_op_cbfunc_t cbfunc, void *cbdata);

/* Client called either PMIx_Fence or PMIx_Fence_nb. In either case,
 * the host server will be called via a non-blocking function to execute
 * the specified operation. All processes in the specified ranges are
 * required to participate in the Fence[_nb] operation. If the "barrier"
 * parameter is "true", then the callback is to be executed once all
 * participants have participated. If the "barrier" parameter is "false",
 * then the callback is to be executed immediately upon completion of
 * whatever local bookkeeping the host server must do to support the
 * function.
 *
 * If the "barrier" parameter is "true", AND the "collect_data" parameter
 * is "true", then the callback function shall return *all* modex data
 * provided by the participants. If the "collect_data" parameter is "false",
 * then the callback shall just return the status */
typedef int (*pmix_server_fencenb_fn_t)(const pmix_range_t ranges[], size_t nranges,
                                        int barrier, int collect_data,
                                        pmix_modex_cbfunc_t cbfunc, void *cbdata);

/* Store modex data for the given scope - should be copied into
 * the host server's storage */
typedef int (*pmix_server_store_modex_fn_t)(pmix_scope_t scope, pmix_modex_data_t *data);

/* Retrieve modex data from the specified rank. A rank value of PMIX_RANK_WILDCARD
 * indicates that all modex data associated with the given nspace is to be
 * returned. */
typedef int (*pmix_server_get_modexnb_fn_t)(const char nspace[], int rank,
                                            pmix_modex_cbfunc_t cbfunc, void *cbdata);


/* Retrieve all job-related info for this nspace and rank. The list of
 * supported data keys is provided in pmix_common.h. Note that the host
 * server is not required to support all of the defined keys, nor is it limited
 * to those that are defined in that file. */
typedef int (*pmix_server_get_job_info_fn_t)(const char nspace[], int rank,
                                             pmix_info_t *info[], size_t *ninfo);


/* Publish data per the PMIx API specification. The callback is to be executed
 * upon completion of the operation. The host server is not required to guarantee
 * support for the requested scope - i.e., the server does not need to return an
 * error if the data store doesn't support scope-based isolation. However, the
 * server must return an error (a) if the key is duplicative within the storage
 * scope, and (b) if the server does not allow overwriting of published info by
 * the original publisher - it is left to the discretion of the host server to
 * allow info-key-based flags to modify this behavior */
typedef int (*pmix_server_publish_fn_t)(pmix_scope_t scope,
                                        const pmix_info_t info[], size_t ninfo,
                                        pmix_op_cbfunc_t cbfunc, void *cbdata);

/* Lookup published data. The host server will be passed a NULL-terminated array
 * of string keys along with the scope within which the data is expected to have
 * been published. The host server is not required to guarantee support for all
 * PMIx-defined scopes, but should only search data stores within the specified
 * scope within the context of the corresponding "publish" API. */
typedef int (*pmix_server_lookup_fn_t)(pmix_scope_t scope, char **keys,
                                       pmix_lookup_cbfunc_t cbfunc, void *cbdata);

/* Delete data from the data store. The host server will be passed a NULL-terminated array
 * of string keys along with the scope within which the data is expected to have
 * been published. The callback is to be executed upon completion of the delete
 * procedure */
typedef int (*pmix_server_unpublish_fn_t)(pmix_scope_t scope, char **keys,
                                          pmix_op_cbfunc_t cbfunc, void *cbdata);

/* Spawn a set of applications/processes as per the PMIx API. Note that
 * applications are not required to be MPI or any other programming model.
 * Thus, the host server cannot make any assumptions as to their required
 * support. The callback function is to be executed once all processes have
 * been started. An error in starting any application or process in this
 * request shall cause all applications and processes in the request to
 * be terminated, and an error returned to the originating caller */
typedef int (*pmix_server_spawn_fn_t)(const pmix_app_t apps[], size_t napps,
                                      pmix_spawn_cbfunc_t cbfunc, void *cbdata);

/* Record the specified processes as "connected". This means that the resource
 * manager should treat the failure of any process in the specified group as
 * a reportable event, and take appropriate action. The callback function is
 * to be called once all participating processes have called connect. Note that
 * a process can only engage in *one* connect operation involving the identical
 * set of ranges at a time. However, a process *can* be simultaneously engaged
 * in multiple connect operations, each involving a different set of ranges */
typedef int (*pmix_server_connect_fn_t)(const pmix_range_t ranges[], size_t nranges,
                                        pmix_op_cbfunc_t cbfunc, void *cbdata);

/* Disconnect a previously connected set of processes. An error should be returned
 * if the specified set of ranges was not previously "connected". As above, a process
 * may be involved in multiple simultaneous disconnect operations. However, a process
 * is not allowed to reconnect to a set of ranges that has not fully completed
 * disconnect - i.e., you have to fully disconnect before you can reconnect to the
 * same group of processes. */
typedef int (*pmix_server_disconnect_fn_t)(const pmix_range_t ranges[], size_t nranges,
                                           pmix_op_cbfunc_t cbfunc, void *cbdata);

typedef struct pmix_server_module_1_0_0_t {
    pmix_server_terminated_fn_t       terminated;
    pmix_server_abort_fn_t            abort;
    pmix_server_fencenb_fn_t          fence_nb;
    pmix_server_store_modex_fn_t      store_modex;
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

/* Initialize the server support library. The library supports
 * two modes of operation:
 *
 * (a) internal comm - the library will provide all comm
 *     between the server and clients. In this mode, the
 *     caller is only responsible for processing the
 *     data provided in the module callback functions. The
 *     server library will spin off its own progress thread
 *     that will "block" when not actively processing
 *     messages.
 *
 * (b) external comm - the library will provide message
 *     preparation support (e.g., packing/unpacking data),
 *     but all comm between server and clients will be
 *     the responsibility of the caller.
 *
 * Input parameters:
 *
 * module - pointer to a pmix_server_module_t structure
 * containing the caller's callback functions
 *
 * use_internal_comm - boolean that is true if the server
 * library is to use its internal comm system, false if the
 * caller will be providing its own messaging support
 */
pmix_status_t PMIx_server_init(pmix_server_module_t *module,
                               int use_internal_comm);

/* Finalize the server support library. If internal comm is
 * in-use, the server will shut it down at this time. All
 * memory usage is released */
pmix_status_t PMIx_server_finalize(void);

/* Setup the environment of a child process to be forked
 * by the host so it can correctly interact with the PMIx
 * server. The PMIx client needs some setup information
 * so it can properly connect back to the server. This function
 * will set appropriate environmental variables for this purpose.
 *
 * In addition, the expected user ID and group ID of the
 * child process helps the server library to properly authenticate
 * clients as they connect by requiring the two values to match.
 * These parameters are only relevant when internal comm is
 * in use, and will be ignored otherwise */
pmix_status_t PMIx_server_setup_fork(const char nspace[], int rank,
                                     uid_t uid, gid_t gid, char ***env);


/****    Message processing for the "lite" version of the  ****
 ****    server convenience library. Note that we must     ****
 ****    preserve the ability of the local server to       ****
 ****    operate asynchronously - thus, the lite library   ****
 ****    cannot block while waiting for a reply to be      ****
 ****    generated                                         ****/

/* retrieve the address assigned by the server library for
 * clients to rendezvous with */
pmix_status_t PMIx_get_rendezvous_address(struct sockaddr_un *address);

/* retrieve the size of the PMIx message header so the host
 * server messaging system can read the correct number of bytes.
 * Note that the host will need to provide these bytes separately
 * to the message processor */
size_t PMIx_message_hdr_size(void);

/* given the header read by the host server, return the number of
 * bytes the client included in the payload of the message so
 * the host server can read them. These bytes will need to be
 * provided separately to the message processor */
size_t PMIx_message_payload_size(char *hdr);

/* define a callback function by which the convenience library can
 * request that a message be sent via the specified socket. The PMIx
 * header will be included in the provided payload */
typedef void (*pmix_send_message_cbfunc_t)(int sd, char *payload, size_t size);

/* given a socket, conduct the client-server authentication protocol
 * to authenticate the requested connection. The function will return
 * PMIX_SUCCESS if the connection is authenticated, and an appropriate
 * PMIx error code if not. If the client is authenticated, it will
 * be sent whatever initial job_info the host server can provide */
pmix_status_t PMIx_server_authenticate_client(int sd, int *rank, pmix_send_message_cbfunc_t snd_msg);

/* process a received PMIx client message, sending any desired return
 * via the provided callback function. Params:
 *
 * sd - the socket where the message was received
 * hdr - the header read by the host server messaging system
 * msg - the payload read by the host server messaging system
 * snd_msg - the function to be called when a reply is to be sent */
pmix_status_t PMIx_server_process_msg(int sd, char *hdr, char *msg,
                                      pmix_send_message_cbfunc_t snd_msg);

END_C_DECLS

#endif
