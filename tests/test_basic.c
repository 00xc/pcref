// SPDX-License-Identifier: MIT
// Stress test: workers hammer get/put (with forced CPU migrations)
// while the owner kills the object at a random moment.  Checks that the
// destroy callback runs exactly once and only after every holder is
// gone.
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "pcref.h"

struct obj {
	struct pcref ref;
	int holders; /* workers currently holding their start ref */
	int destroyed;
};

static void obj_destroy(struct obj *o)
{
	assert(__atomic_load_n(&o->holders, __ATOMIC_SEQ_CST) == 0);
	assert(__atomic_add_fetch(&o->destroyed, 1, __ATOMIC_SEQ_CST) == 1);
}
PCREF_DEFINE(obj, struct obj, ref, obj_destroy)

struct worker {
	struct obj *o;
	_Atomic int *stop;
	unsigned seed;
};

static void *worker_fn(void *arg)
{
	struct worker *w = arg;
	struct obj *o = w->o;
	long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
	unsigned long n = 0;

	/* Start ref was taken by main on our behalf. */
	while (!atomic_load_explicit(w->stop, memory_order_relaxed)) {
		obj_get(o);
		if (obj_tryget(o)) {
			obj_put(o);
		}
		obj_put(o);
		if ((++n & 0x3ff) == 0) { /* force migrations -> rseq aborts */
			cpu_set_t set;
			CPU_ZERO(&set);
			CPU_SET(rand_r(&w->seed) % ncpu, &set);
			sched_setaffinity(0, sizeof(set), &set);
		}
	}
	__atomic_sub_fetch(&o->holders, 1, __ATOMIC_SEQ_CST);
	obj_put(o); /* drop start ref */
	return NULL;
}

static int run_round(unsigned flags, int nthreads, unsigned seed)
{
	struct obj o = { .holders = 0, .destroyed = 0 };
	_Atomic int stop = 0;
	pthread_t th[nthreads];
	struct worker w[nthreads];

	assert(obj_ref_init(&o, flags) == 0);
	int percpu = pcref_is_percpu(&o.ref);
	for (int i = 0; i < nthreads; i++) {
		w[i] = (struct worker){ &o, &stop, seed + i };
		obj_get(&o);
		__atomic_add_fetch(&o.holders, 1, __ATOMIC_SEQ_CST);
		pthread_create(&th[i], NULL, worker_fn, &w[i]);
	}
	usleep(rand_r(&seed) % 3000);
	obj_kill(&o); /* owner drops initial ref, mid-flight */
	assert(!obj_tryget_live(&o));
	usleep(rand_r(&seed) % 1000);
	atomic_store_explicit(&stop, 1, memory_order_relaxed);
	for (int i = 0; i < nthreads; i++)
		pthread_join(th[i], NULL);
	assert(__atomic_load_n(&o.destroyed, __ATOMIC_SEQ_CST) == 1);
	return percpu;
}

static int confirmed;

static void confirm_cb(struct pcref *ref)
{
	(void)ref;
	confirmed++;
}

int main(void)
{
	int nthreads = 8, percpu_rounds = 0, rounds = 300;

	for (int r = 0; r < rounds; r++)
		percpu_rounds +=
				run_round(0 /* default: per-cid */, nthreads, r + 1);
	printf("PERCPU-requested rounds ok (%d of %d actually per-CPU)\n",
		   percpu_rounds, rounds);

	for (int r = 0; r < 100; r++)
		assert(run_round(PCREF_INIT_ATOMIC, nthreads, r + 1000) == 0);
	printf("ATOMIC rounds ok\n");

	/* Single-threaded semantics. */
	struct obj o = { 0 };
	assert(obj_ref_init(&o, 0) == 0);
	obj_get(&o);
	obj_get(&o);
	obj_put(&o);
	obj_put(&o);
	assert(obj_tryget_live(&o));
	obj_put(&o);
	obj_switch_to_atomic(&o);
	/* One (owner) reference left: not zero yet, and is_zero readable
	 * now. */
	assert(!pcref_is_zero(&o.ref));
	/* Callbacks must still run on an already-switched object. */
	confirmed = 0;
	pcref_switch_to_atomic(&o.ref, confirm_cb);
	assert(confirmed == 1);
	pcref_kill_and_confirm(&o.ref, confirm_cb);
	assert(confirmed == 2);
	assert(o.destroyed == 1);
	printf("basic semantics ok\n");
	return 0;
}
