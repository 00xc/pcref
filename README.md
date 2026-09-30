# pcref

pcref is a library for scalable reference counting using per-CPU atomics. It
is a userspace reimplementation of the Linux kernel's [per-CPU reference
counting](https://lwn.net/Articles/557478/).

## Background

Reference counting is a memory reclamation mechanism, where a counter of active
users of an object is kept along with that object. When the number of users
drops to zero, the object can be reclaimed (freed).

To implement the reference count, one typically relies on a shared atomic
variable per object, which is incremented and decremented as references are
acquired and released. This has poor scalability, as the cache line owning
the reference count bounces across each CPU updating it. In other words: all
CPUs which access the object, even if they only perform reads, must fight over
updating the counter in a synchronized manner.

### Per-CPU reference counting

Per-CPU refcounting keeps a separate counter for each object on every CPU. For
most of the lifetime of the object, only the per-CPU counters are updated. This
is of course more efficient, but introduces an issue, as there is no single
source for the actual combined reference count of the object.

The key idea is that, only after the original owner of the object drops its
reference, the counters are switched from per-CPU to a single shared one (using
an atomic variable, just like in a naive implementation). If the owner dropping
its reference is treated as a careful synchronization point, the accounting can
be kept accurate.

The owner of the object typically only drops its reference at the end of the of
the lifetime of the object, so the same accounting is achieved with much less of
the cost.

### Per-CPU data in userspace

Since userspace does not have actual per-CPU structures that can be accessed
with disabled preemption, like the kernel does, pcref relies on Linux's
[restartable sequences](https://docs.kernel.org/userspace-api/rseq.html) via
[librseq](https://github.com/compudj/librseq) to safely access per-CPU data
with no contention and no thread local variables.

### Differences to Linux kernel implementation

pcref currently only supports a subset of the Linux kernel's implementation,
and has some differences:

* Switching a reference from per-CPU mode to atomic mode is supported, but not
  the other way around.
* State changes (per-CPU -> atomic) are lock free, but only opt-in synchronous.
  The user must manually wait until the change has completed if they want
  blocking behavior.

## Requirements

* glibc >= 2.35 for automated rseq registration.
* Linux kernel >= v6.3:
  * Restartable sequences v2 with `mm_cid`.
  * membarrier with `PRIVATE_EXPEDITED_RSEQ`.

Note: pcref also makes use of scheduler time slice extensions if available
(kernel >= v7.0), but this is not a hard requirement.

## API

A full description of the public API can be found in [`include/pcref.h`]. The
core functionality is:

```c
/* Initialization */
int pcref_init(struct pcref *ref, pcref_func_t release, unsigned int flags);

/* Basic reference acquisition & release */
void pcref_get(struct pcref *ref);
void pcref_put(struct pcref *ref);

/*
 * Safe reference acquisition & release for users that do not already hold a
 * reference.
 */
bool pcref_tryget(struct pcref *ref);
bool pcref_tryget_live(struct pcref *ref);

/* Shutdown (releases owner's reference, marks object as dying) */
void pcref_kill(struct pcref *ref);

/* Resource release */
void pcref_exit(struct pcref *ref);
```

See also the `PCREF_DEFINE()` macro, which will generate a typed API for your
refcounted object.
