#ifndef PMIX_EVENT_H
#define PMIX_EVENT_H

#include <event.h>
#include <stdlib.h>

/* Dealing with eventlib.
 * TODO: Move it somwhere else */

/* Event priority APIs */
#define pmix_event_base_priority_init(b, n) event_base_priority_init((b), (n))

#define pmix_event_set_priority(x, n) event_priority_set((x), (n))

/* Basic event APIs */
#define PMIX_EV_TIMEOUT EV_TIMEOUT
#define PMIX_EV_READ    EV_READ
#define PMIX_EV_WRITE   EV_WRITE
#define PMIX_EV_SIGNAL  EV_SIGNAL
/* Persistent event: won't get removed automatically when activated. */
#define PMIX_EV_PERSIST EV_PERSIST

#define PMIX_EVLOOP_ONCE     EVLOOP_ONCE        /**< Block at most once. */
#define PMIX_EVLOOP_NONBLOCK EVLOOP_NONBLOCK    /**< Do not block. */
typedef struct event pmix_event_t;
typedef struct event_base pmix_event_base_t;

#define pmix_event_enable_debug_mode() event_enable_debug_mode()

#define pmix_event_set(b, x, fd, fg, cb, arg) event_assign((x), (b), (fd), (fg), (event_callback_fn) (cb), (arg))

#define pmix_event_add(ev, tv) event_add((ev), (tv))

#define pmix_event_del(ev) event_del((ev))

#define pmix_event_active(x, y, z) event_active((x), (y), (z))

#define pmix_event_new(b, fd, fg, cb, arg) event_new((b), (fd), (fg), (event_callback_fn) (cb), (arg))

static inline pmix_event_t* pmix_event_alloc(void)
{
    pmix_event_t *ev;

    ev = (pmix_event_t*)malloc(sizeof(pmix_event_t));
    return ev;
}

#define pmix_event_free(x) event_free((x))

#define PMIX_EV_ERROR_PRI         0
#define PMIX_EV_MSG_HI_PRI        1
#define PMIX_EV_SYS_HI_PRI        2
#define PMIX_EV_MSG_LO_PRI        3
#define PMIX_EV_SYS_LO_PRI        4
#define PMIX_EV_INFO_HI_PRI       5
#define PMIX_EV_INFO_LO_PRI       6
#define PMIX_EV_LOWEST_PRI        7

#endif
