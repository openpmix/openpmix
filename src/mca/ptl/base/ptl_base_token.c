/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/* The token PMIx_server_setup_fork gives each rank it prepares a process
 * for: PMIX_PTL_TOKEN_LEN random bytes, kept here per namespace and rank
 * and put in the process's environment, in hex, as PMIX_PTL_TOKEN_ENV. A
 * connection from a process of this release that names the rank must
 * present it.
 *
 * PMIx_server_register_client gives a rank one too, which nobody is
 * handed, so that from its registration on the rank is taken only with
 * the token setup_fork hands out. It makes one only for a rank that has
 * none, so the two calls may come in either order. A rank neither
 * registered nor prepared has none, and nothing is asked of it.
 *
 * The environment names the server that made it: the value is
 * "<server nspace>.<server rank>;<hex>". A process sends it to the server
 * of that name, however it reaches that server, and to no other.
 *
 * A rank's token is bound to the pid the kernel reports for the first
 * client connection accepted with it over the local socket, and from then
 * on a client presenting it is taken only from that pid - so the process
 * connects again after a finalize, and the program it execs connects, but
 * no other client process. A tool presenting the token is checked for the
 * token alone, and its connection binds nothing. The binding outlasts the
 * connection. It goes when the host replaces the
 * rank: preparing it again makes a new token, which is not bound;
 * deregistering the rank or its namespace forgets the token; registering
 * it again keeps the token and unbinds it. A connection whose kernel
 * reports no pid - TCP, or a platform with no way to ask - neither binds
 * a token nor is compared with one that is bound.
 *
 * Everything here runs on the progress thread: setup_fork, client
 * registration and deregistration, and the connection handler all do. */

#include "src/include/pmix_config.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "src/class/pmix_hash_table.h"
#include "src/class/pmix_list.h"
#include "src/common/pmix_idenv.h"
#include "src/include/pmix_globals.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_output.h"
#include "src/util/pmix_printf.h"
#include "src/util/pmix_random_bytes.h"

#include "src/mca/ptl/base/base.h"

/* a rank's token */
typedef struct {
    unsigned char bytes[PMIX_PTL_TOKEN_LEN];
    /* the pid it is bound to, 0 until a connection binds it */
    pid_t pid;
} token_t;

/* one namespace's tokens, by rank */
typedef struct {
    pmix_list_item_t super;
    char *nspace;
    pmix_hash_table_t ranks; // rank -> token_t *
} token_ns_t;

/* zero len bytes - through a volatile pointer, so the stores are kept even
 * though the memory is freed next */
void pmix_ptl_base_wipe(void *p, size_t len)
{
    volatile unsigned char *v = (volatile unsigned char *) p;

    while (0 < len--) {
        *v++ = 0;
    }
}

static void tncon(token_ns_t *p)
{
    p->nspace = NULL;
    PMIX_CONSTRUCT(&p->ranks, pmix_hash_table_t);
    pmix_hash_table_init(&p->ranks, 32);
}
static void tndes(token_ns_t *p)
{
    uint32_t rank;
    void *tok, *node = NULL;
    int rc;

    rc = pmix_hash_table_get_first_key_uint32(&p->ranks, &rank, &tok, &node);
    while (PMIX_SUCCESS == rc) {
        pmix_ptl_base_wipe(tok, sizeof(token_t));
        free(tok);
        rc = pmix_hash_table_get_next_key_uint32(&p->ranks, &rank, &tok, node, &node);
    }
    PMIX_DESTRUCT(&p->ranks);
    free(p->nspace);
}
static PMIX_CLASS_INSTANCE(token_ns_t, pmix_list_item_t, tncon, tndes);

void pmix_ptl_base_wipe_token(char *s)
{
    if (NULL != s) {
        pmix_ptl_base_wipe(s, strlen(s));
    }
}

void pmix_ptl_base_wipe_token_info(pmix_info_t *info, size_t ninfo)
{
    size_t n;

    for (n = 0; NULL != info && n < ninfo; n++) {
        if (PMIx_Check_key(info[n].key, PMIX_PTL_TOKEN_KEY) &&
            PMIX_STRING == info[n].value.type) {
            pmix_ptl_base_wipe_token(info[n].value.data.string);
        }
    }
}

static token_ns_t *find_ns(const char *nspace)
{
    token_ns_t *tn;

    PMIX_LIST_FOREACH (tn, &pmix_ptl_base.tokens, token_ns_t) {
        if (0 == strcmp(tn->nspace, nspace)) {
            return tn;
        }
    }
    return NULL;
}

/* Keep tok as the rank's token, in place of any it had - and own it,
 * whatever happens */
static pmix_status_t keep_token(const pmix_proc_t *proc, token_t *tok)
{
    token_ns_t *tn;
    void *old;

    tn = find_ns(proc->nspace);
    if (NULL == tn) {
        tn = PMIX_NEW(token_ns_t);
        if (NULL == tn || NULL == (tn->nspace = strdup(proc->nspace))) {
            if (NULL != tn) {
                PMIX_RELEASE(tn);
            }
            pmix_ptl_base_wipe(tok, sizeof(token_t));
            free(tok);
            return PMIX_ERR_NOMEM;
        }
        pmix_list_append(&pmix_ptl_base.tokens, &tn->super);
    }
    if (PMIX_SUCCESS == pmix_hash_table_get_value_uint32(&tn->ranks, proc->rank, &old)) {
        pmix_ptl_base_wipe(old, sizeof(token_t));
        free(old);
    }
    if (PMIX_SUCCESS != pmix_hash_table_set_value_uint32(&tn->ranks, proc->rank, tok)) {
        (void) pmix_hash_table_remove_value_uint32(&tn->ranks, proc->rank);
        pmix_ptl_base_wipe(tok, sizeof(token_t));
        free(tok);
        return PMIX_ERR_NOMEM;
    }
    return PMIX_SUCCESS;
}

/* A new token, bound to no pid, in *tok */
static pmix_status_t new_token(token_t **tok)
{
    token_t *t;

    t = (token_t *) calloc(1, sizeof(token_t));
    if (NULL == t) {
        return PMIX_ERR_NOMEM;
    }
    if (PMIX_SUCCESS != pmix_util_random_bytes(t->bytes, PMIX_PTL_TOKEN_LEN)) {
        pmix_output(0, "ptl:base: no secure random source to make a connection token from");
        pmix_ptl_base_wipe(t, sizeof(token_t));
        free(t);
        return PMIX_ERROR;
    }
    t->pid = 0;
    *tok = t;
    return PMIX_SUCCESS;
}

/* The rank's token, or NULL if it has none */
static token_t *find_token(const char *nspace, pmix_rank_t rank)
{
    token_ns_t *tn;
    void *val;

    tn = find_ns(nspace);
    if (NULL == tn || PMIX_SUCCESS != pmix_hash_table_get_value_uint32(&tn->ranks, rank, &val)) {
        return NULL;
    }
    return (token_t *) val;
}

pmix_status_t pmix_ptl_base_make_token(const pmix_proc_t *proc, char ***env)
{
    token_t *tok;
    char hex[2 * PMIX_PTL_TOKEN_LEN + 1], *value = NULL;
    size_t n;
    pmix_status_t rc;

    /* a process given no rank of its own has no token - and must not keep
     * one it inherited from whoever made its environment */
    if ('\0' == proc->nspace[0] || !PMIX_RANK_IS_VALID(proc->rank)) {
        rc = pmix_unsetenv(PMIX_PTL_TOKEN_ENV, env);
        return (PMIX_ERR_NOT_FOUND == rc) ? PMIX_SUCCESS : rc;
    }
    if (PMIX_SUCCESS != (rc = new_token(&tok))) {
        return rc;
    }
    for (n = 0; n < PMIX_PTL_TOKEN_LEN; n++) {
        snprintf(&hex[2 * n], 3, "%02x", (unsigned) tok->bytes[n]);
    }
    /* with the name of the server it is for: ours */
    if (0 > pmix_asprintf(&value, "%s.%u;%s", pmix_globals.myid.nspace, pmix_globals.myid.rank,
                          hex)) {
        pmix_ptl_base_wipe(hex, sizeof(hex));
        pmix_ptl_base_wipe(tok, sizeof(token_t));
        free(tok);
        return PMIX_ERR_NOMEM;
    }
    pmix_ptl_base_wipe(hex, sizeof(hex));
    rc = PMIx_Setenv(PMIX_PTL_TOKEN_ENV, value, true, env);
    pmix_ptl_base_wipe(value, strlen(value));
    free(value);
    if (PMIX_SUCCESS != rc) {
        pmix_ptl_base_wipe(tok, sizeof(token_t));
        free(tok);
        return rc;
    }

    /* a rank prepared again - a process the host starts afresh - gets a
     * new token, bound to no pid, and the old one is no longer good */
    return keep_token(proc, tok);
}

pmix_status_t pmix_ptl_base_reserve_token(const pmix_proc_t *proc)
{
    token_t *tok;
    pmix_status_t rc;

    if ('\0' == proc->nspace[0] || !PMIX_RANK_IS_VALID(proc->rank)) {
        return PMIX_SUCCESS;
    }
    if (NULL != find_token(proc->nspace, proc->rank)) {
        /* prepared already: the token it was handed stands */
        return PMIX_SUCCESS;
    }
    if (PMIX_SUCCESS != (rc = new_token(&tok))) {
        return rc;
    }
    return keep_token(proc, tok);
}

static int hexval(char c)
{
    if ('0' <= c && '9' >= c) {
        return c - '0';
    }
    if ('a' <= c && 'f' >= c) {
        return c - 'a' + 10;
    }
    return -1;
}

/* Whether `given` is the hex of tok's bytes */
static bool same_token(const token_t *tok, const char *given)
{
    unsigned char diff = 0;
    size_t n;
    int hi, lo;

    if (NULL == given || 2 * PMIX_PTL_TOKEN_LEN != strlen(given)) {
        return false;
    }
    /* every byte is compared, whatever the first that differs */
    for (n = 0; n < PMIX_PTL_TOKEN_LEN; n++) {
        hi = hexval(given[2 * n]);
        lo = hexval(given[2 * n + 1]);
        if (0 > hi || 0 > lo) {
            diff |= 1;
            continue;
        }
        diff |= (unsigned char) (((hi << 4) | lo) ^ tok->bytes[n]);
    }
    return 0 == diff;
}

pmix_status_t pmix_ptl_base_check_token(const char *nspace, pmix_rank_t rank, const char *given)
{
    return pmix_ptl_base_check_token_from(nspace, rank, given, 0);
}

pmix_status_t pmix_ptl_base_check_token_from(const char *nspace, pmix_rank_t rank,
                                             const char *given, pid_t pid)
{
    token_t *tok;

    tok = find_token(nspace, rank);
    if (NULL == tok) {
        /* the rank was never given one */
        return PMIX_SUCCESS;
    }
    if (!same_token(tok, given)) {
        return PMIX_ERR_INVALID_CRED;
    }
    /* A bound token is taken only from the pid it is bound to. A
     * connection whose kernel reports no pid (pid 0) is not compared -
     * it is taken on its token alone, as every connection was before
     * tokens were bound */
    if (0 < tok->pid && 0 < pid && tok->pid != pid) {
        pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                            "ptl:base: the token of %s:%u is bound to pid %ld, not pid %ld",
                            nspace, rank, (long) tok->pid, (long) pid);
        return PMIX_ERR_INVALID_CRED;
    }
    return PMIX_SUCCESS;
}

void pmix_ptl_base_bind_token(const char *nspace, pmix_rank_t rank, const char *given, pid_t pid)
{
    token_t *tok;

    if (0 >= pid) {
        /* the kernel reported no pid for the connection */
        return;
    }
    tok = find_token(nspace, rank);
    /* only the token the connection presented, which the host may have
     * replaced while it decided on the connection - and only once */
    if (NULL == tok || 0 < tok->pid || !same_token(tok, given)) {
        return;
    }
    tok->pid = pid;
    pmix_output_verbose(2, pmix_ptl_base_framework.framework_output,
                        "ptl:base: the token of %s:%u is bound to pid %ld", nspace, rank,
                        (long) pid);
}

void pmix_ptl_base_unbind_token(const char *nspace, pmix_rank_t rank)
{
    token_t *tok;

    tok = find_token(nspace, rank);
    if (NULL != tok) {
        tok->pid = 0;
    }
}

pid_t pmix_ptl_base_token_pid(const char *nspace, pmix_rank_t rank)
{
    token_t *tok;

    tok = find_token(nspace, rank);
    return (NULL == tok) ? 0 : tok->pid;
}

void pmix_ptl_base_drop_tokens(const char *nspace, pmix_rank_t rank)
{
    token_ns_t *tn;
    void *val;

    tn = find_ns(nspace);
    if (NULL == tn) {
        return;
    }
    if (PMIX_RANK_WILDCARD == rank) {
        pmix_list_remove_item(&pmix_ptl_base.tokens, &tn->super);
        PMIX_RELEASE(tn);
        return;
    }
    if (PMIX_SUCCESS == pmix_hash_table_get_value_uint32(&tn->ranks, rank, &val)) {
        pmix_ptl_base_wipe(val, sizeof(token_t));
        free(val);
        (void) pmix_hash_table_remove_value_uint32(&tn->ranks, rank);
    }
}

pmix_status_t pmix_ptl_base_add_token(pmix_info_t **info, size_t *ninfo, const char *nspace,
                                      pmix_rank_t rank)
{
    const char *value, *token, *dot;
    char *end;
    unsigned long r;
    pmix_info_t *iptr;
    size_t n, m;
    pmix_status_t rc;

    /* "<server nspace>.<server rank>;<hex>": the token goes to that server
     * alone */
    value = pmix_idenv_get(PMIX_PTL_TOKEN_ENV);
    if (NULL == value || NULL == nspace || NULL == (token = strrchr(value, ';'))) {
        return PMIX_SUCCESS;
    }
    for (dot = token - 1; dot >= value && '.' != *dot; dot--) {
    }
    if (dot < value || (size_t) (dot - value) != strlen(nspace) ||
        0 != strncmp(value, nspace, (size_t) (dot - value))) {
        return PMIX_SUCCESS;
    }
    errno = 0;
    r = strtoul(dot + 1, &end, 10);
    if (0 != errno || end != token || end == dot + 1 || (unsigned long) rank != r) {
        return PMIX_SUCCESS;
    }
    ++token;
    PMIX_INFO_CREATE(iptr, *ninfo + 1);
    if (NULL == iptr) {
        return PMIX_ERR_NOMEM;
    }
    /* the token is the one from our environment, whatever else the
     * caller's directives carried under its key */
    for (n = 0, m = 0; n < *ninfo; n++) {
        if (PMIx_Check_key((*info)[n].key, PMIX_PTL_TOKEN_KEY)) {
            continue;
        }
        rc = PMIx_Info_xfer(&iptr[m], &(*info)[n]);
        if (PMIX_SUCCESS != rc) {
            PMIX_INFO_FREE(iptr, *ninfo + 1);
            return rc;
        }
        ++m;
    }
    PMIX_INFO_LOAD(&iptr[m], PMIX_PTL_TOKEN_KEY, token, PMIX_STRING);
    if (NULL != *info) {
        pmix_ptl_base_wipe_token_info(*info, *ninfo);
        PMIX_INFO_FREE(*info, *ninfo);
    }
    *info = iptr;
    *ninfo = m + 1;
    return PMIX_SUCCESS;
}
