/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_UNISTD_H
#    include <unistd.h>
#endif

#include "pmix.h"
#include "src/util/pmix_idname.h"

/* the string is a number if it is nothing but decimal digits */
static bool numeric(const char *str, uint32_t *id, pmix_status_t *rc)
{
    unsigned long val;
    char *end;
    const char *p;

    for (p = str; '\0' != *p; p++) {
        if ('0' > *p || '9' < *p) {
            return false;
        }
    }
    errno = 0;
    val = strtoul(str, &end, 10);
    if (0 != errno || (unsigned long) UINT32_MAX < val) {
        *rc = PMIX_ERR_BAD_PARAM;
    } else {
        *id = (uint32_t) val;
        *rc = PMIX_SUCCESS;
    }
    return true;
}

/* a buffer big enough for one passwd or group entry */
static size_t entry_bufsize(int which)
{
    long sz = sysconf(which);

    return (0 < sz) ? (size_t) sz : 16384;
}

pmix_status_t pmix_util_uid_from_string(const char *str, uint32_t *uid)
{
    struct passwd pw, *res = NULL;
    pmix_status_t rc;
    size_t bufsz;
    char *buf;
    int err;

    if (NULL == str || '\0' == str[0]) {
        return PMIX_ERR_BAD_PARAM;
    }
    if (numeric(str, uid, &rc)) {
        return rc;
    }
    bufsz = entry_bufsize(_SC_GETPW_R_SIZE_MAX);
    buf = (char *) malloc(bufsz);
    if (NULL == buf) {
        return PMIX_ERR_NOMEM;
    }
    err = getpwnam_r(str, &pw, buf, bufsz, &res);
    if (0 == err && NULL != res) {
        *uid = (uint32_t) pw.pw_uid;
        rc = PMIX_SUCCESS;
    } else {
        rc = PMIX_ERR_NOT_FOUND;
    }
    free(buf);
    return rc;
}

pmix_status_t pmix_util_gid_from_string(const char *str, uint32_t *gid)
{
    struct group gr, *res = NULL;
    pmix_status_t rc;
    size_t bufsz;
    char *buf;
    int err;

    if (NULL == str || '\0' == str[0]) {
        return PMIX_ERR_BAD_PARAM;
    }
    if (numeric(str, gid, &rc)) {
        return rc;
    }
    bufsz = entry_bufsize(_SC_GETGR_R_SIZE_MAX);
    buf = (char *) malloc(bufsz);
    if (NULL == buf) {
        return PMIX_ERR_NOMEM;
    }
    err = getgrnam_r(str, &gr, buf, bufsz, &res);
    if (0 == err && NULL != res) {
        *gid = (uint32_t) gr.gr_gid;
        rc = PMIX_SUCCESS;
    } else {
        rc = PMIX_ERR_NOT_FOUND;
    }
    free(buf);
    return rc;
}

pmix_status_t pmix_util_uid_from_value(const pmix_value_t *val, uint32_t *uid)
{
    if (NULL == val) {
        return PMIX_ERR_BAD_PARAM;
    }
    if (PMIX_STRING == val->type) {
        return pmix_util_uid_from_string(val->data.string, uid);
    }
    if (PMIX_SUCCESS != PMIx_Value_get_number(val, uid, PMIX_UINT32)) {
        return PMIX_ERR_BAD_PARAM;
    }
    return PMIX_SUCCESS;
}

pmix_status_t pmix_util_gid_from_value(const pmix_value_t *val, uint32_t *gid)
{
    if (NULL == val) {
        return PMIX_ERR_BAD_PARAM;
    }
    if (PMIX_STRING == val->type) {
        return pmix_util_gid_from_string(val->data.string, gid);
    }
    if (PMIX_SUCCESS != PMIx_Value_get_number(val, gid, PMIX_UINT32)) {
        return PMIX_ERR_BAD_PARAM;
    }
    return PMIX_SUCCESS;
}
