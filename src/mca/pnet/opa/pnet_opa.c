/*
 * Copyright (c) 2015-2020 Intel, Inc.  All rights reserved.
 * Copyright (c) 2016      IBM Corporation.  All rights reserved.
 *
 * Copyright (c) 2021-2026 Nanook Consulting  All rights reserved.
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

#include "pmix_common.h"

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/mca/pcompress/pcompress.h"
#include "src/util/pmix_alfg.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"

#include "pnet_opa.h"
#include "src/mca/pnet/base/base.h"
#include "src/mca/pnet/pnet.h"

static pmix_status_t allocate(pmix_namespace_t *nptr,
                              pmix_info_t info[], size_t ninfo,
                              pmix_list_t *ilist);
static pmix_status_t setup_local_network(pmix_nspace_env_cache_t *nptr,
                                         pmix_info_t info[], size_t ninfo);
static pmix_status_t setup_fork(const pmix_proc_t *proc, char ***env);
static pmix_status_t collect_inventory(pmix_info_t directives[], size_t ndirs,
                                       pmix_list_t *inventory);
static pmix_status_t deliver_inventory(pmix_info_t info[], size_t ninfo,
                                       pmix_info_t directives[], size_t ndirs);
pmix_pnet_module_t pmix_pnet_opa_module = {
    .name = "opa",
    .allocate = allocate,
    .setup_local_network = setup_local_network,
    .setup_fork = setup_fork,
    .collect_inventory = collect_inventory,
    .deliver_inventory = deliver_inventory
};

/* The PCI family this component opened for - see component_open.  Intel
 * also makes ethernet controllers, which report class 0x200 and are not
 * what this component is for, so the class is part of the question and not
 * just the vendor. */
static const pmix_pnet_pcimatch_t mymatch[] = {
    {0x8086, 0x208}         /* Intel fabric controller (Omni-Path HFI) */
};

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
    /* pmix_srand() wants a 32-bit seed, so fold the full width of the
     * time_t into 32 bits rather than silently truncating it - this
     * lets every bit of the clock contribute to the seed and remains
     * well-defined whether time_t is 32 or 64 bits wide */
    uint64_t now = (uint64_t) time(NULL);
    uint32_t seed = (uint32_t) now ^ (uint32_t) (now >> 32);
    pmix_srand(&rng, seed);
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
    if (0 > pmix_asprintf(&format, "%%0%dx", (int) (sizeof(unsigned int)) * 2)) {
        free(string_key);
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
        pmix_snprintf(string_key + written_len, string_key_len - written_len, format, int_ptr[i]);
        written_len = strlen(string_key);
    }

    /* print the middle dash */
    pmix_snprintf(string_key + written_len, string_key_len - written_len, "-");
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
        pmix_snprintf(string_key + written_len, string_key_len - written_len, format, int_ptr[i]);
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
    ssize_t bytes_read;
    size_t n, m, p;
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

    PMIX_ENVAR_CONSTRUCT(&envar);

    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], PMIX_SETUP_APP_ENVARS)) {
            envars = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SETUP_APP_ALL)) {
            envars = PMIX_INFO_TRUE(&info[n]);
            seckeys = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_SETUP_APP_NONENVARS)) {
            seckeys = PMIX_INFO_TRUE(&info[n]);
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_ALLOC_NETWORK)) {
            /* This one is deprecated, which is precisely why its shape has
             * to be checked rather than assumed: nothing else in the
             * library reads it, so a host that gets the type wrong is told
             * so by nobody, and reading a string's bytes as a
             * pmix_data_array_t walks a wild pointer for a garbage count. */
            if (PMIX_DATA_ARRAY != info[n].value.type
                || NULL == info[n].value.data.darray
                || PMIX_INFO != info[n].value.data.darray->type
                || NULL == info[n].value.data.darray->array) {
                continue;
            }
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
            if (16 != bytes_read) {
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
        PMIX_ENVAR_DESTRUCT(&envar);
        if (PMIX_SUCCESS != rc) {
            /* every process in the job has to agree on this key, so a job
             * that launches without it is not a job that runs slower - it
             * is one whose transports were never pre-conditioned */
            PMIX_ERROR_LOG(rc);
            PMIX_DESTRUCT(&mydata);
            return rc;
        }
    }

    if (envars) {
        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                            "pnet: opa harvesting envars %s excluding %s",
                            (NULL == pmix_mca_pnet_opa_component.incparms)
                                ? "NONE"
                                : pmix_mca_pnet_opa_component.incparms,
                            (NULL == pmix_mca_pnet_opa_component.excparms)
                                ? "NONE"
                                : pmix_mca_pnet_opa_component.excparms);
        /* harvest envars to pass along */
        PMIX_CONSTRUCT(&cache, pmix_list_t);
        if (NULL != pmix_mca_pnet_opa_component.include) {
            rc = pmix_util_harvest_envars(pmix_mca_pnet_opa_component.include,
                                          pmix_mca_pnet_opa_component.exclude, &cache);
            if (PMIX_SUCCESS != rc) {
                PMIX_LIST_DESTRUCT(&cache);
                PMIX_DESTRUCT(&mydata);
                return rc;
            }
            /* pack anything that was found */
            PMIX_LIST_FOREACH (kv, &cache, pmix_kval_t) {
                PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &mydata,
                                 &kv->value->data.envar, 1, PMIX_ENVAR);
                if (PMIX_SUCCESS != rc) {
                    /* a short blob is worse than no blob: the compute node
                     * would set the envars that made it into the buffer and
                     * never learn that the rest were dropped */
                    PMIX_ERROR_LOG(rc);
                    PMIX_LIST_DESTRUCT(&cache);
                    PMIX_DESTRUCT(&mydata);
                    return rc;
                }
            }
            PMIX_LIST_DESTRUCT(&cache);
        }
    }

    // unload the buffer
    PMIX_UNLOAD_BUFFER(&mydata, bo.bytes, bo.size);
    // if nothing was packed, then we have nothing to send
    if (0 == bo.size) {
        free(bo.bytes);
        PMIX_DESTRUCT(&mydata);
        return PMIX_SUCCESS;
    }

    /* load all our results into a buffer for xmission to the backend */
    PMIX_KVAL_NEW(kv, PMIX_PNET_OPA_BLOB);
    if (NULL == kv || NULL == kv->value) {
        free(bo.bytes);
        PMIX_DESTRUCT(&mydata);
        return PMIX_ERR_NOMEM;
    }
    kv->value->type = PMIX_BYTE_OBJECT;
    /* to help scalability, compress this blob */
    if (pmix_compress.compress((uint8_t *) bo.bytes, bo.size,
                               (uint8_t **) &kv->value->data.bo.bytes, &kv->value->data.bo.size)) {
        kv->value->type = PMIX_COMPRESSED_BYTE_OBJECT;
        /* the compressed copy now holds the payload - release the
         * uncompressed buffer we unloaded above */
        free(bo.bytes);
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
static pmix_status_t setup_local_network(pmix_nspace_env_cache_t *ns,
                                         pmix_info_t info[], size_t ninfo)
{
    size_t n;
    pmix_buffer_t bkt;
    int32_t cnt;
    pmix_status_t rc = PMIX_SUCCESS;
    uint8_t *data = NULL;
    size_t size = 0;
    bool release = false;
    pmix_envar_list_item_t *ev;
    pmix_proc_t proc;
    pmix_kval_t *kv;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet:opa:setup_local with %lu info", (unsigned long) ninfo);

    /* prep the unpack buffer */
    PMIX_CONSTRUCT(&bkt, pmix_buffer_t);

    for (n = 0; n < ninfo; n++) {
        /* look for my key */
        if (PMIX_CHECK_KEY(&info[n], PMIX_PNET_OPA_BLOB)) {
            pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                "pnet:opa:setup_local found my blob");

            /* if this is a compressed byte object, decompress it */
            if (PMIX_COMPRESSED_BYTE_OBJECT == info[n].value.type) {
                /* this fails on a node that built no compression component
                 * as well as on a damaged blob, and it leaves the output
                 * pointer untouched - so we must not go on to read, or
                 * free, whatever it did not write */
                if (!pmix_compress.decompress(&data, &size,
                                              (uint8_t *) info[n].value.data.bo.bytes,
                                              info[n].value.data.bo.size)) {
                    PMIX_ERROR_LOG(PMIX_ERR_UNPACK_FAILURE);
                    return PMIX_ERR_UNPACK_FAILURE;
                }
                release = true;
            } else {
                data = (uint8_t *) info[n].value.data.bo.bytes;
                size = info[n].value.data.bo.size;
            }
            PMIX_LOAD_BUFFER_NON_DESTRUCT(pmix_globals.mypeer, &bkt, data, size);

            /* all we packed was envars, so just cycle thru */
            ev = PMIX_NEW(pmix_envar_list_item_t);
            cnt = 1;
            PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt, &ev->envar, &cnt, PMIX_ENVAR);
            while (PMIX_SUCCESS == rc) {
                pmix_list_append(&ns->envars, &ev->super);
                /* if this is the transport key, save it.  An envar packs
                 * its name and value independently, so either can arrive
                 * NULL from a blob we did not build ourselves */
                if (NULL != ev->envar.envar && NULL != ev->envar.value
                    && 0 == strcmp(ev->envar.envar,
                                   "OMPI_MCA_orte_precondition_transports")) {
                    /* add it to the job-level info */
                    PMIX_LOAD_PROCID(&proc, ns->ns->nspace, PMIX_RANK_WILDCARD);
                    PMIX_KVAL_NEW(kv, PMIX_CREDENTIAL);
                    if (NULL != kv && NULL != kv->value) {
                        kv->value->type = PMIX_STRING;
                        kv->value->data.string = strdup(ev->envar.value);
                        PMIX_GDS_STORE_KV(rc, pmix_globals.mypeer, &proc, PMIX_INTERNAL, kv);
                        PMIX_RELEASE(kv); // maintain refcount
                        if (PMIX_SUCCESS != rc) {
                            PMIX_ERROR_LOG(rc);
                        }
                    }
                }
                /* get the next envar */
                ev = PMIX_NEW(pmix_envar_list_item_t);
                cnt = 1;
                PMIX_BFROPS_UNPACK(rc, pmix_globals.mypeer, &bkt, &ev->envar, &cnt, PMIX_ENVAR);
            }
            if (PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER == rc) {
                rc = PMIX_SUCCESS;
            }
            // we will have created one more envar than we want
            PMIX_RELEASE(ev);

            /* we are done */
            break;
        }
    }

    if (release) {
        free(data);
    }

    return rc;
}

/* Name this process's HFIs to the fabric software that will use them.
 *
 * PSM3 selects by NIC name, which is the selector the topology already
 * supplies, so that variable can be set from the assignment as it stands.
 *
 * PSM2's HFI_UNIT and OPX's FI_OPX_HFI_SELECT are deliberately NOT set.
 * Both take a unit *ordinal* rather than a name, and an ordinal is
 * meaningful only against the enumeration it came from - which is the
 * driver's, not one PMIx performed.  The trailing digit of "hfi1_0" looks
 * like that ordinal and usually is, but "usually" is the wrong standard
 * here: a wrong value in these variables does not fail, it quietly puts the
 * process on somebody else's HFI.  If hwloc ever records the unit the
 * driver reported, as it does for Level Zero devices, this is where it
 * would be used.
 */
static pmix_status_t setup_fork(const pmix_proc_t *proc, char ***env)
{
    return pmix_pnet_base_set_assigned_devices(proc, mymatch,
                                               sizeof(mymatch) / sizeof(mymatch[0]),
                                               "PSM3_NIC", env);
}

static pmix_status_t collect_inventory(pmix_info_t directives[], size_t ndirs,
                                       pmix_list_t *inventory)
{
    /* search the topology for OPA NICs */
    PMIX_HIDE_UNUSED_PARAMS(directives, ndirs, inventory);

    return PMIX_SUCCESS;
}

static pmix_status_t deliver_inventory(pmix_info_t info[], size_t ninfo,
                                       pmix_info_t directives[], size_t ndirs)
{
    /* look for our inventory blob */
    PMIX_HIDE_UNUSED_PARAMS(info, ninfo, directives, ndirs);

    return PMIX_SUCCESS;
}
