// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Carlos López <carlos.lopezr4096@gmail.com>

#define _GNU_SOURCE
#include <assert.h>
#include <err.h>
#include <linux/membarrier.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "pcref.h"
#include "rseq/rseq.h"

/* Per-thread time slice enable state */
__thread int8_t __time_slice_state;

static pthread_once_t init_once = PTHREAD_ONCE_INIT;

static struct pcref_global {
	unsigned int max_cpus;
} global = {};

#define membarrier(cmd, flags, cpu) \
	syscall(__NR_membarrier, cmd, flags, cpu)

static void global_init(void)
{
	long mask;

	/* librseq needs one process-wide init to consume libc's rseq
	 * registration. */
	if (rseq_init() != RSEQ_INIT_OK)
		errx(EXIT_FAILURE, "pcref: rseq_init() failed");
	if (!rseq_available(RSEQ_AVAILABLE_QUERY_KERNEL))
		errx(EXIT_FAILURE, "pcref: kernel rseq support is required");
	if (!rseq_registered())
		errx(EXIT_FAILURE,
			 "pcref: rseq is not registered for this thread");
	if (!rseq_mm_cid_available())
		errx(EXIT_FAILURE, "pcref: rseq mm_cid support is required");

	mask = membarrier(MEMBARRIER_CMD_QUERY, 0, 0);
	if (mask < 0 || !(mask & MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ))
		errx(EXIT_FAILURE,
			 "pcref: membarrier(PRIVATE_EXPEDITED_RSEQ) is required");
	if (membarrier(MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_RSEQ, 0,
				   0))
		err(EXIT_FAILURE, "pcref: membarrier register failed");

	global.max_cpus = rseq_get_max_nr_cpus();
}

static size_t pcref_get_max_cpus(void)
{
	pthread_once(&init_once, global_init);
	return (size_t)global.max_cpus;
}

int pcref_init(struct pcref *ref, pcref_func_t release,
			   unsigned int flags)
{
	size_t ncpus;

	ref->release = release;
	ref->cells = NULL;
	ref->ncpus = 0;
	atomic_init(&ref->dying, 0);
	atomic_init(&ref->mode, PCREF_MODE_ATOMIC);
	atomic_init(&ref->count, 1);

	if ((flags & PCREF_INIT_ATOMIC))
		return 0;

	ncpus = pcref_get_max_cpus();
	if (posix_memalign((void **)&ref->cells, PCREF_CACHELINE,
					   ncpus * sizeof(ref->cells[0])))
		return -ENOMEM;
	memset(ref->cells, 0, ncpus * sizeof(ref->cells[0]));

	ref->ncpus = ncpus;

	/* Add +1 for the owner's reference. */
	atomic_store_relaxed(&ref->count, PCREF_BIAS + 1);
	atomic_store_release(&ref->mode, PCREF_MODE_PERCPU);
	return 0;
}

void pcref_exit(struct pcref *ref)
{
	DBG_ASSERT(atomic_load(&ref->mode) != PCREF_MODE_SWITCHING);
	/*
	 * Freeing the cells is safe because this function should only be
	 * called after active users are gone (e.g. from release callback).
	 */
	free(ref->cells);
	ref->cells = NULL;
	ref->ncpus = 0;
}

static void rseq_membarrier(void)
{
	if (membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_RSEQ, 0, 0))
		err(EXIT_FAILURE, "pcref: membarrier(PRIVATE_EXPEDITED_RSEQ)");
}

/*
 * Lock-free conversion from PERCPU to ATOMIC mode. Calls @confirm once
 * the switch has completed.
 *
 * Only one thread can start executing the transition PERCPU ->
 * SWITCHING. Threads that do not execute it call @confirm and return
 * without waiting. The only guarantees are that @ref is no longer in
 * PERCPU mode when @confirm is called, and that no rseq critical
 * sections are running or will be run from that point on (but ATOMIC
 * mode may not be ready and thus ref->count may not be reliable yet).
 *
 * If a caller wishes to block until the ref is completely in ATOMIC
 * mode, it may spin within @confirm, waiting until pcref_is_atomic()
 * returns true.
 *
 * The caller must already hold a reference for this function to be
 * safe to call.
 */
void pcref_switch_to_atomic(struct pcref *ref, pcref_func_t confirm)
{
	intptr_t mode = PCREF_MODE_PERCPU;
	intptr_t sum = 0, delta, old;
	uint32_t i;

	/* If born atomic mode, there is nothing to switch */
	if (!ref->cells) {
		if (confirm)
			confirm(ref);
		return;
	}

	/*
	 * Flip the mode. Subsequent attempts to update the refcount will
	 * observe the new mode and will not enter the rseq critical
	 * section.
	 */
	if (!atomic_cas_strong_seqcst(&ref->mode, &mode,
								  PCREF_MODE_SWITCHING)) {
		if (confirm) {
			if (mode == PCREF_MODE_SWITCHING)
				rseq_membarrier();
			confirm(ref);
		}
		return;
	}

	/*
	 * Wait until all rseq critical sections on all other CPUs are done.
	 */
	rseq_membarrier();

	/*
	 * There can be no more updates to the per-CPU cells at this point.
	 * Fold them into the atomic counter, dropping the bias.
	 */
	for (i = 0; i < ref->ncpus; ++i)
		sum += RSEQ_READ_ONCE(ref->cells[i].cnt);
	delta = sum - PCREF_BIAS;

	/*
	 * The caller holds a reference for the whole call, so the true
	 * count is >= 1 and this commit can never reach zero
	 */
	old = atomic_fetch_add_acqrel(&ref->count, delta);
	assert(old + delta > 0);
	(void)old;

	/* Now set mode to ATOMIC, signalling that the count is exact. */
	atomic_store_release(&ref->mode, PCREF_MODE_ATOMIC);

	if (confirm)
		confirm(ref);
}

static inline void __pcref_kill(struct pcref *ref)
{
#ifdef DEBUG
	int old;

	/*
	 * Use default ordering (seq_cst) to pair with seq_cst increment in
	 * pcref_tryget_live()
	 */
	old = atomic_exchange(&ref->dying, 1);
	assert(!old);
#else
	atomic_store(&ref->dying, 1);
#endif
}

void pcref_kill_and_confirm(struct pcref *ref, pcref_func_t confirm)
{
	__pcref_kill(ref);
	pcref_switch_to_atomic(ref, confirm);
	/*
	 * This function must be called only once, meaning that the switch
	 * to ATOMIC should have completed upon return from
	 * pcref_switch_to_atomic() Drop the owner's reference through the
	 * atomic path.
	 */
	__pcref_atomic_put(ref, 1);
}
