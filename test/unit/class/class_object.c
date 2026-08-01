/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the pmix_object_t class system:
 *   PMIX_NEW, PMIX_RETAIN, PMIX_RELEASE, PMIX_CONSTRUCT, PMIX_DESTRUCT,
 *   reference counting, and class hierarchy.
 *
 * pmix_object_t is a pure base class and cannot be instantiated directly
 * (its cls_construct_array is pre-set to NULL).  All tests use
 * pmix_list_item_t, the simplest derived class, as a concrete subject.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/class/pmix_list.h"
#include "src/class/pmix_object.h"

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
/* PMIX_NEW / PMIX_RELEASE                                             */
/* ------------------------------------------------------------------ */

static void test_new_not_null(void)
{
    pmix_list_item_t *obj = PMIX_NEW(pmix_list_item_t);
    report("PMIX_NEW returns non-NULL", NULL != obj);
    if (NULL != obj) {
        PMIX_RELEASE(obj);
    }
}

static void test_new_refcount_one(void)
{
    pmix_list_item_t *obj = PMIX_NEW(pmix_list_item_t);
    if (NULL == obj) {
        report("PMIX_NEW refcount: alloc failed", 0);
        return;
    }
    report("PMIX_NEW sets refcount to 1", 1 == ((pmix_object_t *) obj)->obj_reference_count);
    PMIX_RELEASE(obj);
}

static void test_release_sets_null(void)
{
    pmix_list_item_t *obj = PMIX_NEW(pmix_list_item_t);
    if (NULL == obj) {
        report("PMIX_RELEASE sets NULL: alloc failed", 0);
        return;
    }
    PMIX_RELEASE(obj);
    report("PMIX_RELEASE sets pointer to NULL", NULL == obj);
}

/* ------------------------------------------------------------------ */
/* PMIX_RETAIN                                                         */
/* ------------------------------------------------------------------ */

static void test_retain_increments_refcount(void)
{
    pmix_list_item_t *obj = PMIX_NEW(pmix_list_item_t);
    if (NULL == obj) {
        report("PMIX_RETAIN: alloc failed", 0);
        return;
    }
    PMIX_RETAIN(obj);
    report("PMIX_RETAIN increments refcount to 2", 2 == ((pmix_object_t *) obj)->obj_reference_count);
    PMIX_RELEASE(obj);
    report("PMIX_RELEASE after retain: refcount back to 1", 1 == ((pmix_object_t *) obj)->obj_reference_count);
    PMIX_RELEASE(obj);
}

static void test_retain_release_multiple(void)
{
    pmix_list_item_t *obj = PMIX_NEW(pmix_list_item_t);
    if (NULL == obj) {
        report("multi retain/release: alloc failed", 0);
        return;
    }
    PMIX_RETAIN(obj);
    PMIX_RETAIN(obj);
    report("double retain: refcount is 3", 3 == ((pmix_object_t *) obj)->obj_reference_count);
    PMIX_RELEASE(obj);
    PMIX_RELEASE(obj);
    report("double release: refcount is 1", 1 == ((pmix_object_t *) obj)->obj_reference_count);
    PMIX_RELEASE(obj);
}

/* ------------------------------------------------------------------ */
/* PMIX_CONSTRUCT / PMIX_DESTRUCT (stack allocation)                   */
/* ------------------------------------------------------------------ */

static void test_construct_destruct(void)
{
    pmix_list_item_t obj;
    PMIX_CONSTRUCT(&obj, pmix_list_item_t);
    report("PMIX_CONSTRUCT: refcount is 1", 1 == ((pmix_object_t *) &obj)->obj_reference_count);
    report("PMIX_CONSTRUCT: class pointer is non-NULL", NULL != ((pmix_object_t *) &obj)->obj_class);
    PMIX_DESTRUCT(&obj);
}

/* ------------------------------------------------------------------ */
/* pmix_list_t (deeper subclass)                                        */
/* ------------------------------------------------------------------ */

static void test_list_object(void)
{
    pmix_list_t *lst = PMIX_NEW(pmix_list_t);
    report("PMIX_NEW(pmix_list_t): non-NULL", NULL != lst);
    if (NULL != lst) {
        report("PMIX_NEW(pmix_list_t): refcount is 1",
               1 == ((pmix_object_t *) lst)->obj_reference_count);
        PMIX_RELEASE(lst);
    }
}

/* ------------------------------------------------------------------ */
/* Class hierarchy: class name and depth                                */
/* ------------------------------------------------------------------ */

static void test_class_name(void)
{
    pmix_list_item_t *obj = PMIX_NEW(pmix_list_item_t);
    if (NULL == obj) {
        report("class name: alloc failed", 0);
        return;
    }
    report("pmix_list_item_t: class name matches",
           0 == strcmp(((pmix_object_t *) obj)->obj_class->cls_name, "pmix_list_item_t"));
    PMIX_RELEASE(obj);
}

static void test_class_parent(void)
{
    pmix_list_item_t *obj = PMIX_NEW(pmix_list_item_t);
    if (NULL == obj) {
        report("class parent: alloc failed", 0);
        return;
    }
    pmix_class_t *cls = ((pmix_object_t *) obj)->obj_class;
    report("pmix_list_item_t: parent class is non-NULL", NULL != cls->cls_parent);
    report("pmix_list_item_t: parent class name is pmix_object_t",
           NULL != cls->cls_parent &&
           0 == strcmp(cls->cls_parent->cls_name, "pmix_object_t"));
    PMIX_RELEASE(obj);
}

static void test_hierarchy_depth(void)
{
    pmix_list_item_t *item = PMIX_NEW(pmix_list_item_t);
    if (NULL == item) {
        report("hierarchy depth: alloc failed", 0);
        return;
    }
    report("pmix_list_item_t: hierarchy depth >= 2",
           ((pmix_object_t *) item)->obj_class->cls_depth >= 2);
    PMIX_RELEASE(item);
}

/* ------------------------------------------------------------------ */
/* pmix_class_t sizeof field                                            */
/* ------------------------------------------------------------------ */

static void test_class_sizeof(void)
{
    pmix_list_item_t *obj = PMIX_NEW(pmix_list_item_t);
    if (NULL == obj) {
        report("class sizeof: alloc failed", 0);
        return;
    }
    report("cls_sizeof matches sizeof(pmix_list_item_t)",
           ((pmix_object_t *) obj)->obj_class->cls_sizeof == sizeof(pmix_list_item_t));
    PMIX_RELEASE(obj);
}

/* ------------------------------------------------------------------ */
/* Regressions                                                          */
/* ------------------------------------------------------------------ */

/* PMIX_NEW() carried its own copy of the TMA-clearing code and cleared one
 * fewer member than pmix_obj_construct_tma() did, leaving a stray
 * uninitialized function pointer in every heap object in the library.
 * pmix_obj_get_tma() keys off tma_malloc alone, so it never showed up as a
 * crash -- it just meant anything copying or inspecting the whole struct
 * read garbage. The offending member (tma_memmove) has since been deleted
 * as dead weight, but the invariant it broke is the one that matters:
 * PMIX_NEW and PMIX_CONSTRUCT must leave *every* pmix_tma_t member
 * cleared, and both must do it through the one shared helper. */
static void test_new_clears_whole_tma(void)
{
    pmix_list_item_t *obj = PMIX_NEW(pmix_list_item_t);
    pmix_object_t *base = (pmix_object_t *) obj;

    report("PMIX_NEW: allocation succeeded", NULL != obj);
    if (NULL == obj) {
        return;
    }

    report("PMIX_NEW: tma_malloc cleared", NULL == base->obj_tma.tma_malloc);
    report("PMIX_NEW: tma_calloc cleared", NULL == base->obj_tma.tma_calloc);
    report("PMIX_NEW: tma_realloc cleared", NULL == base->obj_tma.tma_realloc);
    report("PMIX_NEW: tma_strdup cleared", NULL == base->obj_tma.tma_strdup);
    report("PMIX_NEW: tma_free cleared", NULL == base->obj_tma.tma_free);
    report("PMIX_NEW: data_context cleared", NULL == base->obj_tma.data_context);
    report("PMIX_NEW: data_ptr cleared", NULL == base->obj_tma.data_ptr);
    report("PMIX_NEW: reports no custom allocator", NULL == pmix_obj_get_tma(base));

    PMIX_RELEASE(obj);
}

/* PMIX_CONSTRUCT() has always gone through pmix_obj_construct_tma(); check
 * it agrees with PMIX_NEW() now that they share the code. */
static void test_construct_clears_whole_tma(void)
{
    pmix_list_item_t item;
    pmix_object_t *base = (pmix_object_t *) &item;

    memset(&item, 0xa5, sizeof(item)); /* poison, so "cleared" means cleared */
    PMIX_CONSTRUCT(&item, pmix_list_item_t);

    report("PMIX_CONSTRUCT: tma_malloc cleared", NULL == base->obj_tma.tma_malloc);
    report("PMIX_CONSTRUCT: tma_free cleared", NULL == base->obj_tma.tma_free);
    report("PMIX_CONSTRUCT: data_ptr cleared", NULL == base->obj_tma.data_ptr);
    report("PMIX_CONSTRUCT: reports no custom allocator", NULL == pmix_obj_get_tma(base));
    report("PMIX_CONSTRUCT: refcount is 1", 1 == base->obj_reference_count);

    PMIX_DESTRUCT(&item);
}

/* A statically initialized object has to describe the same state that
 * PMIX_CONSTRUCT would produce -- PMIX_OBJ_STATIC_INIT no longer carries a
 * PTHREAD_MUTEX_INITIALIZER now that the count is a C11 atomic. */
static pmix_list_item_t static_item = PMIX_LIST_ITEM_STATIC_INIT;

static void test_static_init(void)
{
    pmix_object_t *base = (pmix_object_t *) &static_item;

    report("static init: refcount is 1", 1 == base->obj_reference_count);
    report("static init: class is set", NULL != base->obj_class);
    report("static init: no custom allocator", NULL == pmix_obj_get_tma(base));

    /* retain/release still has to work on it */
    PMIX_RETAIN(&static_item);
    report("static init: retain reaches 2", 2 == base->obj_reference_count);
    pmix_obj_update(base, -1);
    report("static init: back to 1", 1 == base->obj_reference_count);
}

/* pmix_obj_update() returns the *new* count in both directions. That is
 * what PMIX_RELEASE tests against zero to decide whether to run the
 * destructors, so an off-by-one here is a leak or a double free. */
static void test_obj_update_returns_new_value(void)
{
    pmix_list_item_t *obj = PMIX_NEW(pmix_list_item_t);

    report("obj_update: +1 returns 2", 2 == pmix_obj_update((pmix_object_t *) obj, 1));
    report("obj_update: +1 again returns 3", 3 == pmix_obj_update((pmix_object_t *) obj, 1));
    report("obj_update: -1 returns 2", 2 == pmix_obj_update((pmix_object_t *) obj, -1));
    report("obj_update: -2 returns 0", 0 == pmix_obj_update((pmix_object_t *) obj, -2));

    /* count is back to 0; put it back to 1 so PMIX_RELEASE frees it once */
    pmix_obj_update((pmix_object_t *) obj, 1);
    PMIX_RELEASE(obj);
    report("obj_update: object released cleanly", NULL == obj);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_object_t (class system) unit tests ===\n\n");

    test_new_not_null();
    test_new_refcount_one();
    test_release_sets_null();
    test_retain_increments_refcount();
    test_retain_release_multiple();
    test_construct_destruct();
    test_list_object();
    test_class_name();
    test_class_parent();
    test_hierarchy_depth();
    test_class_sizeof();
    test_new_clears_whole_tma();
    test_construct_clears_whole_tma();
    test_static_init();
    test_obj_update_returns_new_value();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
