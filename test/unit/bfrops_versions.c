/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * What each modern bfrops component registers.
 *
 * A component is named for the release whose wire format it speaks, and
 * two peers that name the same component must agree on every type it
 * carries. So once a release has shipped a component, that component's
 * type set is frozen; a release that adds types adds a component. These
 * cases pin the sets down:
 *
 * - v61 is what v6.1.0 shipped: v51's set plus PMIX_NODE_PID, and
 *   nothing more;
 * - v70 is v61's set plus the two types added since, PMIX_REGEX2 and
 *   PMIX_ALLOC_INHERIT;
 * - v70 is the format this build speaks for itself, and the first one
 *   it offers a client.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/bfrops/base/base.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pmix_server_module_t mymodule = {
    .client_connected = NULL
};

static int npass = 0;
static int nfail = 0;

static void report(const char *name, bool passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        npass++;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        nfail++;
    }
}

static bool registers(pmix_bfrops_module_t *mod, pmix_data_type_t type)
{
    return NULL != mod->data_type_string(type);
}

/* every type registered by "big" is registered by "small", apart from
 * the ones listed as added - and every added one is registered by "big"
 * and not by "small" */
static void test_superset(const char *small, const char *big,
                          const pmix_data_type_t *added, size_t nadded)
{
    pmix_bfrops_module_t *s, *b;
    pmix_data_type_t t;
    size_t n;
    bool ok = true, isadded;
    char label[128];

    s = pmix_bfrops_base_assign_module(small);
    b = pmix_bfrops_base_assign_module(big);
    snprintf(label, sizeof(label), "%s and %s are both available", small, big);
    report(label, NULL != s && NULL != b);
    if (NULL == s || NULL == b) {
        return;
    }
    for (t = 0; t < PMIX_DATA_TYPE_MAX; t++) {
        isadded = false;
        for (n = 0; n < nadded; n++) {
            if (added[n] == t) {
                isadded = true;
                break;
            }
        }
        if (isadded) {
            if (!registers(b, t) || registers(s, t)) {
                fprintf(stdout, "    type %u (%s): %s %s it, %s %s it\n", (unsigned) t,
                        PMIx_Data_type_string(t), big, registers(b, t) ? "has" : "lacks",
                        small, registers(s, t) ? "has" : "lacks");
                ok = false;
            }
        } else if (registers(b, t) != registers(s, t)) {
            fprintf(stdout, "    type %u (%s): only %s has it\n", (unsigned) t,
                    PMIx_Data_type_string(t), registers(b, t) ? big : small);
            ok = false;
        }
    }
    snprintf(label, sizeof(label), "%s registers exactly %s's types plus %lu more", big, small,
             (unsigned long) nadded);
    report(label, ok);
}

/* a format refuses to pack a type it does not carry */
static void test_v61_refuses_newer_types(void)
{
    pmix_bfrops_module_t *mod;
    pmix_buffer_t buf;
    pmix_alloc_inheritance_t inh = PMIX_ALLOC_INHERIT_DEFAULT;
    pmix_regex2_t rx = PMIX_REGEX2_STATIC_INIT;
    char rxtype[] = "test";
    uint8_t rxbytes[] = {'a', 'b', 'c'};
    pmix_status_t rc;

    mod = pmix_bfrops_base_assign_module("v61");
    if (NULL == mod) {
        report("v61 is available", false);
        return;
    }
    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    buf.type = PMIX_BFROP_BUFFER_NON_DESC;
    rc = mod->pack(&buf, &inh, 1, PMIX_ALLOC_INHERIT);
    report("v61 will not pack a PMIX_ALLOC_INHERIT", PMIX_ERR_UNKNOWN_DATA_TYPE == rc);
    PMIX_DESTRUCT(&buf);

    PMIX_CONSTRUCT(&buf, pmix_buffer_t);
    buf.type = PMIX_BFROP_BUFFER_NON_DESC;
    rx.type = rxtype;
    rx.bytes = rxbytes;
    rx.len = sizeof(rxbytes);
    rc = mod->pack(&buf, &rx, 1, PMIX_REGEX2);
    report("v61 will not pack a PMIX_REGEX2", PMIX_ERR_UNKNOWN_DATA_TYPE == rc);
    PMIX_DESTRUCT(&buf);
}

static void test_native_is_v70(void)
{
    pmix_bfrops_module_t *mod;
    char *list, *v70, *v61;

    mod = pmix_bfrops_base_assign_module(NULL);
    report("this build speaks v70 for itself",
           NULL != mod && 0 == strcmp(mod->version, "v70"));

    /* a server offers its formats in priority order, and a client takes
     * the first it has - so v70 must be offered ahead of v61 */
    list = pmix_bfrops_base_get_components();
    v70 = (NULL == list) ? NULL : strstr(list, "PMIX_SERVER_URI70");
    v61 = (NULL == list) ? NULL : strstr(list, "PMIX_SERVER_URI61");
    if (NULL != list) {
        fprintf(stdout, "    offered: %s\n", list);
    }
    report("a server offers v70 ahead of v61", NULL != v70 && NULL != v61 && v70 < v61);
    free(list);
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    const pmix_data_type_t v61_added[] = {PMIX_NODE_PID};
    const pmix_data_type_t v70_added[] = {PMIX_REGEX2, PMIX_ALLOC_INHERIT};

    (void) argc;
    (void) argv;

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== bfrops component type sets ===\n\n");

    test_superset("v51", "v61", v61_added, sizeof(v61_added) / sizeof(v61_added[0]));
    test_superset("v61", "v70", v70_added, sizeof(v70_added) / sizeof(v70_added[0]));
    test_v61_refuses_newer_types();
    test_native_is_v70();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
