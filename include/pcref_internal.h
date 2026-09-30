/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2026 Carlos López <carlos.lopezr4096@gmail.com>
 *
 * pcref implementation internals
 */
#ifndef PCREF_INTERNAL_H
#define PCREF_INTERNAL_H

#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rseq/compiler.h"
#include <rseq/rseq.h>

#include "pcref_rseq.h"

#ifdef DEBUG
#define DBG_ASSERT(s) assert(s)
#else
#define DBG_ASSERT(s)
#endif

#define PCREF_CACHELINE 64

#define pcref_container_of(ptr, type, member) \
	((type *)((char *)(ptr) - offsetof(type, member)))

/* Thin wrappers over the C11 atomics, naming the memory orders pcref
 * uses. */
#define atomic_load_relaxed(r) \
	atomic_load_explicit(r, memory_order_relaxed)
#define atomic_load_acquire(r) \
	atomic_load_explicit(r, memory_order_acquire)
#define atomic_store_release(r, v) \
	atomic_store_explicit(r, v, memory_order_release)
#define atomic_store_relaxed(r, v) \
	atomic_store_explicit(r, v, memory_order_relaxed)
#define atomic_fetch_add_relaxed(r, v) \
	atomic_fetch_add_explicit(r, v, memory_order_relaxed)
#define atomic_fetch_add_acqrel(r, v) \
	atomic_fetch_add_explicit(r, v, memory_order_acq_rel)
#define atomic_fetch_sub_acqrel(r, v) \
	atomic_fetch_sub_explicit(r, v, memory_order_acq_rel)
#define atomic_cas_strong_seqcst(r, e, d)    \
	atomic_compare_exchange_strong_explicit( \
			r, e, d, memory_order_seq_cst, memory_order_acquire)

/* Values of pcref::mode */
enum {
	/*
	 * Reference count updates take the fast path into the per-CPU
	 * counters.
	 */
	PCREF_MODE_PERCPU = 0,
	/*
	 * Intermediate state. Per-CPU fast paths are disabled but there
	 * may be in-flight rseq critical sections, and the global refcount
	 * has not been updated from the per-CPU ones.
	 */
	PCREF_MODE_SWITCHING = 1,
	/*
	 * Reference count updates are routed to the global shared counter,
	 * and all rseq critical sections are done. The global counter has
	 * a valid value.
	 */
	PCREF_MODE_ATOMIC = 2,
};

/*
 * While in PERCPU mode, the atomic counter carries this bias so that
 * slow-path get/put (used by threads without rseq, or after repeated
 * aborts) can never make it reach zero and trigger a bogus release. The
 * bias is removed when the per-CPU cells are folded into the atomic
 * counter.
 */
#define PCREF_BIAS ((intptr_t)1 << (sizeof(intptr_t) * 8 - 2))

/*
 * Per-CPU reference count.
 *
 * Cells may underflow if a reference is acquired on one CPU and
 * released in another.
 */
struct pcref_cell {
	intptr_t cnt;
} __attribute__((aligned(PCREF_CACHELINE)));

/*
 * Reference count tracking structure, embedded in the refcounted
 * object.
 */
struct pcref {
	/* One of PCREF_MODE_* */
	_Atomic intptr_t mode;
	/* Per-CPU objects, NULL when ATOMIC-from-birth */
	struct pcref_cell *cells;
	/* Object release function, called only once */
	void (*release)(struct pcref *ref);
	uint32_t ncpus;
	/* Set by pcref_kill() */
	_Atomic int dying;
	/* Global atomic reference count */
	_Atomic intptr_t count;
};

/* Function type for release and confirmation callbacks */
typedef void (*pcref_func_t)(struct pcref *ref);

#define PCREF_FAST_RETRIES 8

/*
 * Add @delta to this object's refcount, when in per-CPU mode.
 * Returns false if the caller should use the atomic slow path.
 */
static inline bool __pcref_percpu_add(struct pcref *ref, intptr_t delta)
{
	struct pcref_cell *cell;
	uint32_t cid;
	intptr_t old;
	int ret, i;

	if (rseq_unlikely(atomic_load_acquire(&ref->mode) !=
					  PCREF_MODE_PERCPU))
		return false;

	if (rseq_unlikely(rseq_current_cpu_raw() < 0))
		return false;

	for (i = 0; i < PCREF_FAST_RETRIES; ++i) {
		cid = rseq_current_mm_cid();
		if (rseq_unlikely(cid >= ref->ncpus))
			return false;

		scoped_time_slice() {
			cell = &ref->cells[cid];
			old = RSEQ_READ_ONCE(cell->cnt);

			ret = rseq_load_cbne_load_cbne_store__ptr_relaxed_mm_cid(
					(intptr_t *)&cell->cnt, old, (intptr_t *)&ref->mode,
					PCREF_MODE_PERCPU, old + delta, (int)cid);
		}

		if (rseq_likely(!ret))
			return true;

		/* Failed due to preemption, retry */
		if (ret < 0)
			continue;

		/* Cell or mode changed. Abort if mode is no longer per-CPU */
		if (atomic_load_relaxed(&ref->mode) != PCREF_MODE_PERCPU)
			return false;
	}

	return false;
}

static inline void __pcref_atomic_put(struct pcref *ref,
									  unsigned long nr)
{
	intptr_t delta = (intptr_t)nr;
	intptr_t old;

	old = atomic_fetch_sub_acqrel(&ref->count, delta);
	DBG_ASSERT(old >= delta);
	if (old == delta)
		ref->release(ref);
}

/*
 * Atomic-mode tryget: take @nr references unless the count is zero.
 * Uses @order as the ordering for a successful CAS..
 */
static inline bool __pcref_atomic_tryget(struct pcref *ref,
										 unsigned long nr,
										 memory_order order)
{
	intptr_t c = atomic_load_relaxed(&ref->count);

	do {
		if (!c)
			return false;
	} while (!atomic_compare_exchange_weak_explicit(
			&ref->count, &c, c + (intptr_t)nr, order,
			memory_order_relaxed));

	DBG_ASSERT(c > 0);
	DBG_ASSERT((uintptr_t)nr <= (uintptr_t)INTPTR_MAX - (uintptr_t)c);

	return true;
}

static inline bool __pcref_tryget(struct pcref *ref, unsigned long nr,
								  memory_order order)
{
	if (__pcref_percpu_add(ref, (intptr_t)nr))
		return true;
	return __pcref_atomic_tryget(ref, nr, order);
}

#endif /* PCREF_INTERNAL_H */
