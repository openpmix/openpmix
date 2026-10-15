/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "src/include/pmix_config.h"

#include "src/common/pmix_idenv.h"
#include "src/mca/ptl/base/base.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_printf.h"

#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* the variables, less the URIs - which are named by prefix */
static const char *const idvars[] = {
    "PMIX_NAMESPACE",
    "PMIX_RANK",
    "PMIX_SERVER_NSPACE",
    "PMIX_SERVER_RANK",
    PMIX_PTL_LOCAL_URI_ENV,
    PMIX_PTL_TOKEN_ENV,
    "PMIX_LAUNCHER_RNDZ_URI",
    "PMIX_SECURITY_MODE",
    "PMIX_BFROP_BUFFER_TYPE",
    "PMIX_GDS_MODULE",
    "PMIX_VERSION",
    NULL
};
#define PMIX_IDENV_URI_PREFIX "PMIX_SERVER_URI"

/* what was taken, as "name=value", and the process it was taken for: its
 * pid, and on Linux the pid namespace it is in - a process forked into a
 * namespace of its own can have the pid its parent has in its own */
static char **kept = NULL;
static pid_t kept_pid = 0;
static unsigned long kept_pidns = 0;

static unsigned long pid_namespace(void)
{
#if defined(__linux__)
    struct stat st;

    if (0 == stat("/proc/self/ns/pid", &st)) {
        return (unsigned long) st.st_ino;
    }
#endif
    return 0;
}

/* whether what is kept was kept for this process */
static bool kept_here(void)
{
    return NULL != kept && kept_pid == getpid() && kept_pidns == pid_namespace();
}

/* whether the first `len` characters of s name one of the variables */
static bool is_idvar(const char *s, size_t len)
{
    size_t n, plen = strlen(PMIX_IDENV_URI_PREFIX);

    if (len >= plen && 0 == strncmp(s, PMIX_IDENV_URI_PREFIX, plen)) {
        return true;
    }
    for (n = 0; NULL != idvars[n]; n++) {
        if (len == strlen(idvars[n]) && 0 == strncmp(s, idvars[n], len)) {
            return true;
        }
    }
    return false;
}

/* Overwrite s where it lies - unless this process may not write there, as
 * it may not a string a program handed putenv(). The kernel copies the
 * zeros in from a pipe, and refuses an address it cannot write with
 * EFAULT, where a store would raise a signal */
static void wipe_in_place(char *s)
{
    char zeros[256];
    size_t len, n;
    int p[2];

    if (NULL == s || 0 == (len = strlen(s)) || 0 != pipe(p)) {
        return;
    }
    memset(zeros, 0, sizeof(zeros));
    while (0 < len) {
        n = (len < sizeof(zeros)) ? len : sizeof(zeros);
        if ((ssize_t) n != write(p[1], zeros, n) || (ssize_t) n != read(p[0], s, n)) {
            break;
        }
        s += n;
        len -= n;
    }
    close(p[0]);
    close(p[1]);
}

/* release an array of "name=value", the token's value wiped first */
static void release(char **arr)
{
    pmix_ptl_base_wipe_token(pmix_getenv(PMIX_PTL_TOKEN_ENV, arr));
    PMIx_Argv_free(arr);
}

static void forget(void)
{
    release(kept);
    kept = NULL;
}

void pmix_idenv_take(void)
{
    char **names = NULL, **fresh = NULL, *eq, *value, *name, *entry;
    size_t n, len;
    bool seen;

    if (NULL != kept && !kept_here()) {
        /* kept for the process this one was forked from */
        forget();
    }
    kept_pid = getpid();
    kept_pidns = pid_namespace();

    /* the names first, once each: taking one out changes the environment */
    for (n = 0; NULL != environ && NULL != environ[n]; n++) {
        eq = strchr(environ[n], '=');
        if (NULL == eq || !is_idvar(environ[n], (size_t) (eq - environ[n]))) {
            continue;
        }
        len = (size_t) (eq - environ[n]);
        seen = false;
        for (size_t m = 0; NULL != names && NULL != names[m] && !seen; m++) {
            seen = (strlen(names[m]) == len && 0 == strncmp(names[m], environ[n], len));
        }
        if (seen) {
            continue;
        }
        name = strndup(environ[n], len);
        if (NULL == name || PMIX_SUCCESS != PMIx_Argv_append_nosize(&names, name)) {
            free(name);
            PMIx_Argv_free(names);
            return;
        }
        free(name);
    }
    if (NULL == names) {
        /* none of them: what was kept stands */
        return;
    }

    /* A namespace names a process anew: what it comes with replaces
     * everything kept, so no value of the identity it replaces is left
     * beside it. Without one - a variable put right before an init is
     * tried again, say - each replaces only the kept value of its name */
    if (NULL == getenv("PMIX_NAMESPACE")) {
        for (n = 0; NULL != kept && NULL != kept[n]; n++) {
            seen = false;
            for (size_t m = 0; NULL != names[m] && !seen; m++) {
                len = strlen(names[m]);
                seen = (0 == strncmp(kept[n], names[m], len) && '=' == kept[n][len]);
            }
            if (!seen && PMIX_SUCCESS != PMIx_Argv_append_nosize(&fresh, kept[n])) {
                release(fresh);
                PMIx_Argv_free(names);
                return;
            }
        }
    }

    /* Keep them all - each as getenv answers it, the first entry of its
     * name - or take none: they are one process's identity */
    for (n = 0; NULL != names[n]; n++) {
        value = getenv(names[n]);
        if (NULL == value) {
            continue;
        }
        if (0 > pmix_asprintf(&entry, "%s=%s", names[n], value)) {
            release(fresh);
            PMIx_Argv_free(names);
            return;
        }
        if (PMIX_SUCCESS != PMIx_Argv_append_nosize(&fresh, entry)) {
            pmix_ptl_base_wipe_token(entry);
            free(entry);
            release(fresh);
            PMIx_Argv_free(names);
            return;
        }
        pmix_ptl_base_wipe_token(entry);
        free(entry);
    }
    forget();
    kept = fresh;

    /* and out of the environment - unsetenv takes every entry of a name.
     * Each entry of the token is overwritten first: the environment's
     * copy is the one others may be shown */
    len = strlen(PMIX_PTL_TOKEN_ENV);
    for (n = 0; NULL != environ && NULL != environ[n]; n++) {
        if (0 == strncmp(environ[n], PMIX_PTL_TOKEN_ENV, len) && '=' == environ[n][len]) {
            wipe_in_place(environ[n] + len + 1);
        }
    }
    for (n = 0; NULL != names[n]; n++) {
        (void) unsetenv(names[n]);
    }
    PMIx_Argv_free(names);
}

bool pmix_idenv_is_identity(const char *entry)
{
    const char *eq;

    if (NULL == entry || NULL == (eq = strchr(entry, '='))) {
        return false;
    }
    return is_idvar(entry, (size_t) (eq - entry));
}

const char *pmix_idenv_get(const char *name)
{
    const char *value;

    if (kept_here() && NULL != (value = pmix_getenv(name, kept))) {
        return value;
    }
    return getenv(name);
}
