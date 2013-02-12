/* libdlb - data structures and utilities library
 * Copyright (C) 2013 Daniel Beer <dlbeer@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "waitq.h"
#include "rbt_iter.h"
#include "containers.h"

static int cmp_by_deadline(const void *key, const struct rbt_node *node)
{
	const struct waitq_timer *kt = (const struct waitq_timer *)key;
	const struct waitq_timer *nt =
		container_of(node, const struct waitq_timer, waiting_set);

	if (kt->deadline < nt->deadline)
		return -1;
	if (kt->deadline > nt->deadline)
		return 1;
	if (kt < nt)
		return -1;
	if (kt > nt)
		return 1;

	return 0;
}

static struct waitq_timer *expire_one(struct waitq *wq, clock_ticks_t now)
{
	struct rbt_node *n;
	struct waitq_timer *t = NULL;

	thr_mutex_lock(&wq->lock);
	n = rbt_iter_first(&wq->waiting_set);
	if (n) {
		t = container_of(n, struct waitq_timer, waiting_set);
		if (t->deadline > now)
			t = NULL;
		else
			rbt_remove(&wq->waiting_set, n);
	}
	thr_mutex_unlock(&wq->lock);

	return t;
}

void waitq_init(struct waitq *wq)
{
	wq->wakeup = NULL;
	thr_mutex_init(&wq->lock);
	wq->waiting_set.root = NULL;
	wq->waiting_set.compare = cmp_by_deadline;
}

void waitq_destroy(struct waitq *wq)
{
	thr_mutex_destroy(&wq->lock);
}

int waitq_next_deadline(struct waitq *wq)
{
	struct rbt_node *n;
	clock_ticks_t now = clock_now();
	clock_ticks_t deadline;

	thr_mutex_lock(&wq->lock);
	n = rbt_iter_first(&wq->waiting_set);
	if (n)
		deadline = container_of(n, struct waitq_timer,
			waiting_set)->deadline;
	thr_mutex_unlock(&wq->lock);

	if (!n)
		return -1;
	if (deadline < now)
		return 0;

	return deadline - now;
}

unsigned int waitq_dispatch(struct waitq *wq, struct runq *rq,
			    unsigned int limit)
{
	unsigned int count = 0;
	clock_ticks_t now = clock_now();

	while (!limit || count < limit) {
		struct waitq_timer *t = expire_one(wq, now);

		if (!t)
			break;

		runq_exec(rq, &t->task, t->task.func);
		count++;
	}

	return count;
}

void waitq_wait(struct waitq *wq, struct waitq_timer *t,
		int interval_ms, waitq_timer_func_t func)
{
	int need_wakeup = 0;

	t->task.func = (runq_task_func_t)func;
	t->deadline = clock_now() + interval_ms;

	thr_mutex_lock(&wq->lock);
	rbt_insert(&wq->waiting_set, t, &t->waiting_set);
	need_wakeup = !rbt_iter_prev(&t->waiting_set);
	thr_mutex_unlock(&wq->lock);

	if (need_wakeup && wq->wakeup)
		wq->wakeup(wq);
}

static void reschedule(struct waitq *wq, struct waitq_timer *t,
		       clock_ticks_t deadline)
{
	struct rbt_node *n;
	int need_wakeup = 0;

	/* See if the timer is in the set. If so, de-queue it. */
	thr_mutex_lock(&wq->lock);
	n = rbt_find(&wq->waiting_set, t);
	if (n) {
		rbt_remove(&wq->waiting_set, n);
		need_wakeup |= !rbt_iter_prev(n);
	}
	thr_mutex_unlock(&wq->lock);

	if (!n)
		return;

	/* Rewrite the deadline */
	t->deadline = deadline;

	/* Put it back in the set */
	thr_mutex_lock(&wq->lock);
	rbt_insert(&wq->waiting_set, t, &t->waiting_set);
	need_wakeup |= !rbt_iter_prev(&t->waiting_set);
	thr_mutex_unlock(&wq->lock);

	if (need_wakeup && wq->wakeup)
		wq->wakeup(wq);
}

void waitq_cancel(struct waitq *wq, struct waitq_timer *t)
{
	reschedule(wq, t, 0);
}

void waitq_reschedule(struct waitq *wq, struct waitq_timer *t,
		      int interval_ms)
{
	reschedule(wq, t, clock_now() + interval_ms);
}
