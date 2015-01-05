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
pmix_status_t PMIx_Init(char namespace[], int *rank,
                        struct event_base *evbase,
                        char *mycredential);

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
 * that all modex data is to be included in the callback. Returned data
 * is locally cached so that subsequent calls to PMIx_Get can be serviced
 * without communicating to/from the server, but at the cost of increased
 * memory footprint */
pmix_status_t PMIx_Fence_nb(const pmix_range_t ranges[], size_t nranges,
                            int barrier, int collect_data,
                            pmix_cbfunc_t cbfunc, void *cbdata);

/* Put */
pmix_status_t PMIx_Put(pmix_scope_t scope, const char key[], pmix_value_t *val);

/* Get */
pmix_status_t PMIx_Get(const char namespace[], int rank,
                       const char key[], pmix_value_t **val);

/* Get_nb */
pmix_status_t PMIx_Get_nb(const char namespace[], int rank,
                          const char key[],
                          pmix_cbfunc_t cbfunc, void *cbdata);

/* Publish - the "info" parameter
 * consists of an array of pmix_info_t objects that
 * are to be pushed to the "universal" namespace
 * for lookup by others subject to the provided scope.
 * Note that the keys must be unique within the specified
 * scope or else an error will be returned (first published
 * wins). */
pmix_status_t PMIx_Publish(pmix_scope_t scope,
                           const pmix_info_t info[],
                           size_t ninfo);

/* Lookup - the "info" parameter consists of an array of
 * pmix_info_t objects that specify the requested
 * information. Info will be returned in the provided objects.
 * The namespace param will contain the namespace of
 * the process that published the service_name. Passing
 * a NULL for the namespace param indicates that the
 * namespace information need not be returned */
pmix_status_t PMIx_Lookup(pmix_scope_t scope,
                          pmix_info_t info[], size_t ninfo,
                          char namespace[]);

/* Unpublish - the "info" parameter
 * consists of an array of pmix_info_t objects */
pmix_status_t PMIx_Unpublish(pmix_scope_t scope,
                             const pmix_info_t info[],
                             size_t ninfo);

/* Spawn a new job */
pmix_status_t PMIx_Spawn(const pmix_app_t apps[],
                         size_t napps,
                         char namespace[]);

/* register an errhandler to report errors that occur
 * within the client library, but are not reportable
 * via the API itself (e.g., loss of connection to
 * the server) */
void PMIx_Register_errhandler(pmix_errhandler_fn_t errhandler);

/* deregister the errhandler */
void PMIx_Deregister_errhandler(void);

/* connect */
pmix_status_t PMIx_Connect(const pmix_range_t ranges[], size_t nranges);

/* disconnect */
pmix_status_t PMIx_Disconnect(const pmix_range_t ranges[], size_t nranges);

/* Key-Value pair management macros */
// TODO: add all possible types/fields here.

#define PMIX_VAL_FIELD_int(x) ((x)->data.integer)
#define PMIX_VAL_FIELD_uint32(x) ((x)->data.uint32)
#define PMIX_VAL_FIELD_uint16(x) ((x)->data.uint16)
#define PMIX_VAL_FIELD_string(x) ((x)->data.string)
#define PMIX_VAL_FIELD_float(x) ((x)->data.fval)

#define PMIX_VAL_TYPE_int    PMIX_INT
#define PMIX_VAL_TYPE_uint32 PMIX_UINT32
#define PMIX_VAL_TYPE_uint16 PMIX_UINT16
#define PMIX_VAL_TYPE_string PMIX_STRING
#define PMIX_VAL_TYPE_float  PMIX_FLOAT

#define PMIX_VAL_set_assign(_v, _field, _val )   \
    do {                                                            \
        (_v)->type = PMIX_VAL_TYPE_ ## _field;                      \
        PMIX_VAL_FIELD_ ## _field((_v)) = _val;                     \
    } while(0);

#define PMIX_VAL_set_strdup(_v, _field, _val )       \
    do {                                                                \
        (_v)->type = PMIX_VAL_TYPE_ ## _field;                          \
        PMIX_VAL_FIELD_ ## _field((_v)) = strdup(_val);                 \
    } while(0);

#define PMIX_VAL_SET_int     PMIX_VAL_set_assign
#define PMIX_VAL_SET_uint32  PMIX_VAL_set_assign
#define PMIX_VAL_SET_uint16  PMIX_VAL_set_assign
#define PMIX_VAL_SET_string  PMIX_VAL_set_strdup
#define PMIX_VAL_SET_float   PMIX_VAL_set_assign

#define PMIX_VAL_SET(_v, _field, _val )   \
    PMIX_VAL_SET_ ## _field(_v, _field, _val)

#define PMIX_VAL_FREE(_v) \
     PMIx_free_value_data(_v)

END_C_DECLS
#endif
