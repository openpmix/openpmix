/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * White-box unit tests for access to a namespace by user and group -
 * src/server/pmix_server_access.c, and the places a job's owner and
 * access list are recorded. See docs/security-plan.rst.
 *
 * A job has an owner and may name further users and groups allowed to
 * access it. A requester is allowed if it is root, runs as the server's
 * own user, is the owner, is a listed user, or belongs to a listed group.
 *
 * Test cases:
 *
 *   registration naming an owner and an access list -> recorded
 *   the same inside a PMIX_JOB_INFO_ARRAY              -> recorded
 *   an access list given by name                       -> resolved to
 *                                                         numbers; the
 *                                                         host's array is
 *                                                         left as it was
 *   an access list naming nobody                       -> registration
 *                                                         refused
 *   an owner that is not a number                      -> ignored, as it
 *                                                         always has been
 *   a malformed PMIX_ACCESS_PERMISSIONS                -> registration
 *                                                         refused, and no
 *                                                         namespace left
 *   no owner registered, a client registered           -> the client's
 *                                                         user owns it; a
 *                                                         registered owner
 *                                                         replaces that
 *   no owner and no client                             -> only root and
 *                                                         the server's user
 *   an update naming a new list                        -> replaces it; one
 *                                                         not naming a list
 *                                                         keeps it
 *   the rule                                           -> each branch
 *   a user's groups                                    -> looked up when
 *                                                         first needed, and
 *                                                         again when a check
 *                                                         would otherwise
 *                                                         refuse
 *   a user the host registers (register_resources
 *   with PMIX_USERID)                                  -> known, with its
 *                                                         groups; dropped on
 *                                                         deregistration
 *   a job registered with an owner                     -> the owner known,
 *                                                         with its groups
 *   a user registered with its groups (PMIX_GRPID
 *   as a data array)                                   -> the host's groups
 *                                                         kept, nothing
 *                                                         looked up
 *   a job's rule as a host keeps it
 *   (pmix_server_access_load)                          -> names resolved; a
 *                                                         malformed list
 *                                                         changes nothing;
 *                                                         groups count only
 *                                                         if the job names
 *                                                         them
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"

#include "src/include/pmix_globals.h"
#include "src/server/pmix_server_ops.h"

#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ids no real account should have */
#define ACC_OWNER   4242
#define ACC_OGROUP  4343
#define ACC_USER    5151
#define ACC_GROUP   6161
#define ACC_STRANGER 9999
#define ACC_CLIENT  7000

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        npass++;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        nfail++;
    }
}

/* A blocking round trip through the progress thread: anything queued
 * ahead of it has run when it returns */
static void progress_barrier(void)
{
    pmix_value_t v;

    PMIX_VALUE_LOAD(&v, "barrier", PMIX_STRING);
    PMIx_Store_internal(&pmix_globals.myid, "access-ut.barrier", &v);
    PMIX_VALUE_DESTRUCT(&v);
}

static pmix_namespace_t *find_ns(const char *name)
{
    pmix_namespace_t *ns;

    PMIX_LIST_FOREACH (ns, &pmix_globals.nspaces, pmix_namespace_t) {
        if (0 == strcmp(ns->nspace, name)) {
            return ns;
        }
    }
    return NULL;
}

static bool registered(pmix_status_t rc)
{
    return PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc;
}

static pmix_status_t reg(const char *name, int nlocal, pmix_info_t *info, size_t ninfo)
{
    pmix_nspace_t ns;
    pmix_status_t rc;

    PMIX_LOAD_NSPACE(ns, name);
    rc = PMIx_server_register_nspace(ns, nlocal, info, ninfo, NULL, NULL);
    progress_barrier();
    return rc;
}

/* an info whose value is a data array of uint32 ids */
static void load_ids(pmix_info_t *info, const char *key, const uint32_t *ids, size_t n)
{
    pmix_data_array_t *da;

    PMIX_DATA_ARRAY_CREATE(da, n, PMIX_UINT32);
    memcpy(da->array, ids, n * sizeof(uint32_t));
    PMIX_INFO_LOAD(info, key, da, PMIX_DATA_ARRAY);
    PMIX_DATA_ARRAY_FREE(da);
}

/* PMIX_ACCESS_PERMISSIONS holding the given entries */
static void load_perms(pmix_info_t *info, pmix_info_t *entries, size_t n)
{
    pmix_data_array_t da;

    da.type = PMIX_INFO;
    da.size = n;
    da.array = entries;
    PMIX_INFO_LOAD(info, PMIX_ACCESS_PERMISSIONS, &da, PMIX_DATA_ARRAY);
}

static bool has_id(const uint32_t *ids, size_t n, uint32_t id)
{
    size_t m;

    for (m = 0; m < n; m++) {
        if (ids[m] == id) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */

static void test_registered_policy(void)
{
    pmix_info_t info[3], perms[2];
    uint32_t uid = ACC_OWNER, gid = ACC_OGROUP, user = ACC_USER, group = ACC_GROUP;
    pmix_namespace_t *ns;
    pmix_status_t rc;

    PMIX_INFO_LOAD(&info[0], PMIX_USERID, &uid, PMIX_UINT32);
    PMIX_INFO_LOAD(&info[1], PMIX_GRPID, &gid, PMIX_UINT32);
    load_ids(&perms[0], PMIX_ACCESS_USERIDS, &user, 1);
    load_ids(&perms[1], PMIX_ACCESS_GRPIDS, &group, 1);
    load_perms(&info[2], perms, 2);
    rc = reg("acc-a", 1, info, 3);
    report("a job naming an owner and an access list registers", registered(rc));
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    PMIX_INFO_DESTRUCT(&info[2]);
    PMIX_INFO_DESTRUCT(&perms[0]);
    PMIX_INFO_DESTRUCT(&perms[1]);

    ns = find_ns("acc-a");
    report("its owner is recorded",
           NULL != ns && PMIX_OWNER_REGISTERED == ns->access.source &&
           ACC_OWNER == ns->access.uid && ACC_OGROUP == ns->access.gid);
    report("its access list is recorded",
           NULL != ns && 1 == ns->access.nuids && ACC_USER == ns->access.uids[0] &&
           1 == ns->access.ngids && ACC_GROUP == ns->access.gids[0]);
    if (NULL == ns) {
        return;
    }

    /* the rule */
    report("the owner is allowed", pmix_server_access_permitted(ACC_OWNER, ns));
    report("a listed user is allowed", pmix_server_access_permitted(ACC_USER, ns));
    report("anyone else is refused", !pmix_server_access_permitted(ACC_STRANGER, ns));
    report("root is allowed", pmix_server_access_permitted(0, ns));
    report("the server's own user is allowed",
           pmix_server_access_permitted(geteuid(), ns));

    /* an update naming a new user list replaces it, and leaves the
     * group list alone */
    user = ACC_USER + 1;
    load_ids(&perms[0], PMIX_ACCESS_USERIDS, &user, 1);
    load_perms(&info[0], perms, 1);
    rc = reg("acc-a", -1, info, 1);
    report("an update naming a new user list is accepted", registered(rc));
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&perms[0]);
    report("the new list replaces the old",
           1 == ns->access.nuids && ACC_USER + 1 == ns->access.uids[0] &&
           !pmix_server_access_permitted(ACC_USER, ns));
    report("the list the update did not name is kept",
           1 == ns->access.ngids && ACC_GROUP == ns->access.gids[0]);
    report("and so is the owner",
           PMIX_OWNER_REGISTERED == ns->access.source && ACC_OWNER == ns->access.uid);
}

static void test_job_info_array(void)
{
    pmix_info_t job[2], perms[1], info;
    pmix_data_array_t da;
    uint32_t uid = ACC_OWNER + 3, group = ACC_GROUP + 3;
    pmix_namespace_t *ns;
    pmix_status_t rc;

    PMIX_INFO_LOAD(&job[0], PMIX_USERID, &uid, PMIX_UINT32);
    load_ids(&perms[0], PMIX_ACCESS_GRPIDS, &group, 1);
    load_perms(&job[1], perms, 1);
    da.type = PMIX_INFO;
    da.size = 2;
    da.array = job;
    PMIX_INFO_LOAD(&info, PMIX_JOB_INFO_ARRAY, &da, PMIX_DATA_ARRAY);
    rc = reg("acc-e", 1, &info, 1);
    report("a job giving them inside a job-info array registers", registered(rc));
    PMIX_INFO_DESTRUCT(&info);
    PMIX_INFO_DESTRUCT(&job[0]);
    PMIX_INFO_DESTRUCT(&job[1]);
    PMIX_INFO_DESTRUCT(&perms[0]);
    ns = find_ns("acc-e");
    report("the owner and list inside a job-info array are recorded",
           NULL != ns && ACC_OWNER + 3 == ns->access.uid && 1 == ns->access.ngids &&
           ACC_GROUP + 3 == ns->access.gids[0]);
}

static void test_names(void)
{
    struct passwd *pw = getpwuid(geteuid());
    struct group *gr = getgrgid(getegid());
    pmix_info_t info[2];
    pmix_data_array_t *da;
    pmix_namespace_t *ns;
    pmix_status_t rc;
    char **names;

    if (NULL == pw || NULL == pw->pw_name || NULL == gr || NULL == gr->gr_name) {
        fprintf(stdout, "  SKIP: this process's user or group has no name\n");
        return;
    }
    /* a data array of user names, and a single group name */
    PMIX_DATA_ARRAY_CREATE(da, 1, PMIX_STRING);
    names = (char **) da->array;
    names[0] = strdup(pw->pw_name);
    PMIX_INFO_LOAD(&info[0], PMIX_ACCESS_USERIDS, da, PMIX_DATA_ARRAY);
    PMIX_DATA_ARRAY_FREE(da);
    PMIX_INFO_LOAD(&info[1], PMIX_ACCESS_GRPIDS, gr->gr_name, PMIX_STRING);
    rc = reg("acc-b", 1, info, 2);
    report("a job whose access list is given by name registers", registered(rc));
    report("the host's array is left as it was",
           PMIX_DATA_ARRAY == info[0].value.type &&
           PMIX_STRING == info[0].value.data.darray->type &&
           PMIX_STRING == info[1].value.type);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    ns = find_ns("acc-b");
    report("the names are recorded as numbers",
           NULL != ns && has_id(ns->access.uids, ns->access.nuids, (uint32_t) geteuid()) &&
           has_id(ns->access.gids, ns->access.ngids, (uint32_t) getegid()));

    PMIX_INFO_LOAD(&info[0], PMIX_ACCESS_GRPIDS, "access-ut-no-such-group", PMIX_STRING);
    rc = reg("acc-b2", 1, info, 1);
    report("an access list naming no group is refused", PMIX_ERR_NOT_FOUND == rc);
    PMIX_INFO_DESTRUCT(&info[0]);
}

static void test_malformed(void)
{
    pmix_info_t info;
    bool flag = true;
    pmix_status_t rc;
    pmix_byte_object_t notuid = {.bytes = (char *) "not-a-uid", .size = 9};
    pmix_namespace_t *ns;

    /* an owner that is not a number has always been ignored */
    PMIX_INFO_LOAD(&info, PMIX_USERID, &notuid, PMIX_BYTE_OBJECT);
    rc = reg("acc-oddowner", 1, &info, 1);
    ns = find_ns("acc-oddowner");
    report("an owner that is not a number is ignored, and the job registers",
           registered(rc) && NULL != ns && PMIX_OWNER_UNKNOWN == ns->access.source);
    PMIX_INFO_DESTRUCT(&info);

    PMIX_INFO_LOAD(&info, PMIX_ACCESS_PERMISSIONS, &flag, PMIX_BOOL);
    rc = reg("acc-bad", 1, &info, 1);
    report("a PMIX_ACCESS_PERMISSIONS that is not an info array is refused",
           PMIX_ERR_BAD_PARAM == rc);
    report("and leaves no namespace behind", NULL == find_ns("acc-bad"));
    PMIX_INFO_DESTRUCT(&info);
}

static void test_owner_fallback(void)
{
    pmix_proc_t proc;
    pmix_info_t info;
    uint32_t uid = ACC_CLIENT + 100;
    pmix_namespace_t *ns;
    pmix_status_t rc;

    /* a client registered before its job: the job belongs to its user */
    PMIX_LOAD_PROCID(&proc, "acc-c", 0);
    rc = PMIx_server_register_client(&proc, ACC_CLIENT, ACC_CLIENT + 1, NULL, NULL, NULL);
    progress_barrier();
    ns = find_ns("acc-c");
    report("with no owner registered, the client's user owns the job",
           NULL != ns && PMIX_OWNER_FROM_CLIENT == ns->access.source &&
           ACC_CLIENT == ns->access.uid && pmix_server_access_permitted(ACC_CLIENT, ns));
    (void) rc;

    PMIX_INFO_LOAD(&info, PMIX_USERID, &uid, PMIX_UINT32);
    rc = reg("acc-c", 2, &info, 1);
    PMIX_INFO_DESTRUCT(&info);
    report("a registered owner replaces the client's",
           registered(rc) && NULL != ns && PMIX_OWNER_REGISTERED == ns->access.source &&
           ACC_CLIENT + 100 == ns->access.uid && !pmix_server_access_permitted(ACC_CLIENT, ns));

    PMIX_LOAD_PROCID(&proc, "acc-c", 1);
    rc = PMIx_server_register_client(&proc, ACC_CLIENT + 5, ACC_CLIENT + 6, NULL, NULL, NULL);
    progress_barrier();
    report("a later client does not replace a registered owner",
           NULL != ns && ACC_CLIENT + 100 == ns->access.uid);

    /* no owner and no client */
    rc = reg("acc-d", 1, NULL, 0);
    ns = find_ns("acc-d");
    report("with no owner known, only the server's user and root are allowed",
           registered(rc) && NULL != ns && PMIX_OWNER_UNKNOWN == ns->access.source &&
           !pmix_server_access_permitted(ACC_CLIENT, ns) &&
           pmix_server_access_permitted(geteuid(), ns) &&
           pmix_server_access_permitted(0, ns));
}

/* a user who is neither root nor us, with an account and so a group -
 * "nobody", where the system has one */
static struct passwd *other_user(void)
{
    struct passwd *pw = getpwnam("nobody");

    if (NULL == pw || 0 == pw->pw_uid || geteuid() == pw->pw_uid) {
        return NULL;
    }
    return pw;
}

static bool has_gid(const pmix_user_t *user, gid_t gid)
{
    uint16_t n;

    for (n = 0; NULL != user->gids && n < user->ngids; n++) {
        if (user->gids[n] == gid) {
            return true;
        }
    }
    return false;
}

static void test_users(void)
{
    pmix_user_t *user, *again;
    pmix_info_t info;
    struct passwd *pw;
    gid_t mine[256];
    uint32_t id;
    int nmine, i;
    bool all = true;
    pmix_status_t rc;

    user = pmix_server_user_create(geteuid());
    report("a new user's groups are not looked up yet", NULL != user && 0 == user->ngids);
    if (NULL == user) {
        return;
    }
    rc = pmix_server_user_refresh(user);
    nmine = getgroups(256, mine);
    for (i = 0; i < nmine; i++) {
        if (!has_gid(user, mine[i])) {
            fprintf(stdout, "    group %u is missing\n", (unsigned) mine[i]);
            all = false;
        }
    }
    report("a refresh finds every group we belong to", PMIX_SUCCESS == rc && 0 < nmine && all);
    PMIX_RELEASE(user);

    user = pmix_server_user_create((uid_t) ACC_STRANGER);
    rc = pmix_server_user_refresh(user);
    report("a user with no account belongs to no group",
           PMIX_SUCCESS == rc && 0 == user->ngids);
    PMIX_RELEASE(user);

    user = pmix_server_user_get((uid_t) ACC_STRANGER);
    again = pmix_server_user_get((uid_t) ACC_STRANGER);
    report("the server keeps one record per user", NULL != user && user == again);

    /* the host registers a user AND its groups: taken as given, nothing
     * looked up - this user has no account, so a lookup would find none */
    {
        pmix_info_t reginfo[2];
        pmix_data_array_t *gda;
        pmix_access_t job;
        uint32_t fake = ACC_USER + 7, jgid = ACC_GROUP, owner = ACC_OWNER;

        PMIX_INFO_LOAD(&reginfo[0], PMIX_USERID, &fake, PMIX_UINT32);
        PMIX_DATA_ARRAY_CREATE(gda, 2, PMIX_UINT32);
        ((uint32_t *) gda->array)[0] = ACC_OGROUP;
        ((uint32_t *) gda->array)[1] = ACC_GROUP;
        PMIX_INFO_LOAD(&reginfo[1], PMIX_GRPID, gda, PMIX_DATA_ARRAY);
        PMIX_DATA_ARRAY_FREE(gda);
        rc = PMIx_server_register_resources(reginfo, 2, NULL, NULL);
        progress_barrier();
        user = pmix_server_user_get((uid_t) fake);
        report("a user registered with its groups keeps the host's groups",
               registered(rc) && NULL != user && 2 == user->ngids &&
               has_gid(user, ACC_OGROUP) && has_gid(user, ACC_GROUP));
        pmix_server_access_construct(&job);
        PMIX_INFO_LOAD(&info, PMIX_USERID, &owner, PMIX_UINT32);
        (void) pmix_server_access_load(&job, &info, 1);
        PMIX_INFO_DESTRUCT(&info);
        job.gids = &jgid;
        job.ngids = 1;
        report("the rule uses the host's groups",
               PMIX_SUCCESS == pmix_server_access_check(user, &job));
        job.gids = NULL;
        job.ngids = 0;
        pmix_server_access_destruct(&job);
        rc = PMIx_server_deregister_resources(reginfo, 1, NULL, NULL);
        progress_barrier();
        PMIX_INFO_DESTRUCT(&reginfo[0]);
        PMIX_INFO_DESTRUCT(&reginfo[1]);
        user = pmix_server_user_get((uid_t) fake);
        report("deregistering it drops the host's groups",
               registered(rc) && NULL != user && 0 == user->ngids);
        pmix_server_user_remove((uid_t) fake);
    }

    pw = other_user();
    if (NULL == pw) {
        fprintf(stdout, "  SKIP: registering a user - no \"nobody\" account\n");
        return;
    }
    /* the host registers a user: its groups are looked up then */
    id = (uint32_t) pw->pw_uid;
    PMIX_INFO_LOAD(&info, PMIX_USERID, &id, PMIX_UINT32);
    rc = PMIx_server_register_resources(&info, 1, NULL, NULL);
    progress_barrier();
    user = pmix_server_user_get(pw->pw_uid);
    report("a user the host registers is known, with its groups",
           registered(rc) && NULL != user && has_gid(user, pw->pw_gid));
    rc = PMIx_server_deregister_resources(&info, 1, NULL, NULL);
    progress_barrier();
    PMIX_INFO_DESTRUCT(&info);
    user = pmix_server_user_get(pw->pw_uid);
    report("deregistering the user drops the record",
           registered(rc) && NULL != user && 0 == user->ngids);
    pmix_server_user_remove(pw->pw_uid);

    /* registering a job it owns makes the user known, groups and all */
    PMIX_INFO_LOAD(&info, PMIX_USERID, &id, PMIX_UINT32);
    rc = reg("acc-owned", 1, &info, 1);
    PMIX_INFO_DESTRUCT(&info);
    user = pmix_server_user_get(pw->pw_uid);
    report("a job's owner is known once the job is registered, with its groups",
           registered(rc) && NULL != user && has_gid(user, pw->pw_gid));
    pmix_server_user_remove(pw->pw_uid);
}

static void test_check(void)
{
    pmix_access_t job, bare;
    pmix_info_t info[3], perms[2];
    pmix_user_t *user;
    struct passwd *pw = other_user();
    uint32_t owner = ACC_OWNER, listed = ACC_USER, group, bogus = ACC_GROUP;

    pmix_server_access_construct(&job);
    PMIX_INFO_LOAD(&info[0], PMIX_USERID, &owner, PMIX_UINT32);
    load_ids(&perms[0], PMIX_ACCESS_USERIDS, &listed, 1);
    group = (NULL != pw) ? (uint32_t) pw->pw_gid : ACC_GROUP;
    load_ids(&perms[1], PMIX_ACCESS_GRPIDS, &group, 1);
    load_perms(&info[1], perms, 2);
    report("a host loads a job's owner and access list",
           PMIX_SUCCESS == pmix_server_access_load(&job, info, 2) &&
           PMIX_OWNER_REGISTERED == job.source && ACC_OWNER == job.uid && 1 == job.nuids &&
           1 == job.ngids);
    PMIX_INFO_DESTRUCT(&info[0]);
    PMIX_INFO_DESTRUCT(&info[1]);
    PMIX_INFO_DESTRUCT(&perms[0]);
    PMIX_INFO_DESTRUCT(&perms[1]);

    PMIX_INFO_LOAD(&info[0], PMIX_ACCESS_PERMISSIONS, "not-an-array", PMIX_STRING);
    report("a malformed access list is refused and changes nothing",
           PMIX_ERR_BAD_PARAM == pmix_server_access_load(&job, info, 1) && 1 == job.nuids &&
           ACC_USER == job.uids[0]);
    PMIX_INFO_DESTRUCT(&info[0]);

    user = pmix_server_user_create(0);
    report("the rule: root is allowed", PMIX_SUCCESS == pmix_server_access_check(user, &job));
    PMIX_RELEASE(user);
    user = pmix_server_user_create(geteuid());
    report("the rule: our own user is allowed",
           PMIX_SUCCESS == pmix_server_access_check(user, &job));
    PMIX_RELEASE(user);
    user = pmix_server_user_create(ACC_OWNER);
    report("the rule: the owner is allowed", PMIX_SUCCESS == pmix_server_access_check(user, &job));
    PMIX_RELEASE(user);
    user = pmix_server_user_create(ACC_USER);
    report("the rule: a listed user is allowed",
           PMIX_SUCCESS == pmix_server_access_check(user, &job));
    PMIX_RELEASE(user);
    user = pmix_server_user_create(ACC_STRANGER);
    report("the rule: anyone else is refused",
           PMIX_ERR_NO_PERMISSIONS == pmix_server_access_check(user, &job));
    PMIX_RELEASE(user);

    if (NULL != pw) {
        user = pmix_server_user_create(pw->pw_uid);
        report("the rule: a member of a listed group is allowed, its groups looked up then",
               PMIX_SUCCESS == pmix_server_access_check(user, &job) &&
               has_gid(user, pw->pw_gid));
        /* a group list gone stale - the user has since joined the group */
        free(user->gids);
        user->gids = (gid_t *) malloc(sizeof(gid_t));
        user->gids[0] = (gid_t) bogus;
        user->ngids = 1;
        report("the rule: a stale group list is looked up again before refusing",
               PMIX_SUCCESS == pmix_server_access_check(user, &job) &&
               has_gid(user, pw->pw_gid));

        /* groups count only for a job that names them */
        pmix_server_access_construct(&bare);
        PMIX_INFO_LOAD(&info[0], PMIX_USERID, &owner, PMIX_UINT32);
        (void) pmix_server_access_load(&bare, info, 1);
        PMIX_INFO_DESTRUCT(&info[0]);
        report("the rule: groups do not count for a job that names none",
               PMIX_ERR_NO_PERMISSIONS == pmix_server_access_check(user, &bare));
        pmix_server_access_destruct(&bare);
        PMIX_RELEASE(user);

        /* names are resolved */
        pmix_server_access_construct(&bare);
        PMIX_INFO_LOAD(&info[0], PMIX_ACCESS_USERIDS, pw->pw_name, PMIX_STRING);
        report("a user named in an access list is resolved to its number",
               PMIX_SUCCESS == pmix_server_access_load(&bare, info, 1) && 1 == bare.nuids &&
               (uint32_t) pw->pw_uid == bare.uids[0]);
        PMIX_INFO_DESTRUCT(&info[0]);
        pmix_server_access_destruct(&bare);
    } else {
        fprintf(stdout, "  SKIP: group membership - no \"nobody\" account\n");
    }

    /* a job with no owner known is ours */
    pmix_server_access_construct(&bare);
    user = pmix_server_user_create(ACC_OWNER);
    report("the rule: with no owner known, only root and our own user are allowed",
           PMIX_ERR_NO_PERMISSIONS == pmix_server_access_check(user, &bare));
    PMIX_RELEASE(user);
    pmix_server_access_destruct(&bare);
    pmix_server_access_destruct(&job);
}

int main(int argc, char **argv)
{
    static pmix_server_module_t mymodule = {0};
    pmix_status_t rc;

    (void) argc;
    (void) argv;

    fprintf(stdout, "server_access: access by user and group unit tests\n");

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    test_registered_policy();
    test_job_info_array();
    test_names();
    test_malformed();
    test_owner_fallback();
    test_users();
    test_check();

    PMIx_server_finalize();

    fprintf(stdout, "server_access: %d passed, %d failed\n", npass, nfail);
    return (0 == nfail) ? 0 : 1;
}
