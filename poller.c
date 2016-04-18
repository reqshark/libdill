/*

  Copyright (c) 2016 Martin Sustrik

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"),
  to deal in the Software without restriction, including without limitation
  the rights to use, copy, modify, merge, publish, distribute, sublicense,
  and/or sell copies of the Software, and to permit persons to whom
  the Software is furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included
  in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
  THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
  IN THE SOFTWARE.

*/

#include "cr.h"
#include "list.h"
#include "poller.h"
#include "utils.h"

/* Forward declarations for the functions implemented by specific poller
   mechanisms (poll, epoll, kqueue). */
void dill_poller_init(void);
static void dill_poller_addin(struct dill_clause *cl, int id, int fd);
static void dill_poller_addout(struct dill_clause *cl, int id, int fd);
static void dill_poller_clean(int fd);
static int dill_poller_poll(int timeout);

/* Global linked list of all timers. The list is ordered.
   First timer to be resumed comes first and so on. */
static struct dill_list dill_timers = {0};

/* Adds a timer clause to the list of waited for clauses. */
void dill_addtimer(struct dill_clause *cl, int id, int64_t deadline) {
    dill_assert(deadline >= 0);
    cl->i = deadline;
    /* Move the timer into the right place in the ordered list
       of existing timers. TODO: This is an O(n) operation! */
    struct dill_list_item *it = dill_list_begin(&dill_timers);
    while(it) {
        struct dill_clause *itcl = dill_cont(it, struct dill_clause, epitem);
        /* If multiple timers expire at the same momemt they will be fired
           in the order they were created in (> rather than >=). */
        if(itcl->i > cl->i)
            break;
        it = dill_list_next(it);
    }
    dill_list_item_init(&cl->epitem);
    dill_list_insert(&dill_timers, &cl->epitem, it);
    cl->eplist = &dill_timers;
    dill_waitfor(cl, id);
}

/* Returns number of milliseconds till next timer expiration.
   -1 stands for infinity. */
static int dill_timer_next(void) {
    if(dill_list_empty(&dill_timers))
        return -1;
    int64_t nw = now();
    int64_t expiry = dill_cont(dill_list_begin(&dill_timers),
        struct dill_clause, epitem)->i;
    return (int) (nw >= expiry ? 0 : expiry - nw);
}

void dill_poller_wait(int block) {
    dill_poller_init();
    while(1) {
        /* Compute timeout for the subsequent poll. */
        int timeout = block ? dill_timer_next() : 0;
        /* Wait for events. */
        int fd_fired = dill_poller_poll(timeout);
        /* Fire all expired timers. */
        int timer_fired = 0;
        if(!dill_list_empty(&dill_timers)) {
            int64_t nw = now();
            while(!dill_list_empty(&dill_timers)) {
                struct dill_clause *cl = dill_cont(
                    dill_list_begin(&dill_timers), struct dill_clause, epitem);
                if(cl->i > nw)
                    break;
                dill_list_erase(&dill_timers, dill_list_begin(&dill_timers));
                dill_trigger(cl, ETIMEDOUT);
                timer_fired = 1;
            }
        }
        /* Never retry the poll in non-blocking mode. */
        if(!block || fd_fired || timer_fired)
            break;
        /* If timeout was hit but there were no expired timers do the poll
           again. It can happen if the timers were canceled in the meantime. */
    }
}

int dill_fdin(int fd, int64_t deadline, const char *current) {
    dill_poller_init();
    struct dill_clause fdcl;
    struct dill_clause tmcl;
    dill_poller_addin(&fdcl, 1, fd);
    if(deadline > 0)
        dill_addtimer(&tmcl, 2, deadline);
    int id = dill_wait();
    if(dill_slow(id < 0)) return -1;
    if(dill_slow(id == 2)) {errno = ETIMEDOUT; return -1;}
    dill_assert(id == 1);
    return 0;
}

int dill_fdout(int fd, int64_t deadline, const char *current) {
    dill_poller_init();
    struct dill_clause fdcl;
    struct dill_clause tmcl;
    dill_poller_addout(&fdcl, 1, fd);
    if(deadline > 0)
        dill_addtimer(&tmcl, 2, deadline);
    int id = dill_wait();
    if(dill_slow(id < 0)) return -1;
    if(dill_slow(id == 2)) {errno = ETIMEDOUT; return -1;}
    dill_assert(id == 1);
    return 0;
}

/* Include the poll-mechanism-specific stuff. */

/* User overloads. */
#if defined DILL_EPOLL
#include "epoll.inc"
#elif defined DILL_KQUEUE
#include "kqueue.inc"
#elif defined DILL_POLL
#include "poll.inc"
/* Defaults. */
#elif defined __linux__ && !defined DILL_NO_EPOLL
#include "epoll.inc"
#elif defined BSD && !defined DILL_NO_KQUEUE
#include "kqueue.inc"
#else
#include "poll.inc"
#endif

