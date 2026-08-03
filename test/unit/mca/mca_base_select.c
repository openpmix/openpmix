/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the generic component selection and filtering in
 * src/mca/base: pmix_mca_base_select() (query every component, keep the
 * highest priority, close the rest) and
 * pmix_mca_base_components_filter() (trim a framework's component list
 * down to what its selection parameter asks for).
 *
 * The components here are fabricated in this file. They have names no
 * framework uses, no MCA variables, and no repository entry, which is
 * what makes it safe to let the base close and unload them: both of
 * those look the component up by name and find nothing to do.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pmix_common.h"
#include "src/class/pmix_list.h"
#include "src/mca/base/pmix_base.h"
#include "src/mca/mca.h"
#include "src/runtime/pmix_init_util.h"

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
/* fabricated components                                               */
/* ------------------------------------------------------------------ */

/* one module object per component, so the caller can tell which
 * component's module came back */
static pmix_mca_base_module_t mod_low;
static pmix_mca_base_module_t mod_high;
static pmix_mca_base_module_t mod_mid;

static int n_close_calls = 0;

static int query_low(pmix_mca_base_module_t **module, int *priority)
{
    *module = &mod_low;
    *priority = 10;
    return PMIX_SUCCESS;
}

static int query_high(pmix_mca_base_module_t **module, int *priority)
{
    *module = &mod_high;
    *priority = 90;
    return PMIX_SUCCESS;
}

static int query_mid(pmix_mca_base_module_t **module, int *priority)
{
    *module = &mod_mid;
    *priority = 50;
    return PMIX_SUCCESS;
}

/* a component that declines by returning an error */
static int query_declines(pmix_mca_base_module_t **module, int *priority)
{
    *module = NULL;
    *priority = 0;
    return PMIX_ERR_NOT_AVAILABLE;
}

/* a component that succeeds but hands back no module */
static int query_no_module(pmix_mca_base_module_t **module, int *priority)
{
    *module = NULL;
    *priority = 1000;
    return PMIX_SUCCESS;
}

/* a component that says "stop -- do not pick anyone else" */
static int query_fatal(pmix_mca_base_module_t **module, int *priority)
{
    *module = NULL;
    *priority = 0;
    return PMIX_ERR_FATAL;
}

static int count_close(void)
{
    n_close_calls++;
    return PMIX_SUCCESS;
}

static pmix_mca_base_component_t *make_component(const char *name,
                                                 pmix_mca_base_query_component_2_0_0_fn_t query)
{
    pmix_mca_base_component_t *c = calloc(1, sizeof(*c));
    if (NULL == c) {
        return NULL;
    }
    pmix_strncpy(c->pmix_mca_project_name, "pmix", PMIX_MCA_BASE_MAX_PROJECT_NAME_LEN);
    pmix_strncpy(c->pmix_mca_type_name, "utestsel", PMIX_MCA_BASE_MAX_TYPE_NAME_LEN);
    pmix_strncpy(c->pmix_mca_component_name, name, PMIX_MCA_BASE_MAX_COMPONENT_NAME_LEN);
    c->pmix_mca_query_component = query;
    c->pmix_mca_close_component = count_close;
    return c;
}

/* Build a list of component list items. The caller owns the component
 * structs; pmix_mca_base_select() releases the list items. */
static void add_component(pmix_list_t *list, pmix_mca_base_component_t *c)
{
    pmix_mca_base_component_list_item_t *cli = PMIX_NEW(pmix_mca_base_component_list_item_t);
    cli->cli_component = c;
    pmix_list_append(list, &cli->super);
}

/* ------------------------------------------------------------------ */
/* pmix_mca_base_select                                                */
/* ------------------------------------------------------------------ */

static void test_select_highest_priority(void)
{
    pmix_list_t list;
    pmix_mca_base_module_t *module = NULL;
    pmix_mca_base_component_t *component = NULL;
    pmix_mca_base_component_t *lo, *hi, *mid;
    int priority = -1, rc;

    PMIX_CONSTRUCT(&list, pmix_list_t);
    lo = make_component("lo", query_low);
    hi = make_component("hi", query_high);
    mid = make_component("mid", query_mid);
    /* deliberately not in priority order */
    add_component(&list, lo);
    add_component(&list, hi);
    add_component(&list, mid);

    n_close_calls = 0;
    rc = pmix_mca_base_select("utestsel", 0, &list, &module, &component, &priority);
    report("select succeeds", PMIX_SUCCESS == rc);
    report("select picks the highest priority module", &mod_high == module);
    report("select reports the winning component", hi == component);
    report("select reports the winning priority", 90 == priority);
    report("select closed the two losers, not the winner", 2 == n_close_calls);
    report("select left only the winner on the list", 1 == pmix_list_get_size(&list));

    PMIX_LIST_DESTRUCT(&list);
    free(lo);
    free(hi);
    free(mid);
}

/* A component with no query function at all is skipped, as is one whose
 * query declines or returns no module. */
static void test_select_skips(void)
{
    pmix_list_t list;
    pmix_mca_base_module_t *module = NULL;
    pmix_mca_base_component_t *component = NULL;
    pmix_mca_base_component_t *none, *decl, *nomod, *lo;
    int priority = -1, rc;

    PMIX_CONSTRUCT(&list, pmix_list_t);
    none = make_component("noquery", NULL);
    decl = make_component("declines", query_declines);
    nomod = make_component("nomodule", query_no_module);
    lo = make_component("lo", query_low);
    add_component(&list, none);
    add_component(&list, decl);
    add_component(&list, nomod);
    add_component(&list, lo);

    rc = pmix_mca_base_select("utestsel", 0, &list, &module, &component, &priority);
    report("select succeeds past the skipped components", PMIX_SUCCESS == rc);
    report("a component with no query function is skipped", lo == component);
    report("a declining component is skipped", &mod_low == module);
    /* nomodule claimed priority 1000 but returned no module, so it must
     * not have won despite outranking everyone */
    report("a component that returns no module cannot win", 10 == priority);

    PMIX_LIST_DESTRUCT(&list);
    free(none);
    free(decl);
    free(nomod);
    free(lo);
}

/* PMIX_ERR_FATAL from a query stops selection outright: the caller
 * asked for something that cannot be provided, and quietly picking a
 * different component would do the wrong thing silently. */
static void test_select_fatal_stops(void)
{
    pmix_list_t list;
    pmix_mca_base_module_t *module = NULL;
    pmix_mca_base_component_t *component = NULL;
    pmix_mca_base_component_t *fatal, *hi;
    int priority = -1, rc;

    PMIX_CONSTRUCT(&list, pmix_list_t);
    fatal = make_component("fatal", query_fatal);
    hi = make_component("hi", query_high);
    add_component(&list, fatal);
    add_component(&list, hi);

    n_close_calls = 0;
    rc = pmix_mca_base_select("utestsel", 0, &list, &module, &component, &priority);
    report("a fatal query aborts selection", PMIX_ERR_FATAL == rc);
    report("a fatal query selects nothing", NULL == component && NULL == module);
    report("a fatal query does not close the remaining components", 0 == n_close_calls);

    PMIX_LIST_DESTRUCT(&list);
    free(fatal);
    free(hi);
}

static void test_select_nothing_available(void)
{
    pmix_list_t list;
    pmix_mca_base_module_t *module = NULL;
    pmix_mca_base_component_t *component = NULL;
    pmix_mca_base_component_t *decl;
    int priority = -1, rc;

    PMIX_CONSTRUCT(&list, pmix_list_t);
    decl = make_component("declines", query_declines);
    add_component(&list, decl);

    n_close_calls = 0;
    rc = pmix_mca_base_select("utestsel", 0, &list, &module, &component, &priority);
    report("select with no viable component reports NOT_FOUND", PMIX_ERR_NOT_FOUND == rc);
    report("select with no viable component returns no module",
           NULL == module && NULL == component);
    report("select closed the component it could not use", 1 == n_close_calls);
    report("select emptied the list", 0 == pmix_list_get_size(&list));

    PMIX_LIST_DESTRUCT(&list);
    free(decl);
}

/* An empty list is not a crash. */
static void test_select_empty_list(void)
{
    pmix_list_t list;
    pmix_mca_base_module_t *module = NULL;
    pmix_mca_base_component_t *component = NULL;
    int rc;

    PMIX_CONSTRUCT(&list, pmix_list_t);
    /* priority_out is documented as optional */
    rc = pmix_mca_base_select("utestsel", 0, &list, &module, &component, NULL);
    report("select over an empty list reports NOT_FOUND", PMIX_ERR_NOT_FOUND == rc);
    report("select over an empty list returns no module",
           NULL == module && NULL == component);
    PMIX_LIST_DESTRUCT(&list);
}

/* ------------------------------------------------------------------ */
/* pmix_mca_base_components_filter                                     */
/* ------------------------------------------------------------------ */

static void filter_case(const char *selection, const char *expect_remaining, int expect_count)
{
    pmix_mca_base_framework_t fw;
    pmix_mca_base_component_list_item_t *cli;
    pmix_mca_base_component_t *a, *b, *c;
    char label[256];
    int rc, count;
    bool found_expected = false;

    memset(&fw, 0, sizeof(fw));
    fw.framework_project = (char *) "pmix";
    fw.framework_name = (char *) "utestsel";
    fw.framework_output = 0;
    fw.framework_selection = (char *) selection;
    PMIX_CONSTRUCT(&fw.framework_components, pmix_list_t);

    a = make_component("alpha", query_low);
    b = make_component("beta", query_mid);
    c = make_component("gamma", query_high);
    add_component(&fw.framework_components, a);
    add_component(&fw.framework_components, b);
    add_component(&fw.framework_components, c);

    rc = pmix_mca_base_components_filter(&fw);
    count = (int) pmix_list_get_size(&fw.framework_components);

    snprintf(label, sizeof(label), "filter \"%s\" keeps %d component(s)",
             NULL == selection ? "(none)" : selection, expect_count);
    report(label, PMIX_SUCCESS == rc && count == expect_count);

    if (NULL != expect_remaining) {
        PMIX_LIST_FOREACH (cli, &fw.framework_components,
                           pmix_mca_base_component_list_item_t) {
            if (0 == strcmp(cli->cli_component->pmix_mca_component_name, expect_remaining)) {
                found_expected = true;
            }
        }
        snprintf(label, sizeof(label), "filter \"%s\" keeps \"%s\"",
                 NULL == selection ? "(none)" : selection, expect_remaining);
        report(label, found_expected);
    }

    PMIX_LIST_DESTRUCT(&fw.framework_components);
    free(a);
    free(b);
    free(c);
}

static void test_filter(void)
{
    /* no selection at all: everything stays */
    filter_case(NULL, "alpha", 3);
    /* an empty string is the same as no selection */
    filter_case("", "alpha", 3);
    /* include one */
    filter_case("beta", "beta", 1);
    /* include two */
    filter_case("alpha,gamma", "gamma", 2);
    /* exclude one */
    filter_case("^beta", "alpha", 2);
    /* exclude two */
    filter_case("^alpha,gamma", "beta", 1);
    /* a name that matches nothing leaves nothing, and is not an error
     * (the warning about it is separate, and off by default) */
    filter_case("nosuchcomponent", NULL, 0);
}

int main(int argc, char **argv)
{
    int rc;

    (void) argc;
    (void) argv;

    setenv("PMIX_MCA_mca_base_param_files", "none", 1);

    rc = pmix_init_util(NULL, 0, NULL);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "pmix_init_util failed: %d\n", rc);
        return 1;
    }

    fprintf(stdout, "\n=== mca_base component selection unit tests ===\n\n");

    test_select_highest_priority();
    test_select_skips();
    test_select_fatal_stops();
    test_select_nothing_available();
    test_select_empty_list();
    test_filter();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    pmix_finalize_util();

    return (nfail > 0) ? 1 : 0;
}
