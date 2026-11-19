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

#include <stdlib.h>
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
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <time.h>

#include "pmix_common.h"

#include "src/class/pmix_list.h"
#include "src/hwloc/pmix_hwloc.h"
#include "src/include/pmix_globals.h"
#include "src/mca/pcompress/pcompress.h"
#include "src/util/pmix_alfg.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_error.h"
#include "src/util/pmix_name_fns.h"
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
static void deregister_nspace(pmix_namespace_t *nptr);
static void finalize(void);
static pmix_status_t collect_inventory(pmix_info_t directives[], size_t ndirs,
                                       void *inventory);
static pmix_status_t deliver_inventory(pmix_info_t info[], size_t ninfo,
                                       pmix_info_t directives[], size_t ndirs);
pmix_pnet_module_t pmix_pnet_opa_module = {
    .name = "opa",
    .finalize = finalize,
    .allocate = allocate,
    .setup_local_network = setup_local_network,
    .setup_fork = setup_fork,
    .deregister_nspace = deregister_nspace,
    .collect_inventory = collect_inventory,
    .deliver_inventory = deliver_inventory
};

/* The HFIs this component is for - what component_open looks for and what
 * setup_fork claims, from one table so the two cannot drift apart.  Both
 * are Omni-Path HFIs, PCI class 0x208, and both are driven by hfi1 and OPX:
 *
 * - CN5000 and later carry Cornelis's own vendor id;
 * - OPA100 carries the id Intel gave it, which is in the silicon, so the
 *   cards in service still report it.
 *
 * The class is part of the question and not just the vendor: Intel's
 * ethernet controllers report 0x200 and are not ours. */
const pmix_pnet_pcimatch_t pmix_pnet_opa_match[] = {
    {0x434e, 0x208, 0},     /* Cornelis CN5000 and later */
    {0x8086, 0x208, 0}      /* Omni-Path 100 */
};
const size_t pmix_pnet_opa_nmatch = sizeof(pmix_pnet_opa_match) / sizeof(pmix_pnet_opa_match[0]);

/* The variable OPX, the libfabric provider for every generation of
 * Omni-Path HFI, reads its job key from.  OPX names its intra-node shared
 * memory segments with the key and hands it to the hfi1 driver as the
 * context's uuid, so processes reach each other through shared memory only
 * when their keys agree.  With the variable unset OPX takes SLURM_JOB_ID,
 * and with that unset too a fixed built-in default. */
#define OPX_UUID_ENVAR "FI_OPX_UUID"

/* One FI_OPX_UUID per namespace.  On the server that sets a job up,
 * allocate records the key it gave the job, so a job spawned from it can
 * be given the same one.  On a daemon, setup_local_network records the key
 * it was sent, kept out of the namespace's envar cache because the base
 * sets that cache with overwrite: a value the child's environment already
 * carries - one the user gave the app - must win, so setup_fork writes
 * this one without overwriting. */
typedef struct {
    pmix_list_item_t super;
    char *nspace;
    char *uuid;
} pnet_opa_uuid_t;

static void uucon(pnet_opa_uuid_t *p)
{
    p->nspace = NULL;
    p->uuid = NULL;
}

static void uudes(pnet_opa_uuid_t *p)
{
    free(p->nspace);
    free(p->uuid);
}
static PMIX_CLASS_INSTANCE(pnet_opa_uuid_t, pmix_list_item_t, uucon, uudes);

static pmix_list_t uuids = PMIX_LIST_STATIC_INIT(uuids);

static pnet_opa_uuid_t *find_uuid(const char *nspace)
{
    pnet_opa_uuid_t *u;

    PMIX_LIST_FOREACH (u, &uuids, pnet_opa_uuid_t) {
        if (0 == strcmp(u->nspace, nspace)) {
            return u;
        }
    }
    return NULL;
}

/* some network transports require a little bit of information to
 * "pre-condition" them - i.e., to setup their individual transport
 * connections so they can generate their endpoint addresses. This
 * function provides a means for doing so. The resulting info is placed
 * into the app_context's env array so it will automatically be pushed
 * into the environment of every MPI process when launched.
 */

/* the fallback for when /dev/urandom cannot be read - a chroot or a
 * container with a restricted /dev, or a process out of descriptors.
 * It cannot be as good as the real thing, but it has to be good enough
 * that two jobs do not end up pre-conditioning their transports with
 * the same key, because nothing downstream would ever say so */
static inline void transports_use_rand(uint64_t *unique_key)
{
    static pmix_rng_buff_t rng;
    static bool seeded = false;
    uint32_t hi, lo;

    pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                        "pnet: opa falling back to a generated transport key");

    if (!seeded) {
        /* seed once and keep drawing from that stream. Re-seeding from
         * the clock on every call would hand two jobs allocated within
         * the same second the identical key.
         *
         * pmix_srand() wants a 32-bit seed, so fold the full width of
         * the time_t into 32 bits rather than silently truncating it -
         * that lets every bit of the clock contribute and remains
         * well-defined whether time_t is 32 or 64 bits wide. Mixing in
         * our pid separates two servers started in the same second */
        uint64_t now = (uint64_t) time(NULL);
        uint32_t seed = (uint32_t) now ^ (uint32_t) (now >> 32) ^ (uint32_t) getpid();
        pmix_srand(&rng, seed);
        seeded = true;
    }
    /* pmix_rand returns 32 bits at a time, and the caller is asking for
     * a 128-bit key - so fill each half of each word rather than
     * leaving the top half of both of them zero */
    hi = pmix_rand(&rng);
    lo = pmix_rand(&rng);
    unique_key[0] = ((uint64_t) hi << 32) | (uint64_t) lo;
    hi = pmix_rand(&rng);
    lo = pmix_rand(&rng);
    unique_key[1] = ((uint64_t) hi << 32) | (uint64_t) lo;
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

/* 128 bits from /dev/urandom, or from the fallback stream when that cannot
 * be read */
static void fill_key(uint64_t *unique_key)
{
    int fd_rand;
    ssize_t bytes_read;

    if (-1 == (fd_rand = open("/dev/urandom", O_RDONLY))) {
        transports_use_rand(unique_key);
    } else {
        bytes_read = read(fd_rand, (char *) unique_key, 2 * sizeof(uint64_t));
        if ((ssize_t) (2 * sizeof(uint64_t)) != bytes_read) {
            transports_use_rand(unique_key);
        }
        close(fd_rand);
    }
}

/* The FI_OPX_UUID for a job, recorded under its namespace.
 *
 * The key is a job's and the jobs it spawns: they have to share it to reach
 * each other through shared memory, and nothing else should share it.  So a
 * job whose request names the process that spawned it (PMIX_PARENT_ID)
 * takes the key of that process's job, which this server gave out when it
 * set that job up - a parent is running when it spawns, so its job has not
 * been deregistered - and any other job gets a fresh one.  Asked again for
 * the same job, it gives the same answer.
 *
 * Jobs launched apart do not share a key, even when they later connect: OPX
 * takes one key per domain and uses it for every peer, so a key handed to a
 * process after it opened its domain has nowhere to go.
 *
 * Printed as 32 hex digits - the most OPX accepts, and the form it parses.
 * The caller frees the returned string. */
static char *opx_job_uuid(const char *nspace, const pmix_proc_t *parent)
{
    pnet_opa_uuid_t *u, *p = NULL;
    uint64_t key[2];
    char *uuid = NULL;

    u = find_uuid(nspace);
    if (NULL != u) {
        return strdup(u->uuid);
    }
    if (NULL != parent && !PMIx_Nspace_invalid(parent->nspace)) {
        p = find_uuid(parent->nspace);
    }
    if (NULL != p) {
        uuid = strdup(p->uuid);
    } else {
        fill_key(key);
        if (0 > pmix_asprintf(&uuid, "%016" PRIx64 "%016" PRIx64, key[0], key[1])) {
            uuid = NULL;
        }
    }
    if (NULL == uuid) {
        return NULL;
    }
    /* remembered, so a job this one spawns can find it */
    u = PMIX_NEW(pnet_opa_uuid_t);
    if (NULL == u) {
        free(uuid);
        return NULL;
    }
    u->nspace = strdup(nspace);
    u->uuid = strdup(uuid);
    if (NULL == u->nspace || NULL == u->uuid) {
        PMIX_RELEASE(u);
        free(uuid);
        return NULL;
    }
    pmix_list_append(&uuids, &u->super);
    return uuid;
}

/* NOTE: if there is any binary data to be transferred, then
 * this function MUST pack it for transport as the host will
 * not know how to do so */
static pmix_status_t allocate(pmix_namespace_t *nptr, pmix_info_t info[], size_t ninfo,
                              pmix_list_t *ilist)
{
    uint64_t unique_key[2];
    char *string_key;
    size_t n, m, p;
    pmix_buffer_t mydata; // Buffer used to store information to be transmitted (scratch storage)
    pmix_kval_t *kv;
    pmix_envar_t envar;
    pmix_byte_object_t bo;
    bool envars = false, seckeys = false;
    const pmix_proc_t *parent = NULL;
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
        } else if (PMIX_CHECK_KEY(&info[n], PMIX_PARENT_ID)) {
            /* a parent we cannot read as a process is no parent */
            if (PMIX_PROC == info[n].value.type && NULL != info[n].value.data.proc) {
                parent = info[n].value.data.proc;
            }
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
        fill_key(unique_key);

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

        /* and OPX's.  It goes after the key above, so a daemon that does
         * not know it merely caches one more envar; and before anything
         * harvested, so an FI_OPX_UUID the user set where the job was
         * launched arrives later in the blob and is the one kept */
        if (NULL == (string_key = opx_job_uuid(nptr->nspace, parent))) {
            PMIX_ERROR_LOG(PMIX_ERR_OUT_OF_RESOURCE);
            PMIX_DESTRUCT(&mydata);
            return PMIX_ERR_OUT_OF_RESOURCE;
        }
        PMIX_ENVAR_LOAD(&envar, OPX_UUID_ENVAR, string_key, ':');
        PMIX_BFROPS_PACK(rc, pmix_globals.mypeer, &mydata, &envar, 1, PMIX_ENVAR);
        free(string_key);
        PMIX_ENVAR_DESTRUCT(&envar);
        if (PMIX_SUCCESS != rc) {
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
    pnet_opa_uuid_t *u;

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
                if (NULL != ev->envar.envar && NULL != ev->envar.value
                    && 0 == strcmp(ev->envar.envar, OPX_UUID_ENVAR)) {
                    /* OPX's job key is set by our own setup_fork, which
                     * does not overwrite - see pnet_opa_uuid_t.  A later
                     * one in the blob replaces an earlier: allocate packs
                     * its own first and anything harvested after it */
                    u = find_uuid(ns->ns->nspace);
                    if (NULL == u) {
                        u = PMIX_NEW(pnet_opa_uuid_t);
                        u->nspace = strdup(ns->ns->nspace);
                        pmix_list_append(&uuids, &u->super);
                    }
                    free(u->uuid);
                    u->uuid = strdup(ev->envar.value);
                    PMIX_RELEASE(ev);
                } else {
                    /* the other envars - the transport key among them - are
                     * kept here, on the namespace's pnet cache, and set in
                     * each of the job's children by setup_fork. They are not
                     * job data: nothing but that child's environment needs
                     * them */
                    pmix_list_append(&ns->envars, &ev->super);
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

/* The OPX unit number of an HFI, or -1 if the name does not give one.
 *
 * OPX takes a unit number in FI_OPX_HFI_SELECT, and every path by which it
 * opens a unit builds the device's name from that number: /dev/hfi1_<N>,
 * /sys/class/infiniband/hfi1_<N>, and the verbs device "hfi1_<N>".  There is
 * no enumeration of OPX's own behind it, so the number is the one in the
 * name rather than an ordinal that usually agrees with it.  The name must
 * be exactly that form - hfi1_, then decimal digits with no leading zero -
 * since nothing else is what OPX would build back from the number. */
static int opx_unit_of(const char *name)
{
    const char *digits;
    char *end = NULL;
    long unit;

    if (NULL == name || 0 != strncmp(name, "hfi1_", 5)) {
        return -1;
    }
    digits = name + 5;
    if (!isdigit((unsigned char) digits[0]) || ('0' == digits[0] && '\0' != digits[1])) {
        return -1;
    }
    errno = 0;
    unit = strtol(digits, &end, 10);
    if (0 != errno || NULL == end || '\0' != *end || unit > INT_MAX) {
        return -1;
    }
    return (int) unit;
}

/* Would OPX open some other device for this unit?
 *
 * OPX's verbs path finds the unit's device by comparing "hfi1_<N>" with
 * each device name over the length of "hfi1_<N>" and taking the first that
 * matches, so unit 1 is also matched by hfi1_10 through hfi1_15, and which
 * one it opens depends on the order the device list comes back in.  The
 * unit can therefore be written only when no other device on this node
 * begins with the assigned name.  If the node's devices cannot be listed,
 * assume the worst. */
static bool opx_prefix_collides(const char *name)
{
    pmix_hwloc_device_t *devs = NULL;
    size_t ndevs = 0, d, len = strlen(name);
    const char *osname;
    bool hit = false;
    pmix_status_t rc;

    rc = pmix_hwloc_get_devices(&pmix_globals.topology, pmix_globals.hostname,
                                PMIX_DEVTYPE_OPENFABRICS, NULL, &devs, &ndevs);
    if (PMIX_SUCCESS != rc) {
        return true;
    }
    for (d = 0; d < ndevs; d++) {
        osname = devs[d].dev.osname;
        if (NULL == osname || 0 == strcmp(osname, name)) {
            continue;
        }
        if (0 == strncmp(osname, name, len)) {
            pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                "pnet:opa: not setting FI_OPX_HFI_SELECT - "
                                "OPX would also take %s for %s", osname, name);
            hit = true;
            break;
        }
    }
    pmix_hwloc_release_devices(devs, ndevs);
    return hit;
}

/* Hand this process its job key, and name its HFI to the fabric software
 * that will use it.
 *
 * The key comes first, and goes to every process of the job whether or not
 * it was mapped against an HFI: OPX needs it either way.  It is not
 * written over a value the process's environment already carries, which
 * can only be one somebody gave it on purpose.
 *
 * The HFI is named to OPX, the libfabric provider for every generation of
 * the hardware, in FI_OPX_HFI_SELECT - and only when the process was given
 * exactly one.  That variable takes a unit number, which is the number in
 * the device's name - see opx_unit_of() - so the assignment can be said
 * exactly, unless another device's name begins with this one's (see
 * opx_prefix_collides()).  A list of units would not say "these", only
 * "the first of these": OPX opens the first fixed unit it reads and stops,
 * so a process given several is left to OPX's own selection, which is less
 * than was asked for but not wrong.  The variable is written over any value
 * already set: a process mapped against a device made the more specific
 * request.
 *
 * PSM2's HFI_UNIT is deliberately NOT set.  PSM2 numbers its units by its
 * own enumeration, which nothing here can see, and a wrong value there does
 * not fail - it quietly puts the process on somebody else's HFI.
 */
static pmix_status_t setup_fork(const pmix_proc_t *proc, char ***env)
{
    pnet_opa_uuid_t *u;
    pmix_status_t rc;
    char *val = NULL, *sel = NULL;
    int unit;

    u = find_uuid(proc->nspace);
    if (NULL != u) {
        rc = PMIx_Setenv(OPX_UUID_ENVAR, u->uuid, false, env);
        if (PMIX_ERR_EXISTS == rc) {
            pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                                "pnet:opa: %s already has %s - leaving it",
                                PMIX_NAME_PRINT(proc), OPX_UUID_ENVAR);
        } else if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            return rc;
        }
    }

    rc = pmix_pnet_base_get_assigned_devices(proc, pmix_pnet_opa_match, pmix_pnet_opa_nmatch,
                                             &val);
    if (PMIX_SUCCESS != rc) {
        /* most processes are mapped against no HFI at all */
        return rc;
    }

    unit = opx_unit_of(val);
    if (0 <= unit && !opx_prefix_collides(val)) {
        if (0 > pmix_asprintf(&sel, "%d", unit)) {
            free(val);
            return PMIX_ERR_NOMEM;
        }
        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                            "pnet: FI_OPX_HFI_SELECT=%s for %s", sel, PMIX_NAME_PRINT(proc));
        rc = PMIx_Setenv("FI_OPX_HFI_SELECT", sel, true, env);
        free(sel);
        if (PMIX_SUCCESS != rc) {
            PMIX_ERROR_LOG(rc);
            free(val);
            return rc;
        }
    } else if (0 > unit) {
        pmix_output_verbose(2, pmix_pnet_base_framework.framework_output,
                            "pnet:opa: not setting FI_OPX_HFI_SELECT for %s - \"%s\" is not "
                            "the name of a single OPX unit", PMIX_NAME_PRINT(proc), val);
    }

    free(val);
    return PMIX_SUCCESS;
}

/* the job is gone, and so is any reason to remember its key */
static void deregister_nspace(pmix_namespace_t *nptr)
{
    pnet_opa_uuid_t *u;

    if (NULL == nptr || NULL == nptr->nspace) {
        return;
    }
    u = find_uuid(nptr->nspace);
    if (NULL != u) {
        pmix_list_remove_item(&uuids, &u->super);
        PMIX_RELEASE(u);
    }
}

/* emptied rather than destructed, so the list is ready for the next
 * PMIx_server_init in this process */
static void finalize(void)
{
    pnet_opa_uuid_t *u;

    while (NULL != (u = (pnet_opa_uuid_t *) pmix_list_remove_first(&uuids))) {
        PMIX_RELEASE(u);
    }
}

static pmix_status_t collect_inventory(pmix_info_t directives[], size_t ndirs,
                                       void *inventory)
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
