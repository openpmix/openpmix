/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the mca_base_component_show_load_errors parser in
 * src/mca/base/pmix_mca_base_components_open.c.
 *
 * The parameter accepts "all", "none", any boolean spelling, or a
 * comma-delimited list of "framework" / "framework/component" items
 * that may be prefixed with "^" to negate the sense of the list. Each
 * form is driven here through pmix_mca_base_show_load_errors_init() and
 * then queried with pmix_mca_base_show_load_errors().
 *
 * NOTE: a list entry naming a bare framework leaves the entry's
 * component name NULL. Asking about a specific component of that
 * framework used to compare against that NULL, which is why the
 * bare-framework cases below matter more than they look.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pmix_common.h"
#include "src/mca/base/pmix_base.h"

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

/* Drive the parser over a value. The parser reads the global directly,
 * so point it at our string for the duration. */
static int setup(const char *value)
{
    pmix_mca_base_component_show_load_errors = (char *) value;
    return pmix_mca_base_show_load_errors_init();
}

static void teardown(void)
{
    pmix_mca_base_show_load_errors_finalize();
    pmix_mca_base_component_show_load_errors = NULL;
}

static void expect(const char *label, const char *framework, const char *component, bool want)
{
    char name[256];
    bool got = pmix_mca_base_show_load_errors(framework, component);

    snprintf(name, sizeof(name), "%s: (%s, %s) -> %s", label,
             NULL == framework ? "NULL" : framework, NULL == component ? "NULL" : component,
             want ? "true" : "false");
    report(name, got == want);
}

static void test_all(void)
{
    report("\"all\" parses", PMIX_SUCCESS == setup("all"));
    expect("all", "ptl", "tcp", true);
    expect("all", "gds", NULL, true);
    expect("all", NULL, NULL, true);
    teardown();
}

static void test_none(void)
{
    report("\"none\" parses", PMIX_SUCCESS == setup("none"));
    expect("none", "ptl", "tcp", false);
    expect("none", "gds", NULL, false);
    teardown();
}

static void test_boolean_synonyms(void)
{
    report("\"true\" parses", PMIX_SUCCESS == setup("true"));
    expect("true", "ptl", "tcp", true);
    teardown();

    report("\"false\" parses", PMIX_SUCCESS == setup("false"));
    expect("false", "ptl", "tcp", false);
    teardown();

    report("\"1\" parses", PMIX_SUCCESS == setup("1"));
    expect("1", "ptl", "tcp", true);
    teardown();

    report("\"0\" parses", PMIX_SUCCESS == setup("0"));
    expect("0", "ptl", "tcp", false);
    teardown();
}

/* An include list naming only frameworks. Every component of a listed
 * framework is included; anything else is not. */
static void test_include_bare_framework(void)
{
    report("\"ptl,gds\" parses", PMIX_SUCCESS == setup("ptl,gds"));
    expect("include-fw", "ptl", "tcp", true);
    expect("include-fw", "ptl", "server", true);
    expect("include-fw", "ptl", NULL, true);
    expect("include-fw", "gds", "hash", true);
    expect("include-fw", "psec", "native", false);
    expect("include-fw", "psec", NULL, false);
    teardown();
}

static void test_include_framework_component(void)
{
    report("\"ptl/tcp\" parses", PMIX_SUCCESS == setup("ptl/tcp"));
    expect("include-fw/comp", "ptl", "tcp", true);
    expect("include-fw/comp", "ptl", "server", false);
    /* no component named: the caller is asking about the framework as a
     * whole, which the list does mention */
    expect("include-fw/comp", "ptl", NULL, true);
    expect("include-fw/comp", "gds", "hash", false);
    teardown();
}

static void test_include_mixed(void)
{
    report("\"ptl/tcp,gds\" parses", PMIX_SUCCESS == setup("ptl/tcp,gds"));
    expect("include-mixed", "ptl", "tcp", true);
    expect("include-mixed", "ptl", "server", false);
    expect("include-mixed", "gds", "hash", true);
    expect("include-mixed", "gds", "shmem3", true);
    expect("include-mixed", "psec", "native", false);
    teardown();
}

/* The "^" form inverts the sense of the list. */
static void test_exclude_bare_framework(void)
{
    report("\"^ptl\" parses", PMIX_SUCCESS == setup("^ptl"));
    expect("exclude-fw", "ptl", "tcp", false);
    expect("exclude-fw", "ptl", NULL, false);
    expect("exclude-fw", "gds", "hash", true);
    teardown();
}

static void test_exclude_framework_component(void)
{
    report("\"^ptl/tcp\" parses", PMIX_SUCCESS == setup("^ptl/tcp"));
    expect("exclude-fw/comp", "ptl", "tcp", false);
    expect("exclude-fw/comp", "ptl", "server", true);
    expect("exclude-fw/comp", "gds", "hash", true);
    teardown();
}

/* Empty entries from consecutive commas are skipped, not treated as a
 * framework named "". */
static void test_empty_entries(void)
{
    report("\"ptl,,gds,\" parses", PMIX_SUCCESS == setup("ptl,,gds,"));
    expect("empty-entries", "ptl", "tcp", true);
    expect("empty-entries", "gds", "hash", true);
    expect("empty-entries", "psec", "native", false);
    teardown();
}

/* More than one "/" in an entry is a user error. */
static void test_too_many_slashes(void)
{
    report("\"ptl/tcp/extra\" rejected", PMIX_SUCCESS != setup("ptl/tcp/extra"));
    teardown();
}

/* A NULL value must not be dereferenced; treat it as the default. */
static void test_null_value(void)
{
    report("NULL value parses", PMIX_SUCCESS == setup(NULL));
    expect("null", "ptl", "tcp", true);
    teardown();
}

int main(int argc, char **argv)
{
    (void) argc;
    (void) argv;

    fprintf(stdout, "\n=== mca_base show_load_errors unit tests ===\n\n");

    test_all();
    test_none();
    test_boolean_synonyms();
    test_include_bare_framework();
    test_include_framework_component();
    test_include_mixed();
    test_exclude_bare_framework();
    test_exclude_framework_component();
    test_empty_entries();
    test_too_many_slashes();
    test_null_value();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
