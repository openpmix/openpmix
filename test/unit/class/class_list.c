/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_list_t and pmix_list_item_t:
 *   append, prepend, remove_first, remove_last, remove_item,
 *   get_size, is_empty, insert, sort, join, PMIX_LIST_FOREACH.
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>

#include "src/class/pmix_list.h"

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

/* A concrete list item carrying an integer payload */
struct int_item_t {
    pmix_list_item_t super;
    int value;
};
typedef struct int_item_t int_item_t;
PMIX_CLASS_DECLARATION(int_item_t);
PMIX_CLASS_INSTANCE(int_item_t, pmix_list_item_t, NULL, NULL);

static int_item_t *make_item(int val)
{
    int_item_t *it = PMIX_NEW(int_item_t);
    if (NULL != it) {
        it->value = val;
    }
    return it;
}

/* ------------------------------------------------------------------ */
/* Empty list                                                           */
/* ------------------------------------------------------------------ */

static void test_empty_list(void)
{
    pmix_list_t list;
    PMIX_CONSTRUCT(&list, pmix_list_t);
    report("empty list: is_empty returns true", pmix_list_is_empty(&list));
    report("empty list: get_size returns 0", 0 == pmix_list_get_size(&list));
    report("empty list: remove_first returns NULL", NULL == pmix_list_remove_first(&list));
    report("empty list: remove_last returns NULL", NULL == pmix_list_remove_last(&list));
    PMIX_DESTRUCT(&list);
}

/* ------------------------------------------------------------------ */
/* append                                                               */
/* ------------------------------------------------------------------ */

static void test_append(void)
{
    pmix_list_t list;
    PMIX_CONSTRUCT(&list, pmix_list_t);

    int_item_t *a = make_item(10);
    int_item_t *b = make_item(20);
    int_item_t *c = make_item(30);

    pmix_list_append(&list, (pmix_list_item_t *) a);
    pmix_list_append(&list, (pmix_list_item_t *) b);
    pmix_list_append(&list, (pmix_list_item_t *) c);

    report("append: size is 3", 3 == pmix_list_get_size(&list));
    report("append: not empty", !pmix_list_is_empty(&list));

    int_item_t *first = (int_item_t *) pmix_list_get_first(&list);
    int_item_t *last  = (int_item_t *) pmix_list_get_last(&list);
    report("append: first element is 10", 10 == first->value);
    report("append: last element is 30", 30 == last->value);

    PMIX_LIST_DESTRUCT(&list);
}

/* ------------------------------------------------------------------ */
/* prepend                                                              */
/* ------------------------------------------------------------------ */

static void test_prepend(void)
{
    pmix_list_t list;
    PMIX_CONSTRUCT(&list, pmix_list_t);

    pmix_list_append(&list, (pmix_list_item_t *) make_item(1));
    pmix_list_prepend(&list, (pmix_list_item_t *) make_item(2));
    pmix_list_prepend(&list, (pmix_list_item_t *) make_item(3));

    report("prepend: size is 3", 3 == pmix_list_get_size(&list));

    int_item_t *first = (int_item_t *) pmix_list_get_first(&list);
    int_item_t *last  = (int_item_t *) pmix_list_get_last(&list);
    report("prepend: first element is 3", 3 == first->value);
    report("prepend: last element is 1", 1 == last->value);

    PMIX_LIST_DESTRUCT(&list);
}

/* ------------------------------------------------------------------ */
/* remove_first / remove_last                                           */
/* ------------------------------------------------------------------ */

static void test_remove_first(void)
{
    pmix_list_t list;
    int_item_t *elem;
    PMIX_CONSTRUCT(&list, pmix_list_t);

    pmix_list_append(&list, (pmix_list_item_t *) make_item(10));
    pmix_list_append(&list, (pmix_list_item_t *) make_item(20));
    pmix_list_append(&list, (pmix_list_item_t *) make_item(30));

    elem = (int_item_t *) pmix_list_remove_first(&list);
    report("remove_first: value is 10", NULL != elem && 10 == elem->value);
    report("remove_first: size is now 2", 2 == pmix_list_get_size(&list));
    PMIX_RELEASE(elem);

    elem = (int_item_t *) pmix_list_remove_first(&list);
    report("remove_first again: value is 20", NULL != elem && 20 == elem->value);
    PMIX_RELEASE(elem);

    PMIX_LIST_DESTRUCT(&list);
}

static void test_remove_last(void)
{
    pmix_list_t list;
    int_item_t *elem;
    PMIX_CONSTRUCT(&list, pmix_list_t);

    pmix_list_append(&list, (pmix_list_item_t *) make_item(10));
    pmix_list_append(&list, (pmix_list_item_t *) make_item(20));
    pmix_list_append(&list, (pmix_list_item_t *) make_item(30));

    elem = (int_item_t *) pmix_list_remove_last(&list);
    report("remove_last: value is 30", NULL != elem && 30 == elem->value);
    report("remove_last: size is now 2", 2 == pmix_list_get_size(&list));
    PMIX_RELEASE(elem);

    PMIX_LIST_DESTRUCT(&list);
}

/* ------------------------------------------------------------------ */
/* remove_item                                                          */
/* ------------------------------------------------------------------ */

static void test_remove_item(void)
{
    pmix_list_t list;
    PMIX_CONSTRUCT(&list, pmix_list_t);

    int_item_t *a = make_item(1);
    int_item_t *b = make_item(2);
    int_item_t *c = make_item(3);

    pmix_list_append(&list, (pmix_list_item_t *) a);
    pmix_list_append(&list, (pmix_list_item_t *) b);
    pmix_list_append(&list, (pmix_list_item_t *) c);

    pmix_list_remove_item(&list, (pmix_list_item_t *) b);
    report("remove_item: size is 2", 2 == pmix_list_get_size(&list));

    int_item_t *first = (int_item_t *) pmix_list_get_first(&list);
    int_item_t *last  = (int_item_t *) pmix_list_get_last(&list);
    report("remove_item: first is 1", 1 == first->value);
    report("remove_item: last is 3", 3 == last->value);

    PMIX_RELEASE(b);
    PMIX_LIST_DESTRUCT(&list);
}

/* ------------------------------------------------------------------ */
/* PMIX_LIST_FOREACH                                                    */
/* ------------------------------------------------------------------ */

static void test_foreach(void)
{
    pmix_list_t list;
    int_item_t *cur;
    int sum = 0;
    PMIX_CONSTRUCT(&list, pmix_list_t);

    pmix_list_append(&list, (pmix_list_item_t *) make_item(5));
    pmix_list_append(&list, (pmix_list_item_t *) make_item(10));
    pmix_list_append(&list, (pmix_list_item_t *) make_item(15));

    PMIX_LIST_FOREACH(cur, &list, int_item_t) {
        sum += cur->value;
    }
    report("PMIX_LIST_FOREACH: sum of 5+10+15 = 30", 30 == sum);

    PMIX_LIST_DESTRUCT(&list);
}

/* ------------------------------------------------------------------ */
/* insert                                                               */
/* ------------------------------------------------------------------ */

static void test_insert(void)
{
    pmix_list_t list;
    int_item_t *cur;
    int vals[3];
    int n = 0;
    PMIX_CONSTRUCT(&list, pmix_list_t);

    pmix_list_append(&list, (pmix_list_item_t *) make_item(1));
    pmix_list_append(&list, (pmix_list_item_t *) make_item(3));

    bool ok = pmix_list_insert(&list, (pmix_list_item_t *) make_item(2), 1);
    report("insert at index 1: returns true", ok);
    report("insert at index 1: size is 3", 3 == pmix_list_get_size(&list));

    PMIX_LIST_FOREACH(cur, &list, int_item_t) {
        if (n < 3) {
            vals[n++] = cur->value;
        }
    }
    report("insert: order is 1, 2, 3",
           3 == n && 1 == vals[0] && 2 == vals[1] && 3 == vals[2]);

    PMIX_LIST_DESTRUCT(&list);
}

static void test_insert_out_of_bounds(void)
{
    pmix_list_t list;
    PMIX_CONSTRUCT(&list, pmix_list_t);

    pmix_list_append(&list, (pmix_list_item_t *) make_item(1));

    int_item_t *extra = make_item(99);
    bool ok = pmix_list_insert(&list, (pmix_list_item_t *) extra, 5);
    report("insert out of bounds: returns false", !ok);
    report("insert out of bounds: size unchanged", 1 == pmix_list_get_size(&list));
    PMIX_RELEASE(extra);

    PMIX_LIST_DESTRUCT(&list);
}

/* ------------------------------------------------------------------ */
/* sort                                                                 */
/* ------------------------------------------------------------------ */

static int cmp_int_items(pmix_list_item_t **a, pmix_list_item_t **b)
{
    int va = ((int_item_t *) *a)->value;
    int vb = ((int_item_t *) *b)->value;
    return (va > vb) - (va < vb);
}

static void test_sort(void)
{
    pmix_list_t list;
    int_item_t *cur;
    int vals[3];
    int n = 0;
    PMIX_CONSTRUCT(&list, pmix_list_t);

    pmix_list_append(&list, (pmix_list_item_t *) make_item(30));
    pmix_list_append(&list, (pmix_list_item_t *) make_item(10));
    pmix_list_append(&list, (pmix_list_item_t *) make_item(20));

    pmix_list_sort(&list, cmp_int_items);

    PMIX_LIST_FOREACH(cur, &list, int_item_t) {
        if (n < 3) {
            vals[n++] = cur->value;
        }
    }
    report("sort: order is 10, 20, 30",
           3 == n && 10 == vals[0] && 20 == vals[1] && 30 == vals[2]);

    PMIX_LIST_DESTRUCT(&list);
}

/* ------------------------------------------------------------------ */
/* join                                                                 */
/* ------------------------------------------------------------------ */

static void test_join(void)
{
    pmix_list_t dst;
    pmix_list_t src;
    PMIX_CONSTRUCT(&dst, pmix_list_t);
    PMIX_CONSTRUCT(&src, pmix_list_t);

    pmix_list_append(&dst, (pmix_list_item_t *) make_item(1));
    pmix_list_append(&dst, (pmix_list_item_t *) make_item(2));
    pmix_list_append(&src, (pmix_list_item_t *) make_item(3));
    pmix_list_append(&src, (pmix_list_item_t *) make_item(4));

    pmix_list_join(&dst, pmix_list_get_end(&dst), &src);

    report("join: dst size is 4", 4 == pmix_list_get_size(&dst));
    report("join: src is empty", pmix_list_is_empty(&src));

    int_item_t *last = (int_item_t *) pmix_list_get_last(&dst);
    report("join: last element of dst is 4", 4 == last->value);

    PMIX_LIST_DESTRUCT(&dst);
    PMIX_DESTRUCT(&src);
}

/* ------------------------------------------------------------------ */
/* PMIX_LIST_RELEASE                                                    */
/* ------------------------------------------------------------------ */

static void test_list_release(void)
{
    pmix_list_t *list = PMIX_NEW(pmix_list_t);
    report("PMIX_LIST_RELEASE: alloc succeeds", NULL != list);
    if (NULL == list) {
        return;
    }

    pmix_list_append(list, (pmix_list_item_t *) make_item(1));
    pmix_list_append(list, (pmix_list_item_t *) make_item(2));

    PMIX_LIST_RELEASE(list);
    report("PMIX_LIST_RELEASE: list pointer is NULL", NULL == list);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_list_t unit tests ===\n\n");

    test_empty_list();
    test_append();
    test_prepend();
    test_remove_first();
    test_remove_last();
    test_remove_item();
    test_foreach();
    test_insert();
    test_insert_out_of_bounds();
    test_sort();
    test_join();
    test_list_release();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
