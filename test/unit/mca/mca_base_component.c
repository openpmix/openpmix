/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the component-level helpers in src/mca/base:
 * the comparison/rendering routines in
 * pmix_mca_base_component_compare.c, the "--mca <framework> a,b,^c"
 * parser in pmix_mca_base_component_find.c, and the component name
 * aliasing in pmix_mca_base_alias.c.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pmix_common.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/base/pmix_mca_base_alias.h"
#include "src/mca/mca.h"
#include "src/util/pmix_argv.h"

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

/* ------------------------------------------------------------------ */
/* component compare                                                   */
/* ------------------------------------------------------------------ */

static void fill(pmix_mca_base_component_t *c, const char *type, const char *name, int major,
                 int minor, int release)
{
    memset(c, 0, sizeof(*c));
    pmix_strncpy(c->pmix_mca_project_name, "pmix", PMIX_MCA_BASE_MAX_PROJECT_NAME_LEN);
    pmix_strncpy(c->pmix_mca_type_name, type, PMIX_MCA_BASE_MAX_TYPE_NAME_LEN);
    pmix_strncpy(c->pmix_mca_component_name, name, PMIX_MCA_BASE_MAX_COMPONENT_NAME_LEN);
    c->pmix_mca_component_major_version = major;
    c->pmix_mca_component_minor_version = minor;
    c->pmix_mca_component_release_version = release;
}

/* compare() is an *inverse* ordering: it returns a negative number for
 * the entry that should sort to the head of a list. */
static void test_compare(void)
{
    pmix_mca_base_component_t a, b;

    fill(&a, "ptl", "tcp", 1, 0, 0);
    fill(&b, "ptl", "tcp", 1, 0, 0);
    report("compare: identical components are equal", 0 == pmix_mca_base_component_compare(&a, &b));
    report("compatible: identical components are compatible",
           0 == pmix_mca_base_component_compatible(&a, &b));

    fill(&b, "ptl", "tcp", 2, 0, 0);
    report("compare: higher major sorts first", 0 < pmix_mca_base_component_compare(&a, &b));
    report("compare: is antisymmetric in major", 0 > pmix_mca_base_component_compare(&b, &a));
    report("compatible: differing major is not compatible",
           0 != pmix_mca_base_component_compatible(&a, &b));

    fill(&b, "ptl", "tcp", 1, 3, 0);
    report("compare: higher minor sorts first", 0 < pmix_mca_base_component_compare(&a, &b));
    report("compatible: differing minor is not compatible",
           0 != pmix_mca_base_component_compatible(&a, &b));

    /* release version separates compare() from compatible() */
    fill(&b, "ptl", "tcp", 1, 0, 9);
    report("compare: release version is significant",
           0 != pmix_mca_base_component_compare(&a, &b));
    report("compatible: release version is ignored",
           0 == pmix_mca_base_component_compatible(&a, &b));

    fill(&b, "ptl", "server", 1, 0, 0);
    report("compare: different component names differ",
           0 != pmix_mca_base_component_compare(&a, &b));

    fill(&b, "gds", "tcp", 1, 0, 0);
    report("compare: different framework names differ",
           0 != pmix_mca_base_component_compare(&a, &b));
}

static void test_to_string(void)
{
    pmix_mca_base_component_t a;
    char *str;

    fill(&a, "ptl", "tcp", 6, 1, 4);
    str = pmix_mca_base_component_to_string(&a);
    report("to_string renders type.name.major.minor",
           NULL != str && 0 == strcmp(str, "ptl.tcp.6.1"));
    free(str);
}

/* ------------------------------------------------------------------ */
/* parse_requested                                                     */
/* ------------------------------------------------------------------ */

static void test_parse_requested(void)
{
    char **names = NULL;
    bool include_mode = false;
    int rc;

    /* NULL and empty select everything */
    rc = pmix_mca_base_component_parse_requested(NULL, &include_mode, &names);
    report("parse: NULL selects everything",
           PMIX_SUCCESS == rc && NULL == names && include_mode);

    rc = pmix_mca_base_component_parse_requested("", &include_mode, &names);
    report("parse: empty selects everything", PMIX_SUCCESS == rc && NULL == names && include_mode);

    /* plain include list */
    rc = pmix_mca_base_component_parse_requested("tcp,server", &include_mode, &names);
    report("parse: include list", PMIX_SUCCESS == rc && NULL != names && include_mode);
    if (NULL != names) {
        report("parse: include list has both names",
               2 == PMIx_Argv_count(names) && 0 == strcmp(names[0], "tcp")
                   && 0 == strcmp(names[1], "server"));
        PMIx_Argv_free(names);
        names = NULL;
    }

    /* exclude list */
    rc = pmix_mca_base_component_parse_requested("^tcp", &include_mode, &names);
    report("parse: exclude list flips include_mode",
           PMIX_SUCCESS == rc && NULL != names && !include_mode);
    if (NULL != names) {
        report("parse: exclude list drops the negate character",
               1 == PMIx_Argv_count(names) && 0 == strcmp(names[0], "tcp"));
        PMIx_Argv_free(names);
        names = NULL;
    }

    /* several leading negates are tolerated */
    rc = pmix_mca_base_component_parse_requested("^^^tcp", &include_mode, &names);
    report("parse: repeated leading negates tolerated",
           PMIX_SUCCESS == rc && NULL != names && !include_mode);
    if (NULL != names) {
        report("parse: repeated negates all stripped",
               1 == PMIx_Argv_count(names) && 0 == strcmp(names[0], "tcp"));
        PMIx_Argv_free(names);
        names = NULL;
    }

    /* a negate anywhere but the front is an error */
    rc = pmix_mca_base_component_parse_requested("tcp,^server", &include_mode, &names);
    report("parse: embedded negate is refused", PMIX_SUCCESS != rc);
    if (NULL != names) {
        PMIx_Argv_free(names);
        names = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* aliases                                                             */
/* ------------------------------------------------------------------ */

static void test_alias(void)
{
    const pmix_mca_base_alias_t *alias;
    pmix_mca_base_alias_item_t *item;
    int nfound;
    int rc;

    report("alias: lookup of an unregistered name returns NULL",
           NULL == pmix_mca_base_alias_lookup("pmix", "utest", "nothing"));

    rc = pmix_mca_base_alias_register("pmix", "utest", "real", "nickname", 0);
    report("alias: register succeeds", PMIX_SUCCESS == rc);

    alias = pmix_mca_base_alias_lookup("pmix", "utest", "real");
    report("alias: lookup finds it", NULL != alias);
    if (NULL != alias) {
        nfound = 0;
        PMIX_LIST_FOREACH (item, (pmix_list_t *) &alias->component_aliases,
                           pmix_mca_base_alias_item_t) {
            if (0 == strcmp(item->component_alias, "nickname")) {
                nfound++;
            }
        }
        report("alias: the registered name is on the list", 1 == nfound);
    }

    /* a second alias for the same component joins the same list */
    rc = pmix_mca_base_alias_register("pmix", "utest", "real", "othername", 0);
    report("alias: second register succeeds", PMIX_SUCCESS == rc);
    alias = pmix_mca_base_alias_lookup("pmix", "utest", "real");
    if (NULL != alias) {
        nfound = 0;
        PMIX_LIST_FOREACH (item, (pmix_list_t *) &alias->component_aliases,
                           pmix_mca_base_alias_item_t) {
            nfound++;
        }
        report("alias: both aliases are on one list", 2 == nfound);
    }

    /* the project and framework are part of the key */
    report("alias: a different framework does not match",
           NULL == pmix_mca_base_alias_lookup("pmix", "other", "real"));

    /* NULL project/framework is a distinct, legal key */
    rc = pmix_mca_base_alias_register(NULL, NULL, "bare", "barealias", 0);
    report("alias: register with no project/framework", PMIX_SUCCESS == rc);
    report("alias: lookup with no project/framework",
           NULL != pmix_mca_base_alias_lookup(NULL, NULL, "bare"));

    /* a NULL component name is a programming error, not a crash */
    report("alias: register with a NULL component is refused",
           PMIX_ERR_BAD_PARAM == pmix_mca_base_alias_register("pmix", "utest", NULL, "x", 0));
    report("alias: lookup with a NULL component returns NULL",
           NULL == pmix_mca_base_alias_lookup("pmix", "utest", NULL));

    pmix_mca_base_alias_cleanup();
    report("alias: lookup after cleanup returns NULL",
           NULL == pmix_mca_base_alias_lookup("pmix", "utest", "real"));
}

int main(int argc, char **argv)
{
    (void) argc;
    (void) argv;

    fprintf(stdout, "\n=== mca_base component helper unit tests ===\n\n");

    test_compare();
    test_to_string();
    test_parse_requested();
    test_alias();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
