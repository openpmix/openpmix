/*
 * Copyright (c) 2015      Intel, Inc.  All rights reserved.
 *
 * NOTE: THE MUNGE CLIENT LIBRARY (libmunge) IS LICENSED AS LGPL
 *
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "pmix_config.h"

#include "src/api/pmix_common.h"
#include "src/include/pmix_globals.h"
#include "src/util/argv.h"
#include "src/util/output.h"
#include "src/usock/usock.h"

#include <unistd.h>
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#include <munge.h>

#include "pmix_sec.h"
#include "pmix_munge.h"

static int munge_init(void);
static void munge_finalize(void);
static char* create_cred(void);
static int validate_cred(pmix_peer_t *peer, char *cred);

pmix_sec_base_module_t pmix_munge_module = {
    "munge",
    munge_init,
    munge_finalize,
    create_cred,
    NULL,
    validate_cred,
    NULL
};

static int munge_init(void)
{
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "sec: munge init");
    return PMIX_SUCCESS;
}

static void munge_finalize(void)
{
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "sec: munge finalize");
}

static char* create_cred(void)
{
    munge_err_t rc;
    char *cred;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "sec: munge create_cred");

    if (EMUNGE_SUCCESS != (rc = munge_encode(&cred, NULL, NULL, 0))) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "sec: munge failed to create credential: %s",
                            munge_strerror(rc));
        return NULL;
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "sec: using credential %s", cred);
    
    return cred;
}

static int validate_cred(pmix_peer_t *peer, char *cred)
{
    uid_t uid;
    gid_t gid;
    munge_err_t rc;
    
    pmix_output_verbose(2, pmix_globals.debug_output,
                        "sec: munge validate_cred %s", cred);

    /* parse the inbound string */
    if (EMUNGE_SUCCESS != (rc = munge_decode(cred, NULL, NULL, NULL, &uid, &gid))) {
        pmix_output_verbose(2, pmix_globals.debug_output,
                            "sec: munge failed to decode credential: %s",
                            munge_strerror(rc));
        return PMIX_ERR_INVALID_CRED;
    }

    /* check uid */
    if (uid != peer->uid) {
        return PMIX_ERR_INVALID_CRED;
    }

    /* check guid */
    if (gid != peer->gid) {
        return PMIX_ERR_INVALID_CRED;
    }

    pmix_output_verbose(2, pmix_globals.debug_output,
                        "sec: munge credential valid");
    return PMIX_SUCCESS;
}

