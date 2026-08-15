/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the attribute dictionary and the key index it seeds.
 *
 * An attribute id is part of the compatibility surface: gds/shmem3
 * stores keys by id in memory it shares with peer processes, so two
 * builds that disagree about which attribute owns an id resolve to the
 * wrong value rather than to an error. Ids are therefore pinned in
 * contrib/dictionary_ids.txt rather than derived from the order the
 * generator happens to scan the headers in.
 *
 * These tests pin the properties that arrangement has to keep. They
 * check the *shipped* artifact rather than the map, because the map is
 * an input the generator has already validated - what matters here is
 * what ends up compiled into the library, and that the key index the
 * whole library translates through actually agrees with it.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_dictionary.h"
#include "src/include/pmix_globals.h"
#include "src/util/pmix_hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
/* the dictionary itself                                               */
/* ------------------------------------------------------------------ */
static void test_dictionary(void)
{
    size_t n, m;
    int dup = 0, oob = 0, empty = 0;

    fprintf(stdout, "\n-- dictionary --\n");

    /* The count and the boundary are different quantities. Entries can
     * share an id (see below), so there are never more distinct ids
     * than entries - and retiring an attribute pushes the boundary the
     * other way. Neither relation is an identity; only this bound holds. */
    report("no more distinct ids than entries",
           PMIX_INDEX_BOUNDARY <= PMIX_DICTIONARY_SIZE);

    for (n = 0; n < PMIX_DICTIONARY_SIZE; n++) {
        if (NULL == pmix_dictionary[n].name || NULL == pmix_dictionary[n].string) {
            empty++;
        }
        /* PMIX_INDEX_BOUNDARY is one past the highest id ever handed
         * out, so every live entry has to sit below it - that is what
         * lets it seed run-time key numbering without colliding. */
        if ((uint32_t) PMIX_INDEX_BOUNDARY <= pmix_dictionary[n].index) {
            oob++;
        }
    }
    report("every entry has a name and a string", 0 == empty);
    report("every id is below the boundary", 0 == oob);

    /* Entries share an id exactly when they share a string, and never
     * otherwise. Sharing is the deliberate case - an attribute and its
     * deprecated alias are one attribute to the datastore, which keys on
     * the string. Two *different* strings on one id would be the failure
     * this whole scheme exists to prevent. O(n^2) over ~700 entries is
     * nothing, so check it outright both ways. */
    for (n = 0; n < PMIX_DICTIONARY_SIZE; n++) {
        for (m = n + 1; m < PMIX_DICTIONARY_SIZE; m++) {
            int same_id = (pmix_dictionary[n].index == pmix_dictionary[m].index);
            int same_str = (0 == strcmp(pmix_dictionary[n].string,
                                        pmix_dictionary[m].string));
            if (same_id == same_str) {
                continue;
            }
            if (10 > dup) {
                fprintf(stdout, "    %s (%s, id %u) vs %s (%s, id %u)\n",
                        pmix_dictionary[n].name, pmix_dictionary[n].string,
                        pmix_dictionary[n].index, pmix_dictionary[m].name,
                        pmix_dictionary[m].string, pmix_dictionary[m].index);
            }
            dup++;
        }
    }
    report("entries share an id exactly when they share a string", 0 == dup);
}

/* ------------------------------------------------------------------ */
/* the key index the dictionary seeds                                  */
/* ------------------------------------------------------------------ */
static void test_keyindex(void)
{
    size_t n;
    int missing = 0, wrong = 0;
    pmix_regattr_input_t *p;

    fprintf(stdout, "\n-- key index --\n");

    /* This is the load-bearing one. Every reserved attribute has to
     * resolve, by its string form, to exactly the id the dictionary
     * gives it - in every process, without anything being exchanged.
     * That is what makes a reserved id mean the same thing on both
     * ends of a shared-memory segment. */
    for (n = 0; n < PMIX_DICTIONARY_SIZE; n++) {
        p = pmix_hash_find_key(UINT32_MAX, pmix_dictionary[n].string, NULL);
        if (NULL == p) {
            if (0 == missing) {
                fprintf(stdout, "    first unresolved: %s (%s)\n",
                        pmix_dictionary[n].name, pmix_dictionary[n].string);
            }
            missing++;
        } else if (p->index != pmix_dictionary[n].index) {
            if (0 == wrong) {
                fprintf(stdout, "    first mismatch: %s wanted %u got %u\n",
                        pmix_dictionary[n].name, pmix_dictionary[n].index,
                        p->index);
            }
            wrong++;
        }
    }
    report("every reserved attribute resolves by string", 0 == missing);
    report("and resolves to its dictionary id", 0 == wrong);

    /* Run-time keys are numbered from the boundary upward. If next_id
     * ever started below it, a user key would be handed an id that a
     * reserved attribute already owns. */
    report("run-time key numbering starts above every reserved id",
           pmix_globals.keyindex.next_id >= (uint32_t) PMIX_INDEX_BOUNDARY);

    /* A key nobody has registered must not resolve. pmix_hash_find_key()
     * is the non-registering lookup: it is what every read-only path
     * uses, including a client reading a segment it may not write. */
    p = pmix_hash_find_key(UINT32_MAX, "pmix.no.such.attribute.exists", NULL);
    report("an unknown key does not resolve", NULL == p);
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    static pmix_server_module_t mymodule = {0};
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    /* the key index is seeded from the dictionary during init */
    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== attribute dictionary unit tests ===\n");
    fprintf(stdout, "    %d entries, id boundary %d\n",
            (int) PMIX_DICTIONARY_SIZE, (int) PMIX_INDEX_BOUNDARY);

    test_dictionary();
    test_keyindex();

    fprintf(stdout, "\n=== %d passed, %d failed ===\n\n", npass, nfail);

    PMIx_server_finalize();
    return (0 == nfail) ? 0 : 1;
}
