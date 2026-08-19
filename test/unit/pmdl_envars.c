/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * Unit tests for the pmdl framework: the way it classifies an MCA
 * parameter, the way it carries a param-file value, and the environment
 * its ompi component builds for a child.
 *
 * What each part covers, and why it is worth a test:
 *
 * - Classification.  Both check_*_param helpers compared only as many
 *   characters as the parameter's first segment holds, so any segment
 *   that was merely a *prefix* of "pmix" or "prte" answered yes and the
 *   value went out under the wrong library's prefix.
 * - A param-file line that names a variable but gives it no value parses
 *   to a NULL value, and PMIX_ENVAR_LOAD writes only the fields it is
 *   handed - so the value the caller later strdup()s and free()s was
 *   whatever malloc had left behind.  The list destructs at the end of
 *   that test, which is where a wild pointer would show itself.
 * - An MPMD job's per-app values (its working directory and command
 *   line) were fetched with the *server's* appnum rather than the
 *   child's, so every rank in the job was told it was running the first
 *   app's command from the first app's directory.
 * - Everything after the per-app breakdown in setup_fork - the restart
 *   count and the MCA-file variables - sat behind an early return taken
 *   by every single-app job, which is to say by nearly every job.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/mca/base/pmix_mca_base_vari.h"
#include "src/mca/pmdl/base/base.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_environ.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP0NS "pmdl-mpmd"
#define APP1NS "pmdl-single"

static int failures = 0;
static int checks = 0;

static void ok(bool cond, const char *what)
{
    ++checks;
    if (!cond) {
        ++failures;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

/* the value of "var" in env, or NULL */
static const char *envval(char **env, const char *var)
{
    size_t len = strlen(var);
    int i;

    for (i = 0; NULL != env && NULL != env[i]; i++) {
        if (0 == strncmp(env[i], var, len) && '=' == env[i][len]) {
            return &env[i][len + 1];
        }
    }
    return NULL;
}

static void ok_env(char **env, const char *var, const char *expected)
{
    const char *ev = envval(env, var);
    char msg[256];

    snprintf(msg, sizeof(msg), "%s is \"%s\" (got \"%s\")", var, expected,
             (NULL == ev) ? "<unset>" : ev);
    ok(NULL != ev && 0 == strcmp(ev, expected), msg);
}

/* one entry as the param-file parser would have left it */
static pmix_mca_base_var_file_value_t *file_value(const char *var, const char *value)
{
    pmix_mca_base_var_file_value_t *fv = PMIX_NEW(pmix_mca_base_var_file_value_t);

    fv->mbvfv_var = strdup(var);
    fv->mbvfv_value = (NULL == value) ? NULL : strdup(value);
    return fv;
}

/* register a job of "napps" apps, each with its own wdir and command,
 * carrying the ompi personality so the pmdl component tracks it */
static pmix_status_t register_job(const char *nspace, uint32_t napps)
{
    void *jinfo, *ainfo, *pinfo;
    pmix_data_array_t darray;
    pmix_info_t *info;
    pmix_status_t rc;
    size_t ninfo;
    pmix_rank_t rk = 0, ldr;
    uint32_t u32, n, m, nprocs, jobsize = 0;
    uint16_t u16;
    char tmp[256];

    /* app n holds n+2 procs, so the sizes and the leaders differ */
    for (n = 0; n < napps; n++) {
        jobsize += n + 2;
    }

    PMIX_INFO_LIST_START(jinfo);
    PMIX_INFO_LIST_ADD(rc, jinfo, PMIX_PERSONALITY, "ompi", PMIX_STRING);
    u32 = jobsize;
    PMIX_INFO_LIST_ADD(rc, jinfo, PMIX_JOB_SIZE, &u32, PMIX_UINT32);
    PMIX_INFO_LIST_ADD(rc, jinfo, PMIX_LOCAL_SIZE, &u32, PMIX_UINT32);
    u32 = jobsize * 2;
    PMIX_INFO_LIST_ADD(rc, jinfo, PMIX_UNIV_SIZE, &u32, PMIX_UINT32);
    u32 = napps;
    PMIX_INFO_LIST_ADD(rc, jinfo, PMIX_JOB_NUM_APPS, &u32, PMIX_UINT32);

    for (n = 0; n < napps; n++) {
        nprocs = n + 2;
        ldr = rk;
        PMIX_INFO_LIST_START(ainfo);
        u32 = n;
        PMIX_INFO_LIST_ADD(rc, ainfo, PMIX_APPNUM, &u32, PMIX_UINT32);
        u32 = nprocs;
        PMIX_INFO_LIST_ADD(rc, ainfo, PMIX_APP_SIZE, &u32, PMIX_UINT32);
        PMIX_INFO_LIST_ADD(rc, ainfo, PMIX_APPLDR, &ldr, PMIX_PROC_RANK);
        snprintf(tmp, sizeof(tmp), "/tmp/app%u", n);
        PMIX_INFO_LIST_ADD(rc, ainfo, PMIX_WDIR, tmp, PMIX_STRING);
        snprintf(tmp, sizeof(tmp), "app%u -x %u", n, n);
        PMIX_INFO_LIST_ADD(rc, ainfo, PMIX_APP_ARGV, tmp, PMIX_STRING);
        PMIX_INFO_LIST_CONVERT(rc, ainfo, &darray);
        PMIX_INFO_LIST_RELEASE(ainfo);
        PMIX_INFO_LIST_ADD(rc, jinfo, PMIX_APP_INFO_ARRAY, &darray, PMIX_DATA_ARRAY);
        PMIX_DATA_ARRAY_DESTRUCT(&darray);

        for (m = 0; m < nprocs; m++) {
            PMIX_INFO_LIST_START(pinfo);
            PMIX_INFO_LIST_ADD(rc, pinfo, PMIX_RANK, &rk, PMIX_PROC_RANK);
            u32 = n;
            PMIX_INFO_LIST_ADD(rc, pinfo, PMIX_APPNUM, &u32, PMIX_UINT32);
            u16 = (uint16_t) rk;
            PMIX_INFO_LIST_ADD(rc, pinfo, PMIX_LOCAL_RANK, &u16, PMIX_UINT16);
            PMIX_INFO_LIST_ADD(rc, pinfo, PMIX_NODE_RANK, &u16, PMIX_UINT16);
            snprintf(tmp, sizeof(tmp), "/tmp/%s/%u", nspace, rk);
            PMIX_INFO_LIST_ADD(rc, pinfo, PMIX_PROCDIR, tmp, PMIX_STRING);
            PMIX_INFO_LIST_CONVERT(rc, pinfo, &darray);
            PMIX_INFO_LIST_RELEASE(pinfo);
            PMIX_INFO_LIST_ADD(rc, jinfo, PMIX_PROC_INFO_ARRAY, &darray, PMIX_DATA_ARRAY);
            PMIX_DATA_ARRAY_DESTRUCT(&darray);
            ++rk;
        }
    }

    PMIX_INFO_LIST_CONVERT(rc, jinfo, &darray);
    PMIX_INFO_LIST_RELEASE(jinfo);
    info = (pmix_info_t *) darray.array;
    ninfo = darray.size;
    rc = PMIx_server_register_nspace((char *) nspace, jobsize, info, ninfo, NULL, NULL);
    PMIX_DATA_ARRAY_DESTRUCT(&darray);
    /* with no callback the registration completes inline, and says so */
    return (PMIX_OPERATION_SUCCEEDED == rc) ? PMIX_SUCCESS : rc;
}

int main(int argc, char **argv)
{
    pmix_mca_base_var_file_value_t *fv, *fvnext;
    pmix_list_t ilist, params;
    pmix_kval_t *kv;
    pmix_proc_t proc;
    pmix_status_t rc;
    const char *ev;
    char **env;
    bool found;

    (void) argc;
    (void) argv;

    /* --- the two classification helpers --- */

    /* these do not need a live server, and neither do they change */
    ok(pmix_pmdl_base_check_pmix_param("pmix_hwloc_topo_file"),
       "a pmix_ parameter belongs to PMIx");
    ok(pmix_pmdl_base_check_pmix_param("ptl_base_verbose"),
       "so does one naming a PMIx framework");
    ok(pmix_pmdl_base_check_pmix_param("gds"),
       "and a bare framework name with no component after it");
    ok(!pmix_pmdl_base_check_pmix_param("pm_foo"),
       "but not a segment that is merely a prefix of \"pmix\"");
    ok(!pmix_pmdl_base_check_pmix_param("pmi_foo"),
       "nor a longer prefix of it");
    ok(!pmix_pmdl_base_check_pmix_param("btl_tcp_if_include"),
       "nor an Open MPI parameter");

    ok(pmix_pmdl_base_check_prte_param("prte_default_hostfile"),
       "a prte_ parameter belongs to PRRTE");
    ok(pmix_pmdl_base_check_prte_param("rmaps_base_mapping_policy"),
       "so does one naming a PRRTE framework");
    ok(!pmix_pmdl_base_check_prte_param("pr_foo"),
       "but not a segment that is merely a prefix of \"prte\"");
    ok(!pmix_pmdl_base_check_prte_param("pml_ob1_verbose"),
       "nor an Open MPI parameter");

    rc = PMIx_server_init(NULL, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "SKIP: PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 77;
    }

    /* --- what parse_file_envars claims out of PMIx's param file --- */

    PMIX_CONSTRUCT(&params, pmix_list_t);
    pmix_list_append(&params, &file_value("btl", "self,tcp")->super);
    pmix_list_append(&params, &file_value("coll_hcoll_enable", "0")->super);
    pmix_list_append(&params, &file_value("iface_name", "eth0")->super);
    pmix_list_append(&params, &file_value("pmix_hwloc_topo_file", "/dev/null")->super);
    pmix_pmdl.parse_file_envars(&params);

    found = false;
    PMIX_LIST_FOREACH (fv, &params, pmix_mca_base_var_file_value_t) {
        if (0 == strcmp(fv->mbvfv_var, "iface_name")) {
            found = true;
        }
        ok(0 != strcmp(fv->mbvfv_var, "btl") && 0 != strcmp(fv->mbvfv_var, "coll_hcoll_enable"),
           "an Open MPI parameter is taken off the list");
    }
    ok(found, "a name that merely starts with a framework name (\"if\") is left alone");
    found = false;
    PMIX_LIST_FOREACH (fv, &params, pmix_mca_base_var_file_value_t) {
        if (0 == strcmp(fv->mbvfv_var, "pmix_hwloc_topo_file")) {
            found = true;
        }
    }
    ok(found, "and so is a parameter of PMIx's own");
    PMIX_LIST_DESTRUCT(&params);

    /* --- a param file value that has no value --- */

    /* harvest_envars turns every entry on this list into a
     * PMIX_SET_ENVAR directive, so put one there that has nothing on the
     * right of the "=" */
    fv = file_value("pmdl_test_novalue", NULL);
    pmix_list_append(&pmix_mca_base_var_file_values, &fv->super);

    PMIX_CONSTRUCT(&ilist, pmix_list_t);
    rc = pmix_pmdl_base_harvest_envars(NULL, NULL, 0, &ilist);
    ok(PMIX_SUCCESS == rc, "harvesting a param file with an empty value succeeds");
    found = false;
    PMIX_LIST_FOREACH (kv, &ilist, pmix_kval_t) {
        if (PMIX_ENVAR != kv->value->type) {
            continue;
        }
        if (0 == strcmp(kv->value->data.envar.envar, "PMIX_MCA_pmdl_test_novalue")) {
            found = true;
            ok(NULL != kv->value->data.envar.value &&
               0 == strlen(kv->value->data.envar.value),
               "an absent param file value forwards as an empty one");
        }
    }
    ok(found, "the empty-valued parameter reaches the child's environment");
    /* and this is where a value nobody wrote would be freed */
    PMIX_LIST_DESTRUCT(&ilist);

    PMIX_LIST_FOREACH_SAFE (fv, fvnext, &pmix_mca_base_var_file_values,
                            pmix_mca_base_var_file_value_t) {
        if (0 == strcmp(fv->mbvfv_var, "pmdl_test_novalue")) {
            pmix_list_remove_item(&pmix_mca_base_var_file_values, &fv->super);
            PMIX_RELEASE(fv);
            break;
        }
    }

    /* --- an MPMD job's per-app values --- */

    rc = register_job(APP0NS, 2);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "SKIP: could not register %s: %s\n", APP0NS, PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 77;
    }

    /* rank 2 is the first rank of the second app */
    PMIX_LOAD_PROCID(&proc, APP0NS, 2);
    env = NULL;
    rc = pmix_pmdl.setup_fork(&proc, &env);
    ok(PMIX_SUCCESS == rc, "setup_fork succeeds for a proc in the second app");
    ok_env(env, "OMPI_COMMAND", "app1");
    ok_env(env, "OMPI_ARGV", "-x 1");
    ok_env(env, "OMPI_MCA_initial_wdir", "/tmp/app1");
    ok_env(env, "OMPI_FILE_LOCATION", "/tmp/" APP0NS "/2");
    ok_env(env, "OMPI_COMM_WORLD_RANK", "2");
    ok_env(env, "OMPI_COMM_WORLD_LOCAL_RANK", "2");
    ok_env(env, "OMPI_COMM_WORLD_SIZE", "5");
    ok_env(env, "OMPI_UNIVERSE_SIZE", "10");
    ok_env(env, "OMPI_NUM_APP_CTX", "2");
    ok_env(env, "OMPI_APP_CTX_NUM_PROCS", "2 3");
    ok_env(env, "OMPI_FIRST_RANKS", "0 2");
    PMIx_Argv_free(env);

    /* and the first app still gets its own */
    PMIX_LOAD_PROCID(&proc, APP0NS, 0);
    env = NULL;
    rc = pmix_pmdl.setup_fork(&proc, &env);
    ok(PMIX_SUCCESS == rc, "setup_fork succeeds for a proc in the first app");
    ok_env(env, "OMPI_COMMAND", "app0");
    ok_env(env, "OMPI_MCA_initial_wdir", "/tmp/app0");
    PMIx_Argv_free(env);

    /* --- a single-app job gets the MCA-file variables too --- */

    rc = register_job(APP1NS, 1);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "SKIP: could not register %s: %s\n", APP1NS, PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 77;
    }
    PMIX_LOAD_PROCID(&proc, APP1NS, 0);
    env = NULL;
    rc = pmix_pmdl.setup_fork(&proc, &env);
    /* nothing here ever set PMIX_REINCARNATION, which is the host's to
     * provide - its absence must not fail the fork */
    ok(PMIX_SUCCESS == rc, "setup_fork succeeds without a reincarnation number");
    ok(NULL == envval(env, "OMPI_MCA_num_restarts"),
       "and says nothing about restarts when the host said nothing");
    ok_env(env, "OMPI_NUM_APP_CTX", "1");
    ok(NULL == envval(env, "OMPI_APP_CTX_NUM_PROCS"),
       "a single-app job gets no per-app breakdown");
    /* these came from parse_file_envars above, and used to be skipped
     * for every job with one app */
    ev = envval(env, "OMPI_MCA_btl");
    ok(NULL != ev && 0 == strcmp(ev, "self,tcp"),
       "a value from an MCA param file reaches a single-app child");
    ev = envval(env, "OMPI_MCA_coll_hcoll_enable");
    ok(NULL != ev && 0 == strcmp(ev, "0"), "and so does the next one");
    PMIx_Argv_free(env);

    pmix_pmdl.deregister_nspace(APP0NS);
    pmix_pmdl.deregister_nspace(APP1NS);

    PMIx_server_finalize();

    fprintf(stderr, "%s: %d checks, %d failures\n", (0 == failures) ? "PASS" : "FAIL", checks,
            failures);
    return (0 == failures) ? 0 : 1;
}
