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

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);
    return (nfail > 0) ? 1 : 0;
}
