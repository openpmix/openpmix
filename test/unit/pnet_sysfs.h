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
 * A captured sysfs tree for the pnet tests.
 *
 * Two components read sysfs for what the topology does not record.  nvd
 * reads each RDMA device's node type from
 * <root>/class/infiniband/<dev>/node_type, which is how it tells an
 * InfiniBand or RoCE adapter from an iWARP one.  cxi finds the CXI device
 * of a Slingshot interface under <root>/class/net/<hsnN>/device/cxi/, which
 * an hwloc older than 2.14 does not report.  The machines these tests run
 * on mostly have no such tree, and none has the devices the fixtures
 * describe, so a test builds the entries it needs under a temporary
 * directory and points the component's sysfs_root parameter at it.
 */

#ifndef PMIX_TEST_PNET_SYSFS_H
#define PMIX_TEST_PNET_SYSFS_H

#include "src/util/pmix_argv.h"
#include "src/util/pmix_printf.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char *root;
    /* everything created, in creation order, so it can be removed in reverse */
    char **made;
} pnet_sysfs_t;

static inline bool pnet_sysfs_mkdir(pnet_sysfs_t *s, const char *path)
{
    struct stat st;

    if (0 == stat(path, &st)) {
        return S_ISDIR(st.st_mode);
    }
    if (0 != mkdir(path, 0700)) {
        return false;
    }
    PMIx_Argv_append_nosize(&s->made, path);
    return true;
}

static inline bool pnet_sysfs_init(pnet_sysfs_t *s)
{
    const char *tmp = getenv("TMPDIR");
    char *base = NULL, *path = NULL;
    bool ret;

    s->root = NULL;
    s->made = NULL;
    pmix_asprintf(&base, "%s/pnet-sysfs-XXXXXX", (NULL == tmp) ? "/tmp" : tmp);
    if (NULL == base || NULL == mkdtemp(base)) {
        free(base);
        return false;
    }
    s->root = base;
    PMIx_Argv_append_nosize(&s->made, base);
    pmix_asprintf(&path, "%s/class", base);
    ret = (NULL != path && pnet_sysfs_mkdir(s, path));
    free(path);
    path = NULL;
    if (!ret) {
        return false;
    }
    pmix_asprintf(&path, "%s/class/infiniband", base);
    ret = (NULL != path && pnet_sysfs_mkdir(s, path));
    free(path);
    return ret;
}

/* record "dev" as a verbs device whose node_type file reads "type" */
static inline bool pnet_sysfs_add(pnet_sysfs_t *s, const char *dev, const char *type)
{
    char *dir = NULL, *file = NULL;
    FILE *fp;
    bool ret = false;

    pmix_asprintf(&dir, "%s/class/infiniband/%s", s->root, dev);
    pmix_asprintf(&file, "%s/class/infiniband/%s/node_type", s->root, dev);
    if (NULL == dir || NULL == file || !pnet_sysfs_mkdir(s, dir)) {
        goto out;
    }
    fp = fopen(file, "w");
    if (NULL == fp) {
        goto out;
    }
    fprintf(fp, "%s\n", type);
    fclose(fp);
    PMIx_Argv_append_nosize(&s->made, file);
    ret = true;
out:
    free(dir);
    free(file);
    return ret;
}

/* create the directory "rel" under the root, and every directory above it */
static inline bool pnet_sysfs_add_dir(pnet_sysfs_t *s, const char *rel)
{
    char **parts;
    char *path = NULL, *next = NULL;
    bool ret = true;
    int n;

    parts = PMIx_Argv_split(rel, '/');
    if (NULL == parts) {
        return false;
    }
    path = strdup(s->root);
    for (n = 0; ret && NULL != path && NULL != parts[n]; n++) {
        pmix_asprintf(&next, "%s/%s", path, parts[n]);
        free(path);
        path = next;
        next = NULL;
        ret = (NULL != path && pnet_sysfs_mkdir(s, path));
    }
    if (NULL == path) {
        ret = false;
    }
    free(path);
    PMIx_Argv_free(parts);
    return ret;
}

static inline void pnet_sysfs_remove(pnet_sysfs_t *s)
{
    int n;

    if (NULL != s->made) {
        n = PMIx_Argv_count(s->made);
        while (0 < n--) {
            if (0 != unlink(s->made[n])) {
                (void) rmdir(s->made[n]);
            }
        }
        PMIx_Argv_free(s->made);
        s->made = NULL;
    }
    s->root = NULL; /* it was the first entry of "made" */
}

#endif
