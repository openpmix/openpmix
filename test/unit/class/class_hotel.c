/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for pmix_hotel_t:
 *   init, checkin, checkout, checkout_and_return_occupant,
 *   knock, is_empty, full-hotel behavior.
 *
 * No event base is used; eviction timers are disabled (evbase=NULL).
 *
 * Exit 0 if all tests pass, 1 otherwise.
 */

#include "src/include/pmix_config.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>

#include "src/class/pmix_hotel.h"

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

/* Dummy eviction callback -- never fired without an event base */
static void dummy_evict(pmix_hotel_t *hotel, int room_num, void *occupant)
{
    (void) hotel;
    (void) room_num;
    (void) occupant;
}

/* ------------------------------------------------------------------ */
/* init                                                                 */
/* ------------------------------------------------------------------ */

static void test_init(void)
{
    pmix_hotel_t hotel;
    PMIX_CONSTRUCT(&hotel, pmix_hotel_t);
    pmix_status_t rc = pmix_hotel_init(&hotel, 5, NULL, 0, dummy_evict);
    report("init: returns PMIX_SUCCESS", PMIX_SUCCESS == rc);
    report("init: num_rooms is 5", 5 == hotel.num_rooms);
    report("init: hotel is empty", pmix_hotel_is_empty(&hotel));
    PMIX_DESTRUCT(&hotel);
}

static void test_init_bad_params(void)
{
    pmix_hotel_t hotel;
    PMIX_CONSTRUCT(&hotel, pmix_hotel_t);

    pmix_status_t rc = pmix_hotel_init(&hotel, 0, NULL, 0, dummy_evict);
    report("init zero rooms: ERR_BAD_PARAM", PMIX_ERR_BAD_PARAM == rc);

    rc = pmix_hotel_init(&hotel, 5, NULL, 0, NULL);
    report("init NULL callback: ERR_BAD_PARAM", PMIX_ERR_BAD_PARAM == rc);

    PMIX_DESTRUCT(&hotel);
}

/* ------------------------------------------------------------------ */
/* checkin / checkout                                                   */
/* ------------------------------------------------------------------ */

static void test_checkin_checkout(void)
{
    pmix_hotel_t hotel;
    int room;
    PMIX_CONSTRUCT(&hotel, pmix_hotel_t);
    pmix_hotel_init(&hotel, 4, NULL, 0, dummy_evict);

    char *guest = "alice";
    pmix_status_t rc = pmix_hotel_checkin(&hotel, guest, &room);
    report("checkin: returns PMIX_SUCCESS",       PMIX_SUCCESS == rc);
    report("checkin: room number valid [0,4)",     room >= 0 && room < 4);
    report("checkin: hotel not empty",            !pmix_hotel_is_empty(&hotel));

    pmix_hotel_checkout(&hotel, room);
    report("checkout: hotel is empty again",       pmix_hotel_is_empty(&hotel));

    PMIX_DESTRUCT(&hotel);
}

/* ------------------------------------------------------------------ */
/* checkout_and_return_occupant                                         */
/* ------------------------------------------------------------------ */

static void test_checkout_and_return(void)
{
    pmix_hotel_t hotel;
    int room;
    void *returned;
    PMIX_CONSTRUCT(&hotel, pmix_hotel_t);
    pmix_hotel_init(&hotel, 4, NULL, 0, dummy_evict);

    char *guest = "bob";
    pmix_hotel_checkin(&hotel, guest, &room);

    pmix_hotel_checkout_and_return_occupant(&hotel, room, &returned);
    report("checkout_and_return: occupant pointer matches",  returned == (void *) guest);
    report("checkout_and_return: hotel is empty after",      pmix_hotel_is_empty(&hotel));

    PMIX_DESTRUCT(&hotel);
}

/* ------------------------------------------------------------------ */
/* knock                                                                */
/* ------------------------------------------------------------------ */

static void test_knock(void)
{
    pmix_hotel_t hotel;
    int room;
    void *occupant;
    PMIX_CONSTRUCT(&hotel, pmix_hotel_t);
    pmix_hotel_init(&hotel, 4, NULL, 0, dummy_evict);

    char *guest = "carol";
    pmix_hotel_checkin(&hotel, guest, &room);

    pmix_hotel_knock(&hotel, room, &occupant);
    report("knock: returns correct occupant",   occupant == (void *) guest);
    report("knock: guest still in hotel",       !pmix_hotel_is_empty(&hotel));

    pmix_hotel_checkout(&hotel, room);
    PMIX_DESTRUCT(&hotel);
}

/* ------------------------------------------------------------------ */
/* hotel full                                                           */
/* ------------------------------------------------------------------ */

static void test_hotel_full(void)
{
    pmix_hotel_t hotel;
    PMIX_CONSTRUCT(&hotel, pmix_hotel_t);
    pmix_hotel_init(&hotel, 2, NULL, 0, dummy_evict);

    int room1, room2, room3;
    char *g1 = "d1", *g2 = "d2", *g3 = "d3";

    pmix_status_t rc = pmix_hotel_checkin(&hotel, g1, &room1);
    report("full test: checkin 1 succeeds", PMIX_SUCCESS == rc);
    rc = pmix_hotel_checkin(&hotel, g2, &room2);
    report("full test: checkin 2 succeeds", PMIX_SUCCESS == rc);

    rc = pmix_hotel_checkin(&hotel, g3, &room3);
    report("full test: checkin 3 fails (hotel full)", PMIX_ERR_OUT_OF_RESOURCE == rc);
    report("full test: returned room is -1", -1 == room3);

    pmix_hotel_checkout(&hotel, room1);
    pmix_hotel_checkout(&hotel, room2);
    PMIX_DESTRUCT(&hotel);
}

/* ------------------------------------------------------------------ */
/* multiple guests                                                      */
/* ------------------------------------------------------------------ */

static void test_multiple_guests(void)
{
    pmix_hotel_t hotel;
    PMIX_CONSTRUCT(&hotel, pmix_hotel_t);
    pmix_hotel_init(&hotel, 10, NULL, 0, dummy_evict);

    int rooms[5];
    char *guests[5] = {"g0", "g1", "g2", "g3", "g4"};
    bool all_checked_in = true;

    for (int i = 0; i < 5; i++) {
        pmix_status_t rc = pmix_hotel_checkin(&hotel, guests[i], &rooms[i]);
        if (PMIX_SUCCESS != rc) {
            all_checked_in = false;
        }
    }
    report("multi_guests: all 5 checkins succeeded", all_checked_in);

    bool all_correct = true;
    for (int i = 0; i < 5; i++) {
        void *occ;
        pmix_hotel_knock(&hotel, rooms[i], &occ);
        if (occ != (void *) guests[i]) {
            all_correct = false;
        }
    }
    report("multi_guests: knock returns correct occupant for each room", all_correct);

    for (int i = 0; i < 5; i++) {
        pmix_hotel_checkout(&hotel, rooms[i]);
    }
    report("multi_guests: all checked out, hotel empty", pmix_hotel_is_empty(&hotel));

    PMIX_DESTRUCT(&hotel);
}

/* ------------------------------------------------------------------ */
/* re-use a room after checkout                                         */
/* ------------------------------------------------------------------ */

static void test_room_reuse(void)
{
    pmix_hotel_t hotel;
    int room1, room2;
    PMIX_CONSTRUCT(&hotel, pmix_hotel_t);
    pmix_hotel_init(&hotel, 1, NULL, 0, dummy_evict);

    char *g1 = "first";
    char *g2 = "second";

    pmix_status_t rc = pmix_hotel_checkin(&hotel, g1, &room1);
    report("room_reuse: first checkin succeeds", PMIX_SUCCESS == rc);

    pmix_hotel_checkout(&hotel, room1);

    rc = pmix_hotel_checkin(&hotel, g2, &room2);
    report("room_reuse: second checkin succeeds after checkout", PMIX_SUCCESS == rc);

    void *occ;
    pmix_hotel_knock(&hotel, room2, &occ);
    report("room_reuse: occupant is 'second'", occ == (void *) g2);

    pmix_hotel_checkout(&hotel, room2);
    PMIX_DESTRUCT(&hotel);
}

/* ------------------------------------------------------------------ */
/* Regressions                                                          */
/* ------------------------------------------------------------------ */

/* PMIX_HOTEL_STATIC_INIT set last_unoccupied_room to 0 while the
 * constructor sets it to -1. -1 is the "no rooms" sentinel checkin() tests,
 * so a statically initialized hotel claimed room 0 was free and then
 * indexed a NULL unoccupied_rooms array. pmix_globals.notifications is
 * built exactly this way, which is why this has to hold before
 * pmix_hotel_init() has ever been called. */
static pmix_hotel_t static_hotel = PMIX_HOTEL_STATIC_INIT;

static void test_static_init(void)
{
    int room = 12345;
    void *occupant = (void *) 0x1;

    report("static init: reports empty", pmix_hotel_is_empty(&static_hotel));
    report("static init: checkin declines rather than faulting",
           PMIX_ERR_OUT_OF_RESOURCE == pmix_hotel_checkin(&static_hotel, occupant, &room));
    report("static init: declined checkin returns room -1", -1 == room);

    /* checkout of a room nobody holds must also be a no-op */
    pmix_hotel_checkout(&static_hotel, -1);
    report("static init: checkout of -1 is a no-op", true);

    occupant = (void *) 0xdeadbeef;
    pmix_hotel_knock(&static_hotel, -1, &occupant);
    report("static init: knock on -1 yields NULL", NULL == occupant);
}

/* The destructor freed rooms/eviction_args/unoccupied_rooms and left all
 * three pointers in place. It now returns the hotel to exactly the state
 * the constructor produces, so nothing is left addressing freed memory.
 *
 * Note what is NOT exercised here: calling PMIX_DESTRUCT twice. That is a
 * contract violation of the object system, and --enable-debug builds abort
 * on the magic-ID assert when you try - correctly. What matters is that
 * the class leaves no dangling state behind for anything that does reach
 * it, which is what these checks describe. */
static void test_destruct_leaves_constructed_state(void)
{
    pmix_hotel_t h;
    int room = -1;

    PMIX_CONSTRUCT(&h, pmix_hotel_t);
    report("destruct: init succeeds",
           PMIX_SUCCESS == pmix_hotel_init(&h, 4, NULL, 0, dummy_evict));
    pmix_hotel_checkin(&h, (void *) 0x1, &room);

    PMIX_DESTRUCT(&h);
    report("destruct: rooms pointer cleared", NULL == h.rooms);
    report("destruct: eviction_args pointer cleared", NULL == h.eviction_args);
    report("destruct: unoccupied_rooms pointer cleared", NULL == h.unoccupied_rooms);
    report("destruct: num_rooms reset to 0", 0 == h.num_rooms);
    report("destruct: last_unoccupied_room back to the -1 sentinel",
           -1 == h.last_unoccupied_room);

    /* checkin against the torn-down hotel must decline, not fault */
    room = 12345;
    report("after destruct: checkin declines",
           PMIX_ERR_OUT_OF_RESOURCE == pmix_hotel_checkin(&h, (void *) 0x1, &room));

    /* and the object is re-usable */
    PMIX_CONSTRUCT(&h, pmix_hotel_t);
    report("re-init after destruct: PMIX_SUCCESS",
           PMIX_SUCCESS == pmix_hotel_init(&h, 4, NULL, 0, dummy_evict));
    report("re-init after destruct: checkin works",
           PMIX_SUCCESS == pmix_hotel_checkin(&h, (void *) 0x1, &room));
    PMIX_DESTRUCT(&h);
}

/* Re-initializing a live hotel used to overwrite all three array pointers
 * and leak everything they addressed. */
static void test_reinit(void)
{
    pmix_hotel_t h;
    int room = -1, i;
    void *occupant;
    bool ok = true;

    PMIX_CONSTRUCT(&h, pmix_hotel_t);
    report("reinit: first init succeeds",
           PMIX_SUCCESS == pmix_hotel_init(&h, 2, NULL, 0, dummy_evict));
    pmix_hotel_checkin(&h, (void *) 0x1, &room);
    report("reinit: first hotel took a guest", 0 <= room);

    report("reinit: second init succeeds",
           PMIX_SUCCESS == pmix_hotel_init(&h, 8, NULL, 0, dummy_evict));
    report("reinit: re-initialized hotel is empty", pmix_hotel_is_empty(&h));

    /* all eight rooms of the new hotel must be usable */
    for (i = 0; i < 8; ++i) {
        if (PMIX_SUCCESS != pmix_hotel_checkin(&h, (void *) (intptr_t) (i + 1), &room)) {
            ok = false;
            break;
        }
    }
    report("reinit: all 8 new rooms check in", ok);
    report("reinit: ninth checkin declines",
           PMIX_ERR_OUT_OF_RESOURCE == pmix_hotel_checkin(&h, (void *) 0x99, &room));

    pmix_hotel_knock(&h, 0, &occupant);
    report("reinit: room 0 has an occupant", NULL != occupant);

    PMIX_DESTRUCT(&h);
}

/* Regression (all builds): the upper bound on room_num used to be an
 * assert() and nothing else, so in every build that ships -- config/pmix.m4
 * adds -DNDEBUG whenever --enable-debug is off -- a room number at or above
 * num_rooms was not checked at all. checkout() then *wrote* through
 * rooms[room_num] past the end of the array, and pushed a second
 * out-of-bounds write into unoccupied_rooms[] by way of
 * ++last_unoccupied_room. The lower bound was always an "if".
 *
 * These cases are written against observable bookkeeping rather than
 * against a fault, so they say something in an optimized build: an
 * out-of-range room must leave occupancy exactly as it found it. Backing
 * the fix out aborts on the assert in a debug build and corrupts
 * last_unoccupied_room here in an optimized one. */
static void test_out_of_range_room_number(void)
{
    pmix_hotel_t h;
    int room = -1;
    int saved;
    void *occupant = (void *) 0xdeadbeef;

    PMIX_CONSTRUCT(&h, pmix_hotel_t);
    report("out-of-range: init succeeds",
           PMIX_SUCCESS == pmix_hotel_init(&h, 4, NULL, 0, dummy_evict));
    report("out-of-range: checkin works", PMIX_SUCCESS == pmix_hotel_checkin(&h, (void *) 0x1, &room));
    saved = h.last_unoccupied_room;

    /* one past the end, and far past the end */
    pmix_hotel_checkout(&h, 4);
    pmix_hotel_checkout(&h, 99999);
    report("out-of-range: checkout past the end changes no bookkeeping",
           saved == h.last_unoccupied_room);
    report("out-of-range: the real occupant is still checked in", !pmix_hotel_is_empty(&h));

    pmix_hotel_checkout_and_return_occupant(&h, 4, &occupant);
    report("out-of-range: checkout_and_return past the end yields NULL", NULL == occupant);
    occupant = (void *) 0xdeadbeef;
    pmix_hotel_knock(&h, 77, &occupant);
    report("out-of-range: knock past the end yields NULL", NULL == occupant);
    report("out-of-range: still no bookkeeping change", saved == h.last_unoccupied_room);

    /* the room that really is occupied still works */
    pmix_hotel_knock(&h, room, &occupant);
    report("out-of-range: the in-range room still answers", (void *) 0x1 == occupant);

    PMIX_DESTRUCT(&h);

    /* A destructed hotel has num_rooms 0 and a NULL rooms[]; room 0 is
     * "in range" by the old lower-bound-only test and dereferenced NULL. */
    occupant = (void *) 0xdeadbeef;
    pmix_hotel_checkout(&h, 0);
    pmix_hotel_checkout_and_return_occupant(&h, 0, &occupant);
    report("destructed hotel: checkout of room 0 is a no-op, not a fault", NULL == occupant);
    occupant = (void *) 0xdeadbeef;
    pmix_hotel_knock(&h, 0, &occupant);
    report("destructed hotel: knock at room 0 is a no-op, not a fault", NULL == occupant);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    fprintf(stdout, "\n=== pmix_hotel_t unit tests ===\n\n");

    test_init();
    test_init_bad_params();
    test_checkin_checkout();
    test_checkout_and_return();
    test_knock();
    test_hotel_full();
    test_multiple_guests();
    test_room_reuse();
    test_static_init();
    test_destruct_leaves_constructed_state();
    test_reinit();
    test_out_of_range_room_number();

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
