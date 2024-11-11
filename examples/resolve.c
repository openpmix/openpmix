#define _GNU_SOURCE
#include <pmix.h>
#include <stdio.h>
#include "examples.h"

#define MIN(a,b) (((a)<(b))?(a):(b))

#define SPAWN_PROCS 2
#define SPAWN_NSPACE "spawn.nspace"

/* PMIx error checking */
#define CHECK_PMIX_ERR(pmix_errno, func_name, proc) \
    if (pmix_errno != PMIX_SUCCESS) { \
        printf("[%s:%u]: Error on %s: %s\n", proc.nspace, proc.rank, \
               func_name, PMIx_Error_string(pmix_errno)); \
        goto fn_fail; \
    }

#define CHECK_ERR(err) if (err > 0) { goto fn_fail; } \

/* Information about process */
static pmix_proc_t own_proc;
static pmix_proc_t parent_proc;
static size_t job_size;
static size_t parent_job_size;
static bool is_spawned;

static size_t nprocs = 0;

/* Status infos and other */
static int number_of_inits = 0;
static bool have_spawn = false;
static char child_nspace[PMIX_MAX_NSLEN + 1];

/* Connect to PMIx server, get size of own namespace, and spawned status/ parent process */
static
int do_basic_init(void)
{
    pmix_status_t rc;
    pmix_proc_t wp;
    pmix_value_t *val = NULL;
    int err = 0;
    char hostname[1024];

    rc = PMIx_Init(&own_proc, NULL, 0);
    CHECK_PMIX_ERR(rc, "PMIx_Init", own_proc);
    gethostname(hostname, sizeof(hostname));

    printf("[%s:%u]: Running on node %s\n", own_proc.nspace, own_proc.rank, hostname);

    /* Get job size */
    PMIX_PROC_CONSTRUCT(&wp);
    PMIX_LOAD_PROCID(&wp, own_proc.nspace, PMIX_RANK_WILDCARD);
    rc = PMIx_Get(&wp, PMIX_JOB_SIZE, NULL, 0, &val);
    CHECK_PMIX_ERR(rc, "PMIx_Get job size", own_proc);
    job_size = val->data.uint32;
    PMIX_VALUE_RELEASE(val);

    /* Get parent */
    rc = PMIx_Get(&own_proc, PMIX_PARENT_ID, NULL, 0, &val);
    if (rc == PMIX_ERR_NOT_FOUND) {
        is_spawned = false;     /* process not spawned */
    } else if (rc == PMIX_SUCCESS) {
        is_spawned = true;     /* spawned process */
        PMIX_PROC_LOAD(&parent_proc, val->data.proc->nspace, val->data.proc->rank);
        PMIX_VALUE_RELEASE(val);
    } else {
        is_spawned = false;
        CHECK_PMIX_ERR(rc, "PMIx_Get parent ID", own_proc);
    }

  fn_exit:
    return err;
  fn_fail:
    err = 1;
    goto fn_exit;

}

/* Connect specified processes */
static
int connect_procs(void)
{
    pmix_proc_t procs[2];
    int rc;

    if (is_spawned) {
        // children load their own_proc nspace
        PMIX_PROC_LOAD(&procs[0], own_proc.nspace, PMIX_RANK_WILDCARD);
        // and their parent
        PMIX_PROC_LOAD(&procs[1], parent_proc.nspace, 0);
    } else {
        // parent loads the child nspace
        PMIX_PROC_LOAD(&procs[0], child_nspace, PMIX_RANK_WILDCARD);
        // and their own nspace
        PMIX_PROC_LOAD(&procs[1], own_proc.nspace, 0);
    }
    printf("[%s:%u]: Connect procs for %s with %s.0\n",
           own_proc.nspace, own_proc.rank, procs[0].nspace, procs[1].nspace);
    rc = PMIx_Connect(procs, 2, NULL, 0);
    return rc;
}

/* Get nspace of spawned processes (used in all processes that did NOT call PMIx_Spawn) */
static
int get_spawned_nspace(void)
{
    int err = 0;
    int pid = getpid();

    pmix_status_t rc;
    pmix_value_t *val = NULL;
    rc = PMIx_Get(&parent_proc, SPAWN_NSPACE, NULL, 0, &val);
    CHECK_PMIX_ERR(rc, "PMIx_Get spawned namespace name", own_proc);
    strncpy(child_nspace, val->data.string, PMIX_MAX_NSLEN);
    printf("[%s:%u]: Get spawned nspace (round %d, pid = %d) Result: %s Child %s\n",
           own_proc.nspace, own_proc.rank, number_of_inits, pid,
           PMIx_Error_string(rc), child_nspace);
    PMIX_VALUE_RELEASE(val);

  fn_exit:
    return err;
  fn_fail:
    err = 1;
    goto fn_exit;
}


static
int prepare(void)
{
    int err = 0;
    nprocs = 0;
    number_of_inits++;

    int pid = getpid();

    printf("[%s:%u]: Init (round %d, pid = %d)\n", own_proc.nspace, own_proc.rank, number_of_inits, pid);

    /* Create array with all active processes */
    if (number_of_inits == 1) {
        /* 1st init */

        /* All processes in own namespace MUST be active */
        nprocs += job_size;

        if (is_spawned) {
            err = connect_procs();
            CHECK_ERR(err);

            /* Get job size of parent's nspace */
            pmix_status_t rc;
            pmix_proc_t wp;
            PMIX_PROC_LOAD(&wp, parent_proc.nspace, PMIX_RANK_WILDCARD);
            pmix_value_t *val = NULL;
            rc = PMIx_Get(&wp, PMIX_JOB_SIZE, NULL, 0, &val);
            CHECK_PMIX_ERR(rc, "PMIx_get size of parent's nspace", own_proc);
            parent_job_size = val->data.uint32;
            nprocs += parent_job_size;
            PMIX_VALUE_RELEASE(val);
        }
    } else {
        /* From 2nd init onward */
        if (have_spawn) {

             if (PMIX_CHECK_PROCID(&parent_proc, &own_proc)) {
                err = connect_procs();
                CHECK_ERR(err);
             } else {
                /* Get name of new namespace */
                err = get_spawned_nspace();
                CHECK_ERR(err);
             }
             /* add size of spawned namespace to nprocs */
             pmix_status_t rc;
             pmix_proc_t wp;
             PMIX_PROC_LOAD(&wp, child_nspace, PMIX_RANK_WILDCARD);
             pmix_value_t *val = NULL;
             rc = PMIx_Get(&wp, PMIX_JOB_SIZE, NULL, 0, &val);
             CHECK_PMIX_ERR(rc, "PMIx_get size of spawned nspace", own_proc);
             nprocs += val->data.uint32;
             PMIX_VALUE_RELEASE(val);

             have_spawn = false;
        }

        nprocs += job_size;
    }

  fn_exit:
    return err;
  fn_fail:
    err = 1;
    goto fn_exit;
}

/* Spawn new processes and put new nspace into KVS (called by parent process) */
static
int do_spawn(void)
{
    int err = 0;
    pmix_status_t rc = PMIX_SUCCESS;
    pmix_proc_t jobproc;

    /* Rank 0 of original namespace is the parent of the spawn */
    PMIX_PROC_LOAD(&parent_proc, own_proc.nspace, 0);
    have_spawn = true;

    if (PMIX_CHECK_PROCID(&own_proc, &parent_proc)) {
        printf("[%s:%u] Spawning %d new processes.\n", own_proc.nspace, own_proc.rank, SPAWN_PROCS);
        pmix_app_t *apps = NULL;
        PMIX_APP_CREATE(apps, 1);
        if (asprintf(&apps[0].cmd, "%s", "./node_map") < 0) {
            return 1;
        }
        apps[0].maxprocs = SPAWN_PROCS;
        apps[0].ninfo = 0;
        apps[0].argv = NULL;
        apps[0].env = NULL;

        /* spawn new procs */
        rc = PMIx_Spawn(NULL, 0, apps, 1, child_nspace);
        CHECK_PMIX_ERR(rc, "PMIx_Spawn", own_proc);
        PMIX_APP_FREE(apps, 1);

        /* parent puts new child nspace into KVS */
        pmix_value_t val;
        PMIX_VALUE_LOAD(&val, child_nspace, PMIX_STRING);
        rc = PMIx_Put(PMIX_GLOBAL, SPAWN_NSPACE, &val);
        CHECK_PMIX_ERR(rc, "PMIx_Put child namespace (parent)", own_proc);
        rc = PMIx_Commit();
        CHECK_PMIX_ERR(rc, "PMIx_Commit", own_proc);
    }
    // circulate the name of the child nspace
    PMIX_PROC_LOAD(&jobproc, own_proc.nspace, PMIX_RANK_WILDCARD);
    err = PMIx_Fence(&jobproc, 1, NULL, 0);
    CHECK_ERR(err);

  fn_exit:
    return err;
  fn_fail:
    err = 1;
    goto fn_exit;
}

static
int get_node_list(char ***nodelist)
{
    int err = 0;
    int u, n;
    pmix_status_t rc = PMIX_SUCCESS;
    char *nodes = NULL;
    char **list, **retain=NULL;
    char nspace[PMIX_MAX_NSLEN + 1];
    bool is_duplicate;

    /* Make sure that nodelist has same order of nodes in all processes */
    for (int i = 0; i < 2; i++) {
        if (i == 0) {
            /* Resolve nodes of own/ parent nspace */
            if (is_spawned) {
                strncpy(nspace, parent_proc.nspace, PMIX_MAX_NSLEN);
            } else {
                strncpy(nspace, own_proc.nspace, PMIX_MAX_NSLEN);
            }
        } else {
            /* Resolve nodes of child/ own nspace */
            if (is_spawned) {
                strncpy(nspace, own_proc.nspace, PMIX_MAX_NSLEN);
            } else {
                strncpy(nspace, child_nspace, PMIX_MAX_NSLEN);
            }
        }
        rc = PMIx_Resolve_nodes(nspace, &nodes);
        CHECK_PMIX_ERR(rc, "PMIx_Resolve_nodes", own_proc);

        /* Add non-duplicate nodes to list */
        list = PMIx_Argv_split(nodes, ',');
        for (u=0; NULL != list[u]; u++) {
            is_duplicate = false;
            for (n=0; NULL != retain && NULL != retain[n]; n++) {
                if (0 == strncmp(list[u], retain[n], MIN(strlen(list[u]), strlen(retain[n])))) {
                    is_duplicate = true;
                }
            }
            if (!is_duplicate) {
                PMIx_Argv_append_nosize(&retain, list[u]);
            }
        }
        free(nodes);
        PMIx_Argv_free(list);
    }

    *nodelist = retain;

  fn_exit:
    return err;
  fn_fail:
    err = 1;
    goto fn_exit;
}

static
void get_proc_idx(pmix_proc_t p, int *idx)
{
    int id = -1;

    if (is_spawned) {
        if(PMIX_CHECK_NSPACE(p.nspace, parent_proc.nspace)) {
            id = p.rank;
        } else {
            // p is a spawned proc, add parent job size
            id = p.rank + parent_job_size;
        }

    } else {
        if (PMIX_CHECK_NSPACE(p.nspace, own_proc.nspace)) {
            id = p.rank;
        } else {
            // p is a spawned proc, add job size
            id = p.rank + job_size;
        }
    }

    *idx = id;
}

static
int create_node_map(void)
{
    int err = 0;
    pmix_status_t rc = PMIX_SUCCESS;
    int *map = NULL; /* Contains id of node on which each process is running, starting from 0 */
    int *used = NULL; /* Contains either 0 (not used) or 1 (used) for each node */
    char **nodelist = NULL;
    int nodecount;

    /* Get list all nodes */
    err = get_node_list(&nodelist);
    CHECK_ERR(err);

    /* Allocate mem for nodemap and node used status */
    map = (int *) malloc(nprocs * sizeof(int));
    if (!map) {
        goto fn_fail;
    }
    nodecount = PMIx_Argv_count(nodelist);
    used = (int *) malloc(nodecount * sizeof(int));
    if (!used) {
        goto fn_fail;
    }

    /* mark all nodes as unused */
    memset(used, 0, nodecount * sizeof(int));
    memset(map, -1, nprocs * sizeof(int));

    /* Iterate over nodes to get processes (peers) running per node */
    for (size_t i = 0; NULL != nodelist[i]; i++) {
        char *node = nodelist[i];
        char nspace[PMIX_MAX_NSLEN +1];
        for (int n = 0; n < 2; n++) {
            if (n == 0) {
                /* Resolve peers of own/ parent nspace on node*/
                if (is_spawned) {
                    strncpy(nspace, parent_proc.nspace, PMIX_MAX_NSLEN);
                } else {
                    strncpy(nspace, own_proc.nspace, PMIX_MAX_NSLEN);
                }
            } else {
                /* Resolve peers of child/ own nspace on node*/
                if (is_spawned) {
                    strncpy(nspace, own_proc.nspace, PMIX_MAX_NSLEN);
                } else {
                    if (0 == strlen(child_nspace)) {
                        break;
                    }
                    strncpy(nspace, child_nspace, PMIX_MAX_NSLEN);
                }
            }
            pmix_proc_t * node_procs = NULL;
            size_t nnode_procs = 0;

            rc = PMIx_Resolve_peers(node, nspace,  &node_procs, &nnode_procs);
            if (rc == PMIX_ERR_NOT_FOUND) {
                printf("[%s:%d] resolving peers: nspace %s has no procs on node %s\n",
                        own_proc.nspace, own_proc.rank, nspace, node);
            } else if (rc != PMIX_SUCCESS) {
                CHECK_PMIX_ERR(rc, "PMIx_Resolve_peers", own_proc);
            }

            if (nnode_procs > 0) {
                used[i] = 1; /* Remember if node is used */
                /* Iterate over peers to set their node id */
                for (unsigned j = 0; j < nnode_procs; j++) {
                    int idx = 0;
                    get_proc_idx(node_procs[j], &idx);
                    map[idx] = i;
                }
                PMIX_PROC_FREE(node_procs, nnode_procs);
            }
        }
    }

    /* Print for debugging */
    char out_nodemap[2048];
    char out_nodeused[2048];
    int c = 0;
    for (size_t i = 0; i < nprocs; i++) {
        c += sprintf(&out_nodemap[c], "%d ", map[i]);
    }
    c = 0;
    for (int i = 0; NULL != nodelist[i]; i++) {
        c += sprintf(&out_nodeused[c], "%d ", used[i]);
    }

    printf("[%s:%d] map: %s ### used %s\n", own_proc.nspace, own_proc.rank, out_nodemap, out_nodeused);

    PMIx_Argv_free(nodelist);

    free(map);
    free(used);

  fn_exit:
    return err;
  fn_fail:
    err = 1;
    goto fn_exit;
}

int main(int argc, char **argv)
{
    int err = 0;
    pmix_status_t rc;
    EXAMPLES_HIDE_UNUSED_PARAMS(argc, argv);

    err = do_basic_init();
    CHECK_ERR(err);

    err = prepare();
    CHECK_ERR(err);

    err = create_node_map();
    CHECK_ERR(err);

    if (!is_spawned) {
        err = do_spawn();
        CHECK_ERR(err);

        err = prepare();
        CHECK_ERR(err);

        err = create_node_map();
        CHECK_ERR(err);
    }

  fn_exit:
    rc = PMIx_Finalize(NULL, 0);
    CHECK_PMIX_ERR(rc, "PMIx_finalize", own_proc);
    printf("[%s:%u]: Bye.\n", own_proc.nspace, own_proc.rank);
    return 0;
  fn_fail:
    printf("[%s:%u]: ERROR!\n", own_proc.nspace, own_proc.rank);
    goto fn_exit;
}
