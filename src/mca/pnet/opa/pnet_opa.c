/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <string.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <time.h>

#if PMIX_WANT_OPAMGT
#include <opamgt/opamgt.h>
#include <opamgt/opamgt_sa.h>
#endif

#include "include/pmix_common.h"

#include "src/mca/base/pmix_mca_base_var.h"
#include "src/class/pmix_list.h"
#include "src/include/pmix_socket_errno.h"
#include "src/include/pmix_globals.h"
#include "src/class/pmix_list.h"
#include "src/util/alfg.h"
#include "src/util/argv.h"
#include "src/util/error.h"
#include "src/util/output.h"
#include "src/util/pmix_environ.h"
#include "src/mca/preg/preg.h"
#include "src/hwloc/hwloc-internal.h"

#include "src/mca/pnet/pnet.h"
#include "src/mca/pnet/base/base.h"
#include "pnet_opa.h"

static pmix_status_t allocate(pmix_namespace_t *nptr,
                              pmix_info_t info[], size_t ninfo,
                              pmix_list_t *ilist);

pmix_pnet_module_t pmix_opa_module = {
    .name = "opa",
    .allocate = allocate
};

/* some network transports require a little bit of information to
 * "pre-condition" them - i.e., to setup their individual transport
 * connections so they can generate their endpoint addresses. This
 * function provides a means for doing so. The resulting info is placed
 * into the app_context's env array so it will automatically be pushed
 * into the environment of every MPI process when launched.
 */

static inline void transports_use_rand(uint64_t* unique_key) {
    pmix_rng_buff_t rng;
    pmix_srand(&rng,(unsigned int)time(NULL));
    unique_key[0] = pmix_rand(&rng);
    unique_key[1] = pmix_rand(&rng);
}

static char* transports_print(uint64_t *unique_key)
{
    unsigned int *int_ptr;
    size_t i, j, string_key_len, written_len;
    char *string_key = NULL, *format = NULL;

    /* string is two 64 bit numbers printed in hex with a dash between
     * and zero padding.
     */
    string_key_len = (sizeof(uint64_t) * 2) * 2 + strlen("-") + 1;
    string_key = (char*) malloc(string_key_len);
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
    if (0 > asprintf(&format, "%%0%dx", (int)(sizeof(unsigned int)) * 2)) {
        return NULL;
    }

    /* print the first number */
    int_ptr = (unsigned int*) &unique_key[0];
    for (i = 0 ; i < sizeof(uint64_t) / sizeof(unsigned int) ; ++i) {
        if (0 == int_ptr[i]) {
            /* inject some energy */
            for (j=0; j < sizeof(unsigned int); j++) {
                int_ptr[i] |= j << j;
            }
        }
        snprintf(string_key + written_len,
                 string_key_len - written_len,
                 format, int_ptr[i]);
        written_len = strlen(string_key);
    }

    /* print the middle dash */
    snprintf(string_key + written_len, string_key_len - written_len, "-");
    written_len = strlen(string_key);

    /* print the second number */
    int_ptr = (unsigned int*) &unique_key[1];
    for (i = 0 ; i < sizeof(uint64_t) / sizeof(unsigned int) ; ++i) {
        if (0 == int_ptr[i]) {
            /* inject some energy */
            for (j=0; j < sizeof(unsigned int); j++) {
                int_ptr[i] |= j << j;
            }
        }
        snprintf(string_key + written_len,
                 string_key_len - written_len,
                 format, int_ptr[i]);
        written_len = strlen(string_key);
    }
    free(format);

    return string_key;
}

/* NOTE: if there is any binary data to be transferred, then
 * this function MUST pack it for transport as the host will
 * not know how to do so */
static pmix_status_t allocate(pmix_namespace_t *nptr,
                              pmix_info_t info[], size_t ninfo,
                              pmix_list_t *ilist)
{
    uint64_t unique_key[2];
    char *string_key;
    int fd_rand;
    size_t bytes_read, n, m, p;
    pmix_kval_t *kv;
    bool envars = false, seckeys = false;
    pmix_status_t rc;
    pmix_info_t *iptr;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:opa:allocate for nspace %s", nptr->nspace);

    if (NULL == info) {
        return PMIX_ERR_TAKE_NEXT_OPTION;
    }

    for (n=0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_SETUP_APP_ENVARS)) {
            envars = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SETUP_APP_ALL)) {
            envars = PMIX_INFO_TRUE(&info[n]);
            seckeys = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SETUP_APP_NONENVARS)) {
            seckeys = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_ALLOC_NETWORK)) {
            iptr = (pmix_info_t*)info[n].value.data.darray->array;
            m = info[n].value.data.darray->size;
            for (p=0; p < m; p++) {
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

    if (seckeys) {
        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                            "pnet: opa providing seckeys");
        /* put the number here - or else create an appropriate string. this just needs to
         * eventually be a string variable
         */
        if(-1 == (fd_rand = open("/dev/urandom", O_RDONLY))) {
            transports_use_rand(unique_key);
        } else {
            bytes_read = read(fd_rand, (char *) unique_key, 16);
            if(bytes_read != 16) {
                transports_use_rand(unique_key);
            }
            close(fd_rand);
        }

        if (NULL == (string_key = transports_print(unique_key))) {
            PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
            return PMIX_ERR_OUT_OF_RESOURCE;
        }

        kv = PMIX_NEW(pmix_kval_t);
        if (NULL == kv) {
            free(string_key);
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
        kv->key = strdup(PMIX_SET_ENVAR);
        kv->value = (pmix_value_t*)malloc(sizeof(pmix_value_t));
        if (NULL == kv->value) {
            free(string_key);
            PMIX_RELEASE(kv);
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
        kv->value->type = PMIX_ENVAR;
        PMIX_ENVAR_LOAD(&kv->value->data.envar, "OPA_TRANSPORT_KEY", string_key, ':');
        pmix_list_append(ilist, &kv->super);
        free(string_key);
    }

    if (envars) {
        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                            "pnet: opa harvesting envars %s excluding %s",
                            (NULL == mca_pnet_opa_component.incparms) ? "NONE" : mca_pnet_opa_component.incparms,
                            (NULL == mca_pnet_opa_component.excparms) ? "NONE" : mca_pnet_opa_component.excparms);
        /* harvest envars to pass along */
        if (NULL != mca_pnet_opa_component.include) {
            rc = pmix_util_harvest_envars(mca_pnet_opa_component.include,
                                          mca_pnet_opa_component.exclude,
                                          ilist);
            if (PMIX_SUCCESS != rc) {
                return rc;
            }
        }
    }

    /* we don't currently manage OPA resources */
    return PMIX_ERR_TAKE_NEXT_OPTION;
}
