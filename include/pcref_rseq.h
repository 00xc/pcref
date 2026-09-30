/* SPDX-License-Identifier: MIT */
/*
 * Copyright (c) 2026 Carlos López <carlos.lopezr4096@gmail.com>
 *
 * librseq helpers
 */
#ifndef PCREF_RSEQ
#define PCREF_RSEQ

#include <linux/prctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <rseq/rseq.h>

#ifndef PR_RSEQ_SLICE_EXTENSION
#define PR_RSEQ_SLICE_EXTENSION 79
#define PR_RSEQ_SLICE_EXTENSION_GET 1
#define PR_RSEQ_SLICE_EXTENSION_SET 2
#define PR_RSEQ_SLICE_EXT_ENABLE 0x01
#endif

#ifndef __NR_rseq_slice_yield
#define __NR_rseq_slice_yield 471
#endif

/*
 * Per-thread enable state: 0 = not yet tried, 1 = enabled, -1 =
 * unavailable. Enablement is per-thread (via prctl), so it is resolved
 * lazily on first use of a bracketed critical section. Defined in
 * pcref.c.
 */
extern __thread int8_t __time_slice_state;

#define rseq_slice_yield() syscall(__NR_rseq_slice_yield)

static inline bool time_slice_enable(void)
{
	if (!rseq_slice_ctrl_available())
		return false;
	return prctl(PR_RSEQ_SLICE_EXTENSION, PR_RSEQ_SLICE_EXTENSION_SET,
				 PR_RSEQ_SLICE_EXT_ENABLE, 0, 0) == 0;
}

static inline bool time_slice_enabled(void)
{
	if (rseq_unlikely(!__time_slice_state))
		__time_slice_state = time_slice_enable() ? 1 : -1;
	return __time_slice_state > 0;
}

static inline void time_slice_begin(void)
{
	if (!time_slice_enabled())
		return;

	RSEQ_WRITE_ONCE(rseq_get_abi()->slice_ctrl.request, 1);
	rseq_barrier();
}

static inline void time_slice_end(void)
{
	struct rseq_abi *abi;

	if (!time_slice_enabled())
		return;

	rseq_barrier();
	abi = rseq_get_abi();
	RSEQ_WRITE_ONCE(abi->slice_ctrl.request, 0);
	if (rseq_unlikely(RSEQ_READ_ONCE(abi->slice_ctrl.granted)))
		rseq_slice_yield();
}

typedef int time_slice_guard_t;

static inline void
__time_slice_guard_cleanup(const time_slice_guard_t *guard)
{
	(void)guard;
	time_slice_end();
}

/*
 * time_slice_guard(): arm a slice request for the rest of the enclosing
 * block.
 */
#define time_slice_guard()                                             \
	time_slice_guard_t __time_slice_guard                              \
			__attribute__((__cleanup__(__time_slice_guard_cleanup))) = \
					(time_slice_begin(), 0)

/*
 * scoped_time_slice(): arm a slice request for just the following
 * statement or block.
 */
#define scoped_time_slice()                                         \
	for (time_slice_guard_t __time_slice_guard                      \
		 __attribute__((__cleanup__(__time_slice_guard_cleanup))) = \
				 (time_slice_begin(), 0),                           \
		 *__time_slice_done = NULL;                                 \
		 !__time_slice_done; __time_slice_done = (void *)1)

#endif
