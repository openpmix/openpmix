/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 *
 * Copyright (c) 2021      Nanook Consulting.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#    include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#    include <sys/stat.h>
#endif
#ifdef HAVE_FCNTL_H
#    include <fcntl.h>
#endif
#include <time.h>

#include "include/pmix_common.h"

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/include/pmix_socket_errno.h"
#include "src/mca/base/pmix_mca_base_var.h"
#include "src/mca/pcompress/pcompress.h"
#include "src/mca/ploc/ploc.h"
#include "src/mca/preg/preg.h"
#include "src/util/alfg.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/name_fns.h"
#include "src/util/output.h"
#include "src/util/pmix_environ.h"

#include "pnet_opa.h"
#include "src/mca/pnet/base/base.h"
#include "src/mca/pnet/pnet.h"

static pmix_status_t opa_init(void);
static void opa_finalize(void);
static pmix_status_t allocate(pmix_namespace_t *nptr, pmix_info_t info[], size_t ninfo,
                              pmix_list_t *ilist);
static pmix_status_t setup_local_network(pmix_namespace_t *nptr, pmix_info_t info[], size_t ninfo);
static pmix_status_t setup_fork(pmix_namespace_t *nptr, const pmix_proc_t *proc, char ***env);
pmix_pnet_module_t pmix_opa_module = {.name = "opa",
                                      .init = opa_init,
                                      .finalize = opa_finalize,
                                      .allocate = allocate,
                                      .setup_local_network = setup_local_network,
                                      .setup_fork = setup_fork};

/* internal structures */
typedef struct {
    pmix_list_item_t super;
    pmix_envar_t envar;
} opa_envar_t;
static void encon(opa_envar_t *p)
{
    PMIX_ENVAR_CONSTRUCT(&p->envar);
}
static void endes(opa_envar_t *p)
{
    PMIX_ENVAR_DESTRUCT(&p->envar);
}
static PMIX_CLASS_INSTANCE(opa_envar_t, pmix_list_item_t, encon, endes);

typedef struct {
    pmix_list_item_t super;
    pmix_nspace_t nspace;
    pmix_list_t envars;
} opa_nspace_t;
static void nscon(opa_nspace_t *p)
{
    PMIX_CONSTRUCT(&p->envars, pmix_list_t);
}
static void nsdes(opa_nspace_t *p)
{
    PMIX_LIST_DESTRUCT(&p->envars);
}
static PMIX_CLASS_INSTANCE(opa_nspace_t, pmix_list_item_t, nscon, nsdes);

/* local variables */
static pmix_list_t mynspaces;

static pmix_status_t opa_init(void)
{
    PMIX_CONSTRUCT(&mynspaces, pmix_list_t);
    return PMIX_SUCCESS;
}

static void opa_finalize(void)
{
    PMIX_LIST_DESTRUCT(&mynspaces);
}

/* some network transports require a little bit of information to
 * "pre-condition" them - i.e., to setup their individual transport
 * connections so they can generate their endpoint addresses. This
 * function provides a means for doing so. The resulting info is placed
 * into the app_context's env array so it will automatically be pushed
 * into the environment of every MPI process when launched.
 */

static inline void transports_use_rand(uint64_t *unique_key)
{
    pmix_rng_buff_t rng;
    pmix_srand(&rng, (unsigned int) time(NULL));
    unique_key[0] = pmix_rand(&rng);
    unique_key[1] = pmix_rand(&rng);
}

static char *transports_print(uint64_t *unique_key)
{
    unsigned int *int_ptr;
    size_t i, j, string_key_len, written_len;
    char *string_key = NULL, *format = NULL;

    /* string is two 64 bit numbers printed in hex with a dash between
     * and zero padding.
     */
    string_key_len = (sizeof(uint64_t) * 2) * 2 + strlen("-") + 1;
    string_key = (char *) malloc(string_key_len);
    if (NULL == string_key) {
        return NULL;
    }

    string_key[0] = '\0';
    written_len = 0;

    /* get a format string based on the length of an unsigned int.  We
     * want to have zero padding for sizeof(unsigned int) * 2
     * characters -- when printing as a hex number, each byte is
     * represented by 2 hex characters.  Format will contain something
     * that looks like %08lx, where the number 8 might be a different
     * number if the system has a different sized long (8 would be for
     * sizeof(int) == 4)).
     */
    if (0 > asprintf(&format, "%%0%dx", (int) (sizeof(unsigned int)) * 2)) {
        return NULL;
    }

    /* print the first number */
    int_ptr = (unsigned int *) &unique_key[0];
    for (i = 0; i < sizeof(uint64_t) / sizeof(unsigned int); ++i) {
        if (0 == int_ptr[i]) {
            /* inject some energy */
            for (j = 0; j < sizeof(unsigned int); j++) {
                int_ptr[i] |= j << j;
            }
        }
        snprintf(string_key + written_len, string_key_len - written_len, format, int_ptr[i]);
        written_len = strlen(string_key);
    }

    /* print the middle dash */
    snprintf(string_key + written_len, string_key_len - written_len, "-");
    written_len = strlen(string_key);

    /* print the second number */
    int_ptr = (unsigned int *) &unique_key[1];
    for (i = 0; i < sizeof(uint64_t) / sizeof(unsigned int); ++i) {
        if (0 == int_ptr[i]) {
            /* inject some energy */
            for (j = 0; j < sizeof(unsigned int); j++) {
                int_ptr[i] |= j << j;
            }
        }
        snprintf(string_key + written_len, string_key_len - written_len, format, int_ptr[i]);
        written_len = strlen(string_key);
    }
    free(format);

    return string_key;
}

/* NOTE: if there is any binary data to be transferred, then
 * this function MUST pack it for transport as the host will
 * not know how to do so */
static pmix_status_t allocate(pmix_namespace_t *nptr, pmix_info_t info[], size_t ninfo,
                              pmix_list_t *ilist)
{
    uint64_t unique_key[2];
    char *string_key;
    int fd_rand;
    size_t bytes_read, n, m, p;
    pmix_buffer_t mydata; // Buffer used to store information to be transmitted (scratch storage)
    pmix_kval_t *kv;
    pmix_envar_t envar;
    pmix_byte_object_t bo;
    bool envars = false, seckeys = false;
    pmix_status_t rc;
    pmix_info_t *iptr;
    pmix_list_t cache;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:opa:allocate for nspace %s", nptr->nspace);

    if (NULL == info) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_SETUP_APP_ENVARS)) {
            envars = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SETUP_APP_ALL)) {
            envars = PMIX_INFO_TRUE(&info[n]);
            seckeys = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SETUP_APP_NONENVARS)) {
            seckeys = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_ALLOC_NETWORK)) {
            iptr = (pmix_info_t *) info[n].value.data.darray->array;
            m = info[n].value.data.darray->size;
            for (p = 0; p < m; p++) {
                if (PMIX_CHECK_KEY(&iptr[p], PMIX_ALLOC_NETWORK_SEC_KEY)) {
                    seckeys = PMIX_INFO_TRUE(&iptr[p]);
                } else if (PMIX_CHECK_KEY(&iptr[p], PMIX_ALLOC_NETWORK_ID)) {
                    /* need to track the request by this ID */
                } else if (PMIX_CHECK_KEY(&iptr[p], PMIX_SETUP_APP_ENVARS)) {
                    envars = PMIX_INFO_TRUE(&iptr[p]);
                } else if (PMIX_CHECK_KEY(&iptr[p], PMIX_SETUP_APP_ALL)) {
                    envars = PMIX_INFO_TRUE(&iptr[p]);
                    seckeys = PMIX_INFO_TRUE(&iptr[p]);
                } else if (PMIX_CHECK_KEY(&iptr[p], PMIX_SETUP_APP_NONENVARS)) {
                    seckeys = PMIX_INFO_TRUE(&iptr[p]);
                }
            }
        }
    }
    /* setup a buffer - we will pack the info into it for transmission to
     * the backend compute node daemons */
    PMIX_CONSTRUCT(&mydata, pmix_buffer_t);

    if (seckeys) {
        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                            "pnet: opa providing seckeys");
        /* put the number here - or else create an appropriate string. this just needs to
         * eventually be a string variable
         */
        if (-1 == (fd_rand = open("/dev/urandom", O_RDONLY))) {
            transports_use_rand(unique_key);
        } else {
            bytes_read = read(fd_rand, (char *) unique_key, 16);
            if (bytes_read != 16) {
                transports_use_rand(unique_key);
            }
            close(fd_rand);
        }

        if (NULL == (string_key = transports_print(unique_key))) {
            PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
            PMIX_DESTRUCT(&mydata);
            return PMIX_ERR_OUT_OF_RESOURCE;
        }

        PMIX_ENVAR_LOAD(&envar, "OMPI_MCA_orte_precondition_transports", string_key, ':');
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &mydata, &envar, 1, PMIX_ENVAR);
        free(string_key);
    }

    if (envars) {
        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                            "pnet: opa harvesting envars %s excluding %s",
                            (NULL == mca_pnet_opa_component.incparms)
                                ? "NONE"
                                : mca_pnet_opa_component.incparms,
                            (NULL == mca_pnet_opa_component.excparms)
                                ? "NONE"
                                : mca_pnet_opa_component.excparms);
        /* harvest envars to pass along */
        PMIX_CONSTRUCT(&cache, pmix_list_t);
        if (NULL != mca_pnet_opa_component.include) {
            rc = pmix_util_harvest_envars(mca_pnet_opa_component.include,
                                          mca_pnet_opa_component.exclude, &cache);
            if (PMIX_SUCCESS != rc) {
                PMIX_LIST_DESTRUCT(&cache);
                PMIX_DESTRUCT(&mydata);
                return rc;
            }
            /* pack anything that was found */
            PMIX_LIST_FOREACH (kv, &cache, pmix_kval_t) {
                PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &mydata, &kv->value->data.envar, 1,
                                 PMIX_ENVAR);
            }
            PMIX_LIST_DESTRUCT(&cache);
        }
    }

    /* load all our results into a buffer for xmission to the backend */
    PMIX_KVAL_NEW(kv, PMIX_PNET_OPA_BLOB);
    if (NULL == kv || NULL == kv->value) {
        PMIX_RELEASE(kv);
        PMIX_DESTRUCT(&mydata);
        return PMIX_ERR_NOMEM;
    }
    kv->value->type = PMIX_BYTE_OBJECT;
    PMIX_UNLOAD_BUFFER(&mydata, bo.bytes, bo.size);
    /* to help scalability, compress this blob */
    if (pmix_compress.compress((uint8_t *) bo.bytes, bo.size,
                               (uint8_t **) &kv->value->data.bo.bytes, &kv->value->data.bo.size)) {
        kv->value->type = PMIX_COMPRESSED_BYTE_OBJECT;
    } else {
        kv->value->data.bo.bytes = bo.bytes;
        kv->value->data.bo.size = bo.size;
    }
    PMIX_DESTRUCT(&mydata);
    pmix_list_append(ilist, &kv->super);
    return PMIX_SUCCESS;
}

/* PMIx_server_setup_local_support calls the "setup_local_network" function.
 * The Standard requires that this come _after_ the host calls the
 * PMIx_server_register_nspace function to ensure that any required information
 * is available to the components. Thus, we have the PMIX_NODE_MAP and
 * PMIX_PROC_MAP available to us and can use them here.
 *
 * When the host calls "setup_local_support", it passes down an array
 * containing the information the "lead" server (e.g., "mpirun") collected
 * from PMIx_server_setup_application. In this case, we search for a blob
 * that our "allocate" function may have included in that info.
 */
static pmix_status_t setup_local_network(pmix_namespace_t *nptr, pmix_info_t info[], size_t ninfo)
{
    size_t n;
    pmix_buffer_t bkt;
    int32_t cnt;
    pmix_info_t *iptr;
    pmix_status_t rc = PMIX_SUCCESS;
    uint8_t *data;
    size_t size;
    bool restore = false;
    opa_nspace_t *ns, *n2;
    opa_envar_t *ev;
    pmix_proc_t proc;
    pmix_kval_t *kv;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:sshot:setup_local_network with %lu info", (unsigned long) ninfo);

    /* prep the unpack buffer */
    PMIX_CONSTRUCT(&bkt, pmix_buffer_t);

    for (n = 0; n < ninfo; n++) {
        /* look for my key */
        if (PMIX_CHECK_KEY(&info[n], PMIX_PNET_OPA_BLOB)) {
            pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                "pnet:opa:setup_local_network found my blob");

            /* cache this so we can restore the payload after processing it.
             * This is necessary as the incoming info array belongs to our
             * host and so we cannot alter it */
            iptr = &info[n];

            /* if this is a compressed byte object, decompress it */
            if (PMIX_COMPRESSED_BYTE_OBJECT == info[n].value.type) {
                pmix_compress.decompress(&data, &size, (uint8_t *) info[n].value.data.bo.bytes,
                                         info[n].value.data.bo.size);
            } else {
                data = (uint8_t *) info[n].value.data.bo.bytes;
                size = info[n].value.data.bo.size;
                restore = true;
            }

            /* this macro NULLs and zero's the incoming bo */
            PMIX_LOAD_BUFFER(pmix_globals.mypeer, &bkt, data, size);

            /* do we have this nspace */
            ns = NULL;
            PMIX_LIST_FOREACH (n2, &mynspaces, opa_nspace_t) {
                if PMIX_CHECK_NSPACE (n2->nspace, nptr->nspace) {
                    ns = n2;
                    break;
                }
            }
            if (NULL == ns) {
                ns = PMIX_NEW(opa_nspace_t);
                PMIX_LOAD_NSPACE(ns->nspace, nptr->nspace);
                pmix_list_append(&mynspaces, &ns->super);
            }

            /* all we packed was envars, so just cycle thru */
            ev = PMIX_NEW(opa_envar_t);
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt, &ev->envar, &cnt, PMIX_ENVAR);
            while (PMIX_SUCCESS == rc) {
                pmix_list_append(&ns->envars, &ev->super);
                /* if this is the transport key, save it */
                if (0
                    == strncmp(ev->envar.envar, "OMPI_MCA_orte_precondition_transports",
                               PMIX_MAX_KEYLEN)) {
                    /* add it to the job-level info */
                    PMIX_LOAD_PROCID(&proc, ns->nspace, PMIX_RANK_WILDCARD);
                    PMIX_KVAL_NEW(kv, PMIX_CREDENTIAL);
                    kv->value->type = PMIX_STRING;
                    kv->value->data.string = ev->envar.value;
                    PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &proc, PMIX_INTERNAL, kv);
                    PMIX_RELEASE(kv); // maintain refcount
                    if (PMIX_SUCCESS != rc) {
                        goto cleanup;
                    }
                }
                /* get the next envar */
                ev = PMIX_NEW(opa_envar_t);
                cnt = 1;
                PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt, &ev->envar, &cnt, PMIX_ENVAR);
            }

            /* we are done */
            break;
        }
    }

cleanup:
    if (restore) {
        /* restore the incoming data */
        iptr->value.data.bo.bytes = bkt.base_ptr;
        iptr->value.data.bo.size = bkt.bytes_used;
    }

    return rc;
}

static pmix_status_t setup_fork(pmix_namespace_t *nptr, const pmix_proc_t *proc, char ***env)
{
    opa_nspace_t *ns, *ns2;
    opa_envar_t *ev;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output, "pnet:opa: setup fork for %s",
                        PMIX_NAME_PRINT(proc));

    /* see if we already have this nspace */
    ns = NULL;
    PMIX_LIST_FOREACH (ns2, &mynspaces, opa_nspace_t) {
        if (PMIX_CHECK_NSPACE(ns2->nspace, proc->nspace)) {
            ns = ns2;
            break;
        }
    }
    if (NULL == ns) {
        /* we don't know anything about this one or
         * it doesn't have any opa data */
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    PMIX_LIST_FOREACH (ev, &ns->envars, opa_envar_t) {
        pmix_setenv(ev->envar.envar, ev->envar.value, true, env);
    }

    return PMIX_SUCCESS;
}
