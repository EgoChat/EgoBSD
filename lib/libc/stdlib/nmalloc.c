/*
 * NMALLOC.C	- New Malloc (ported from kernel slab allocator)
 *
 * Copyright (c) 2003,2004,2009,2010-2019 The DragonFly Project,
 * All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@backplane.com> and by
 * Venkatesh Srinivas <me@endeavour.zapto.org>.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $Id: nmalloc.c,v 1.37 2010/07/23 08:20:35 vsrinivas Exp $
 */
/*
 * This module implements a slab allocator drop-in replacement for the
 * libc malloc().
 *
 * A slab allocator reserves a ZONE for each chunk size, then lays the
 * chunks out in an array within the zone.  Allocation and deallocation
 * is nearly instantaneous, and overhead losses are limited to a fixed
 * worst-case amount.
 *
 * The slab allocator does not have to pre-initialize the list of
 * free chunks for each zone, and the underlying VM will not be
 * touched at all beyond the zone header until an actual allocation
 * needs it.
 *
 * Slab management and locking is done on a per-zone basis.
 *
 *	Alloc Size	Chunking        Number of zones
 *	0-127		8		16
 *	128-255		16		8
 *	256-511		32		8
 *	512-1023	64		8
 *	1024-2047	128		8
 *	2048-4095	256		8
 *	4096-8191	512		8
 *	8192-16383	1024		8
 *	16384-32767	2048		8
 *
 *	Allocations >= ZoneLimit go directly to mmap and a hash table
 *	is used to locate for free.  One and Two-page allocations use the
 *	zone mechanic to avoid excessive mmap()/munmap() calls.
 *
 *			   API FEATURES AND SIDE EFFECTS
 *
 *    + power-of-2 sized allocations up to a page will be power-of-2 aligned.
 *	Above that power-of-2 sized allocations are page-aligned.  Non
 *	power-of-2 sized allocations are aligned the same as the chunk
 *	size for their zone.
 *    + malloc(0) returns a special non-NULL value
 *    + ability to allocate arbitrarily large chunks of memory
 *    + realloc will reuse the passed pointer if possible, within the
 *	limitations of the zone chunking.
 *
 * Multithreaded enhancements for small allocations introduced August 2010.
 * These are in the spirit of 'libumem'. See:
 *	Bonwick, J.; Adams, J. (2001). "Magazines and Vmem: Extending the
 *	slab allocator to many CPUs and arbitrary resources". In Proc. 2001
 *	USENIX Technical Conference. USENIX Association.
 *
 * Oversized allocations employ the BIGCACHE mechanic whereby large
 * allocations may be handed significantly larger buffers, allowing them
 * to avoid mmap/munmap operations even through significant realloc()s.
 * The excess space is only trimmed if too many large allocations have been
 * given this treatment.
 *
 * TUNING
 *
 * The value of the environment variable MALLOC_OPTIONS is a character string
 * containing various flags to tune nmalloc.
 *
 * 'U'   / ['u']	Generate / do not generate utrace entries for ktrace(1)
 *			This will generate utrace events for all malloc,
 *			realloc, and free calls. There are tools (mtrplay) to
 *			replay and allocation pattern or to graph heap structure
 *			(mtrgraph) which can interpret these logs.
 * 'Z'   / ['z']	Zero out / do not zero all allocations.
 *			Each new byte of memory allocated by malloc, realloc, or
 *			reallocf will be initialized to 0. This is intended for
 *			debugging and will affect performance negatively.
 * 'H'	/  ['h']	Pass a hint to the kernel about pages unused by the
 *			allocation functions.
 */

/* cc -shared -fPIC -g -O -I/usr/src/lib/libc/include -o nmalloc.so nmalloc.c */

#include "namespace.h"
#include <sys/param.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/queue.h>
#include <sys/ktrace.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <machine/atomic.h>
#include "un-namespace.h"

#include "libc_private.h"
#include "spinlock.h"

void __free(void *);
void *__malloc(size_t);
void *__calloc(size_t, size_t);
void *__realloc(void *, size_t);
void *__aligned_alloc(size_t, size_t);
size_t __malloc_usable_size(const void *ptr);
int __posix_memalign(void **, size_t, size_t);

/*
 * Linked list of large allocations
 */
typedef struct bigalloc {
	struct bigalloc *next;	/* hash link */
	void	*base;		/* base pointer */
	u_long	active;		/* bytes active */
	u_long	bytes;		/* bytes allocated */
} *bigalloc_t;

/*
 * Note that any allocations which are exact multiples of PAGE_SIZE, or
 * which are >= ZALLOC_ZONE_LIMIT, will fall through to the kmem subsystem.
 */
#define MAX_SLAB_PAGEALIGN	(2 * PAGE_SIZE)	/* max slab for PAGE_SIZE*n */
#define ZALLOC_ZONE_LIMIT	(16 * 1024)	/* max slab-managed alloc */
#define ZALLOC_ZONE_SIZE	(64 * 1024)	/* zone size */
#define ZALLOC_SLAB_MAGIC	0x736c6162	/* magic sanity */

#if ZALLOC_ZONE_LIMIT == 16384
#define NZONES			72
#elif ZALLOC_ZONE_LIMIT == 32768
#define NZONES			80
#else
#error "I couldn't figure out NZONES"
#endif

/*
 * Chunk structure for free elements
 */
typedef struct slchunk {
	struct slchunk *c_Next;
} *slchunk_t;

/*
 * The IN-BAND zone header is placed at the beginning of each zone.
 */
struct slglobaldata;

typedef struct slzone {
	int32_t		z_Magic;	/* magic number for sanity check */
	int		z_NFree;	/* total free chunks / ualloc space */
	struct slzone *z_Next;		/* ZoneAry[] link if z_NFree non-zero */
	int		z_NMax;		/* maximum free chunks */
	char		*z_BasePtr;	/* pointer to start of chunk array */
	int		z_UIndex;	/* current initial allocation index */
	int		z_UEndIndex;	/* last (first) allocation index */
	int		z_ChunkSize;	/* chunk size for validation */
	int		z_FirstFreePg;	/* chunk list on a page-by-page basis */
	int		z_ZoneIndex;
	int		z_Flags;
	struct slchunk *z_PageAry[ZALLOC_ZONE_SIZE / PAGE_SIZE];
} *slzone_t;

typedef struct slglobaldata {
	spinlock_t	Spinlock;
	slzone_t	ZoneAry[NZONES];/* linked list of zones NFree > 0 */
} *slglobaldata_t;

#define SLZF_UNOTZEROD		0x0001

#define FASTSLABREALLOC		0x02

/*
 * Misc constants.  Note that allocations that are exact multiples of
 * PAGE_SIZE, or exceed the zone limit, fall through to the kmem module.
 * IN_SAME_PAGE_MASK is used to sanity-check the per-page free lists.
 */
#define MIN_CHUNK_SIZE		8		/* in bytes */
#define MIN_CHUNK_MASK		(MIN_CHUNK_SIZE - 1)
#define IN_SAME_PAGE_MASK	(~(intptr_t)PAGE_MASK | MIN_CHUNK_MASK)

/*
 * WARNING: A limited number of spinlocks are available, BIGXSIZE should
 *	    not be larger then 64.
 */
#define BIGHSHIFT	10			/* bigalloc hash table */
#define BIGHSIZE	(1 << BIGHSHIFT)
#define BIGHMASK	(BIGHSIZE - 1)
#define BIGXSIZE	(BIGHSIZE / 16)		/* bigalloc lock table */
#define BIGXMASK	(BIGXSIZE - 1)

/*
 * BIGCACHE caches oversized allocations.  Note that a linear search is
 * performed, so do not make the cache too large.
 *
 * BIGCACHE will garbage-collect excess space when the excess exceeds the
 * specified value.  A relatively large number should be used here because
 * garbage collection is expensive.
 */
#define BIGCACHE	16
#define BIGCACHE_MASK	(BIGCACHE - 1)
#define BIGCACHE_LIMIT	(1024 * 1024)		/* size limit */
#define BIGCACHE_EXCESS	(16 * 1024 * 1024)	/* garbage collect */

#define CACHE_CHUNKS	32

#define SAFLAG_ZERO	0x0001
#define SAFLAG_PASSIVE	0x0002
#define SAFLAG_MAGS	0x0004

/*
 * Thread control
 */

#define arysize(ary)	(sizeof(ary)/sizeof((ary)[0]))

/*
 * The assertion macros try to pretty-print assertion failures
 * which can be caused by corruption.  If a lock is held, we
 * provide a macro that attempts to release it before asserting
 * in order to prevent (e.g.) a reentrant SIGABRT calling malloc
 * and deadlocking, resulting in the program freezing up.
 */
#define MASSERT(exp)				\
	do { if (__predict_false(!(exp)))	\
	    _mpanic("assertion: %s in %s",	\
		    #exp, __func__);		\
	} while (0)

#define MASSERT_WTHUNLK(exp, unlk)		\
	do { if (__predict_false(!(exp))) {	\
	    unlk;				\
	    _mpanic("assertion: %s in %s",	\
		    #exp, __func__);		\
	  }					\
	} while (0)

/*
 * Magazines, arrange so the structure is roughly 4KB.
 */
#define M_MAX_ROUNDS		(512 - 3)
#define M_MIN_ROUNDS		16
#define M_ZONE_INIT_ROUNDS	64
#define M_ZONE_HYSTERESIS	32

struct magazine {
	SLIST_ENTRY(magazine) nextmagazine;

	int		flags;
	int		capacity;	/* Max rounds in this magazine */
	int		rounds;		/* Current number of free rounds */
	int		unused01;
	void		*objects[M_MAX_ROUNDS];
};

SLIST_HEAD(magazinelist, magazine);

static spinlock_t zone_mag_lock;
static spinlock_t depot_spinlock;
static struct magazine zone_magazine = {
	.flags = 0,
	.capacity = M_ZONE_INIT_ROUNDS,
	.rounds = 0,
};

#define MAGAZINE_FULL(mp)	(mp->rounds == mp->capacity)
#define MAGAZINE_NOTFULL(mp)	(mp->rounds < mp->capacity)
#define MAGAZINE_EMPTY(mp)	(mp->rounds == 0)
#define MAGAZINE_NOTEMPTY(mp)	(mp->rounds != 0)

/*
 * Each thread will have a pair of magazines per size-class (NZONES)
 * The loaded magazine will support immediate allocations, the previous
 * magazine will either be full or empty and can be swapped at need
 */
typedef struct magazine_pair {
	struct magazine	*loaded;
	struct magazine	*prev;
} magazine_pair;

/* A depot is a collection of magazines for a single zone. */
typedef struct magazine_depot {
	struct magazinelist full;
	struct magazinelist empty;
	spinlock_t	lock;
} magazine_depot;

typedef struct thr_mags {
	magazine_pair	mags[NZONES];
	struct magazine	*newmag;
	int		init;
} thr_mags;

static __thread thr_mags thread_mags TLS_ATTRIBUTE;
static pthread_key_t thread_mags_key;
static pthread_once_t thread_mags_once = PTHREAD_ONCE_INIT;
static magazine_depot depots[NZONES];

/*
 * Fixed globals (not per-cpu)
 */
static const int ZoneSize = ZALLOC_ZONE_SIZE;
static const int ZoneLimit = ZALLOC_ZONE_LIMIT;
static const int ZonePageCount = ZALLOC_ZONE_SIZE / PAGE_SIZE;
static const int ZoneMask = ZALLOC_ZONE_SIZE - 1;

static int opt_madvise = 0;
static int opt_utrace = 0;
static int g_malloc_flags = 0;
static struct slglobaldata SLGlobalData;
static bigalloc_t bigalloc_array[BIGHSIZE];
static spinlock_t bigspin_array[BIGXSIZE];
static volatile void *bigcache_array[BIGCACHE];		/* atomic swap */
static volatile size_t bigcache_size_array[BIGCACHE];	/* SMP races ok */
static volatile int bigcache_index;			/* SMP races ok */
static int malloc_panic;
static size_t excess_alloc;				/* excess big allocs */

static void *_slaballoc(size_t size, int flags);
static void *_slabrealloc(void *ptr, size_t size);
static size_t _slabusablesize(const void *ptr);
static void _slabfree(void *ptr, int, bigalloc_t *);
static int _slabmemalign(void **memptr, size_t alignment, size_t size);
static void *_vmem_alloc(size_t bytes, size_t align, int flags);
static void _vmem_free(void *ptr, size_t bytes);
static void *magazine_alloc(struct magazine *);
static int magazine_free(struct magazine *, void *);
static void *mtmagazine_alloc(int zi, int flags);
static int mtmagazine_free(int zi, void *);
static void mtmagazine_init(void);
static void mtmagazine_destructor(void *);
static slzone_t zone_alloc(int flags);
static void zone_free(void *z);
static void _mpanic(const char *ctl, ...) __printflike(1, 2);
static void malloc_init(void) __constructor(101);

struct nmalloc_utrace {
	void *p;
	size_t s;
	void *r;
};

#define UTRACE(a, b, c)						\
	if (opt_utrace) {					\
		struct nmalloc_utrace ut = {			\
			.p = (a),				\
			.s = (b),				\
			.r = (c)				\
		};						\
		utrace(&ut, sizeof(ut));			\
	}

static void
malloc_init(void)
{
	const char *p = NULL;

	if (issetugid() == 0)
		p = getenv("MALLOC_OPTIONS");

	for (; p != NULL && *p != '\0'; p++) {
		switch(*p) {
		case 'u':	opt_utrace = 0; break;
		case 'U':	opt_utrace = 1; break;
		case 'h':	opt_madvise = 0; break;
		case 'H':	opt_madvise = 1; break;
		case 'z':	g_malloc_flags = 0; break;
		case 'Z':	g_malloc_flags = SAFLAG_ZERO; break;
		default:
			break;
		}
	}

	UTRACE((void *) -1, 0, NULL);
}

/*
 * We have to install a handler for nmalloc thread teardowns when
 * the thread is created.  We cannot delay this because destructors in
 * sophisticated userland programs can call malloc() for the first time
 * during their thread exit.
 *
 * This routine is called directly from pthreads.
 */
void
_nmalloc_thr_init(void)
{
	thr_mags *tp;

	/*
	 * Disallow mtmagazine operations until the mtmagazine is
	 * initialized.
	 */
	tp = &thread_mags;
	tp->init = -1;

	_pthread_once(&thread_mags_once, mtmagazine_init);
	_pthread_setspecific(thread_mags_key, tp);
	tp->init = 1;
}

void
_nmalloc_thr_prepfork(void)
{
	if (__isthreaded) {
		_SPINLOCK(&zone_mag_lock);
		_SPINLOCK(&depot_spinlock);
	}
}

void
_nmalloc_thr_parentfork(void)
{
	if (__isthreaded) {
		_SPINUNLOCK(&depot_spinlock);
		_SPINUNLOCK(&zone_mag_lock);
	}
}

void
_nmalloc_thr_childfork(void)
{
	if (__isthreaded) {
		_SPINUNLOCK(&depot_spinlock);
		_SPINUNLOCK(&zone_mag_lock);
	}
}

/*
 * Handle signal reentrancy safely whether we are threaded or not.
 * This improves the stability for mono and will probably improve
 * stability for other high-level languages which are becoming increasingly
 * sophisticated.
 *
 * The sigblockall()/sigunblockall() implementation uses a counter on
 * a per-thread shared user/kernel page, avoids system calls, and is thus
 *  very fast.
 */
static __inline void
nmalloc_sigblockall(void)
{
	sigblockall();
}

static __inline void
nmalloc_sigunblockall(void)
{
	sigunblockall();
}

/*
 * Thread locks.
 */
static __inline void
slgd_lock(slglobaldata_t slgd)
{
	if (__isthreaded)
		_SPINLOCK(&slgd->Spinlock);
}

static __inline void
slgd_unlock(slglobaldata_t slgd)
{
	if (__isthreaded)
		_SPINUNLOCK(&slgd->Spinlock);
}

static __inline void
depot_lock(magazine_depot *dp __unused)
{
	if (__isthreaded)
		_SPINLOCK(&depot_spinlock);
}

static __inline void
depot_unlock(magazine_depot *dp __unused)
{
	if (__isthreaded)
		_SPINUNLOCK(&depot_spinlock);
}

static __inline void
zone_magazine_lock(void)
{
	if (__isthreaded)
		_SPINLOCK(&zone_mag_lock);
}

static __inline void
zone_magazine_unlock(void)
{
	if (__isthreaded)
		_SPINUNLOCK(&zone_mag_lock);
}

static __inline void
swap_mags(magazine_pair *mp)
{
	struct magazine *tmp;
	tmp = mp->loaded;
	mp->loaded = mp->prev;
	mp->prev = tmp;
}

/*
 * bigalloc hashing and locking support.
 *
 * Return an unmasked hash code for the passed pointer.
 */
static __inline int
_bigalloc_hash(const void *ptr)
{
	int hv;

	hv = ((int)(intptr_t)ptr >> PAGE_SHIFT) ^
	      ((int)(intptr_t)ptr >> (PAGE_SHIFT + BIGHSHIFT));

	return(hv);
}

/*
 * Lock the hash chain and return a pointer to its base for the specified
 * address.
 */
static __inline bigalloc_t *
bigalloc_lock(void *ptr)
{
	int hv = _bigalloc_hash(ptr);
	bigalloc_t *bigp;

	bigp = &bigalloc_array[hv & BIGHMASK];
	if (__isthreaded)
		_SPINLOCK(&bigspin_array[hv & BIGXMASK]);
	return(bigp);
}

/*
 * Lock the hash chain and return a pointer to its base for the specified
 * address.
 *
 * BUT, if the hash chain is empty, just return NULL and do not bother
 * to lock anything.
 */
static __inline bigalloc_t *
bigalloc_check_and_lock(const void *ptr)
{
	int hv = _bigalloc_hash(ptr);
	bigalloc_t *bigp;

	bigp = &bigalloc_array[hv & BIGHMASK];
	if (*bigp == NULL)
		return(NULL);
	if (__isthreaded) {
		_SPINLOCK(&bigspin_array[hv & BIGXMASK]);
	}
	return(bigp);
}

static __inline void
bigalloc_unlock(const void *ptr)
{
	int hv;

	if (__isthreaded) {
		hv = _bigalloc_hash(ptr);
		_SPINUNLOCK(&bigspin_array[hv & BIGXMASK]);
	}
}

/*
 * Find a bigcache entry that might work for the allocation.  SMP races are
 * ok here except for the swap (that is, it is ok if bigcache_size_array[i]
 * is wrong or if a NULL or too-small big is returned).
 *
 * Generally speaking it is ok to find a large entry even if the bytes
 * requested are relatively small (but still oversized), because we really
 * don't know *what* the application is going to do with the buffer.
 */
static __inline
bigalloc_t
bigcache_find_alloc(size_t bytes)
{
	bigalloc_t big = NULL;
	size_t test;
	int i;

	for (i = 0; i < BIGCACHE; ++i) {
		test = bigcache_size_array[i];
		if (bytes <= test) {
			bigcache_size_array[i] = 0;
			big = atomic_swap_ptr(&bigcache_array[i], NULL);
			break;
		}
	}
	return big;
}

/*
 * Free a bigcache entry, possibly returning one that the caller really must
 * free.  This is used to cache recent oversized memory blocks.  Only
 * big blocks smaller than BIGCACHE_LIMIT will be cached this way, so try
 * to collect the biggest ones we can that are under the limit.
 */
static __inline
bigalloc_t
bigcache_find_free(bigalloc_t big)
{
	int i;
	int j;
	int b;

	b = ++bigcache_index;
	for (i = 0; i < BIGCACHE; ++i) {
		j = (b + i) & BIGCACHE_MASK;
		if (bigcache_size_array[j] < big->bytes) {
			bigcache_size_array[j] = big->bytes;
			big = atomic_swap_ptr(&bigcache_array[j], big);
			break;
		}
	}
	return big;
}

static __inline
void
handle_excess_big(void)
{
	int i;
	bigalloc_t big;
	bigalloc_t *bigp;

	if (excess_alloc <= BIGCACHE_EXCESS)
		return;

	for (i = 0; i < BIGHSIZE; ++i) {
		bigp = &bigalloc_array[i];
		if (*bigp == NULL)
			continue;
		if (__isthreaded)
			_SPINLOCK(&bigspin_array[i & BIGXMASK]);
		for (big = *bigp; big; big = big->next) {
			if (big->active < big->bytes) {
				MASSERT_WTHUNLK((big->active & PAGE_MASK) == 0,
				    _SPINUNLOCK(&bigspin_array[i & BIGXMASK]));
				MASSERT_WTHUNLK((big->bytes & PAGE_MASK) == 0,
				    _SPINUNLOCK(&bigspin_array[i & BIGXMASK]));
				munmap((char *)big->base + big->active,
				       big->bytes - big->active);
				atomic_add_long(&excess_alloc,
						big->active - big->bytes);
				big->bytes = big->active;
			}
		}
		if (__isthreaded)
			_SPINUNLOCK(&bigspin_array[i & BIGXMASK]);
	}
}

/*
 * Calculate the zone index for the allocation request size and set the
 * allocation request size to that particular zone's chunk size.
 */
static __inline int
zoneindex(size_t *bytes, size_t *chunking)
{
	size_t n = (unsigned int)*bytes;	/* unsigned for shift opt */

	/*
	 * This used to be 8-byte chunks and 16 zones for n < 128.
	 * However some instructions may require 16-byte alignment
	 * (aka SIMD) and programs might not request an aligned size
	 * (aka GCC-7), so change this as follows:
	 *
	 * 0-15 bytes	8-byte alignment in two zones	(0-1)
	 * 16-127 bytes	16-byte alignment in four zones	(3-10)
	 * zone index 2 and 11-15 are currently unused.
	 */
	if (n < 16) {
		*bytes = n = (n + 7) & ~7;
		*chunking = 8;
		return(n / 8 - 1);		/* 8 byte chunks, 2 zones */
		/* zones 0,1, zone 2 is unused */
	}
	if (n < 128) {
		*bytes = n = (n + 15) & ~15;
		*chunking = 16;
		return(n / 16 + 2);		/* 16 byte chunks, 8 zones */
		/* zones 3-10, zones 11-15 unused */
	}
	if (n < 256) {
		*bytes = n = (n + 15) & ~15;
		*chunking = 16;
		return(n / 16 + 7);
	}
	if (n < 8192) {
		if (n < 512) {
			*bytes = n = (n + 31) & ~31;
			*chunking = 32;
			return(n / 32 + 15);
		}
		if (n < 1024) {
			*bytes = n = (n + 63) & ~63;
			*chunking = 64;
			return(n / 64 + 23);
		}
		if (n < 2048) {
			*bytes = n = (n + 127) & ~127;
			*chunking = 128;
			return(n / 128 + 31);
		}
		if (n < 4096) {
			*bytes = n = (n + 255) & ~255;
			*chunking = 256;
			return(n / 256 + 39);
		}
		*bytes = n = (n + 511) & ~511;
		*chunking = 512;
		return(n / 512 + 47);
	}
#if ZALLOC_ZONE_LIMIT > 8192
	if (n < 16384) {
		*bytes = n = (n + 1023) & ~1023;
		*chunking = 1024;
		return(n / 1024 + 55);
	}
#endif
#if ZALLOC_ZONE_LIMIT > 16384
	if (n < 32768) {
		*bytes = n = (n + 2047) & ~2047;
		*chunking = 2048;
		return(n / 2048 + 63);
	}
#endif
	_mpanic("Unexpected byte count %zu", n);
	return(0);
}

/*
 * We want large magazines for small allocations
 */
static __inline int
zonecapacity(int zi)
{
	int cap;

	cap = (NZONES - zi) * (M_MAX_ROUNDS - M_MIN_ROUNDS) / NZONES +
	      M_MIN_ROUNDS;

	return cap;
}

/*
 * malloc() - call internal slab allocator
 */
void *
__malloc(size_t size)
{
	void *ptr;

	nmalloc_sigblockall();
	ptr = _slaballoc(size, 0);
	if (ptr == NULL)
		errno = ENOMEM;
	else
		UTRACE(0, size, ptr);
	nmalloc_sigunblockall();

	return(ptr);
}

#define MUL_NO_OVERFLOW	(1UL << (sizeof(size_t) * 4))

/*
 * calloc() - call internal slab allocator
 */
void *
__calloc(size_t number, size_t size)
{
	void *ptr;

	if ((number >= MUL_NO_OVERFLOW || size >= MUL_NO_OVERFLOW) &&
	     number > 0 && SIZE_MAX / number < size) {
		errno = ENOMEM;
		return(NULL);
	}

	nmalloc_sigblockall();
	ptr = _slaballoc(number * size, SAFLAG_ZERO);
	if (ptr == NULL)
		errno = ENOMEM;
	else
		UTRACE(0, number * size, ptr);
	nmalloc_sigunblockall();

	return(ptr);
}

/*
 * realloc() (SLAB ALLOCATOR)
 *
 * We do not attempt to optimize this routine beyond reusing the same
 * pointer if the new size fits within the chunking of the old pointer's
 * zone.
 */
void *
__realloc(void *ptr, size_t size)
{
	void *ret;

	nmalloc_sigblockall();
	ret = _slabrealloc(ptr, size);
	if (ret == NULL)
		errno = ENOMEM;
	else
		UTRACE(ptr, size, ret);
	nmalloc_sigunblockall();

	return(ret);
}

/*
 * malloc_usable_size() (SLAB ALLOCATOR)
 */
size_t
__malloc_usable_size(const void *ptr)
{
	return _slabusablesize(ptr);
}

/*
 * aligned_alloc()
 *
 * Allocate (size) bytes with a alignment of (alignment).
 */
void *
__aligned_alloc(size_t alignment, size_t size)
{
	void *ptr;
	int rc;

	nmalloc_sigblockall();
	ptr = NULL;
	rc = _slabmemalign(&ptr, alignment, size);
	if (rc)
		errno = rc;
	nmalloc_sigunblockall();

	return (ptr);
}

/*
 * posix_memalign()
 *
 * Allocate (size) bytes with a alignment of (alignment), where (alignment)
 * is a power of 2 >= sizeof(void *).
 */
int
__posix_memalign(void **memptr, size_t alignment, size_t size)
{
	int rc;

	/*
	 * OpenGroup spec issue 6 check
	 */
	if (alignment < sizeof(void *)) {
		*memptr = NULL;
		return(EINVAL);
	}

	nmalloc_sigblockall();
	rc = _slabmemalign(memptr, alignment, size);
	nmalloc_sigunblockall();

	return (rc);
}

/*
 * The slab allocator will allocate on power-of-2 boundaries up to
 * at least PAGE_SIZE.  We use the zoneindex mechanic to find a
 * zone matching the requirements, and _vmem_alloc() otherwise.
 */
static int
_slabmemalign(void **memptr, size_t alignment, size_t size)
{
	bigalloc_t *bigp;
	bigalloc_t big;
	size_t chunking;
	int zi __unused;

	if (alignment < 1) {
		*memptr = NULL;
		return(EINVAL);
	}

	/*
	 * OpenGroup spec issue 6 checks
	 */
	if ((alignment | (alignment - 1)) + 1 != (alignment << 1)) {
		*memptr = NULL;
		return(EINVAL);
	}

	/*
	 * Our zone mechanism guarantees same-sized alignment for any
	 * power-of-2 allocation.  If size is a power-of-2 and reasonable
	 * we can just call _slaballoc() and be done.  We round size up
	 * to the nearest alignment boundary to improve our odds of
	 * it becoming a power-of-2 if it wasn't before.
	 */
	if (size <= alignment)
		size = alignment;
	else
		size = (size + alignment - 1) & ~(size_t)(alignment - 1);

	/*
	 * If we have overflowed above when rounding to the nearest alignment
	 * boundary, just return ENOMEM, size should be == N * sizeof(void *).
	 *
	 * Power-of-2 allocations up to 8KB will be aligned to the allocation
	 * size and _slaballoc() can simply be used.  Please see line 1082
	 * for this special case: 'Align the storage in the zone based on
	 * the chunking' has a special case for powers of 2.
	 */
	if (size == 0)
		return(ENOMEM);

	if (size <= MAX_SLAB_PAGEALIGN &&
	    (size | (size - 1)) + 1 == (size << 1)) {
		*memptr = _slaballoc(size, 0);
		return(*memptr ? 0 : ENOMEM);
	}

	/*
	 * Otherwise locate a zone with a chunking that matches
	 * the requested alignment, within reason.   Consider two cases:
	 *
	 * (1) A 1K allocation on a 32-byte alignment.  The first zoneindex
	 *     we find will be the best fit because the chunking will be
	 *     greater or equal to the alignment.
	 *
	 * (2) A 513 allocation on a 256-byte alignment.  In this case
	 *     the first zoneindex we find will be for 576 byte allocations
	 *     with a chunking of 64, which is not sufficient.  To fix this
	 *     we simply find the nearest power-of-2 >= size and use the
	 *     same side-effect of _slaballoc() which guarantees
	 *     same-alignment on a power-of-2 allocation.
	 */
	if (size < PAGE_SIZE) {
		zi = zoneindex(&size, &chunking);
		if (chunking >= alignment) {
			*memptr = _slaballoc(size, 0);
			return(*memptr ? 0 : ENOMEM);
		}
		if (size >= 1024)
			alignment = 1024;
		if (size >= 16384)
			alignment = 16384;
		while (alignment < size)
			alignment <<= 1;
		*memptr = _slaballoc(alignment, 0);
		return(*memptr ? 0 : ENOMEM);
	}

	/*
	 * If the slab allocator cannot handle it use vmem_alloc().
	 *
	 * Alignment must be adjusted up to at least PAGE_SIZE in this case.
	 */
	if (alignment < PAGE_SIZE)
		alignment = PAGE_SIZE;
	if (size < alignment)
		size = alignment;
	size = (size + PAGE_MASK) & ~(size_t)PAGE_MASK;
	if (alignment == PAGE_SIZE && size <= BIGCACHE_LIMIT) {
		big = bigcache_find_alloc(size);
		if (big && big->bytes < size) {
			_slabfree(big->base, FASTSLABREALLOC, &big);
			big = NULL;
		}
		if (big) {
			*memptr = big->base;
			big->active = size;
			if (big->active < big->bytes) {
				atomic_add_long(&excess_alloc,
						big->bytes - big->active);
			}
			bigp = bigalloc_lock(*memptr);
			big->next = *bigp;
			*bigp = big;
			bigalloc_unlock(*memptr);
			handle_excess_big();
			return(0);
		}
	}
	*memptr = _vmem_alloc(size, alignment, 0);
	if (*memptr == NULL)
		return(ENOMEM);

	big = _slaballoc(sizeof(struct bigalloc), 0);
	if (big == NULL) {
		_vmem_free(*memptr, size);
		*memptr = NULL;
		return(ENOMEM);
	}
	bigp = bigalloc_lock(*memptr);
	big->base = *memptr;
	big->active = size;
	big->bytes = size;		/* no excess */
	big->next = *bigp;
	*bigp = big;
	bigalloc_unlock(*memptr);

	return(0);
}

/*
 * free() (SLAB ALLOCATOR) - do the obvious
 */
void
__free(void *ptr)
{
	UTRACE(ptr, 0, 0);

	nmalloc_sigblockall();
	_slabfree(ptr, 0, NULL);
	nmalloc_sigunblockall();
}

/*
 * _slaballoc()	(SLAB ALLOCATOR)
 *
 *	Allocate memory via the slab allocator.  If the request is too large,
 *	or if it page-aligned beyond a certain size, we fall back to the
 *	KMEM subsystem
 */
static void *
_slaballoc(size_t size, int flags)
{
	slzone_t z;
	slchunk_t chunk;
	slglobaldata_t slgd;
	size_t chunking;
	thr_mags *tp;
	struct magazine *mp;
	int count;
	int zi;
	int off;
	void *obj;

	/*
	 * Handle the degenerate size == 0 case.  Yes, this does happen.
	 * Return a special pointer.  This is to maintain compatibility with
	 * the original malloc implementation.  Certain devices, such as the
	 * adaptec driver, not only allocate 0 bytes, they check for NULL and
	 * also realloc() later on.  Joy.
	 */
	if (size == 0)
		size = 1;

	/* Capture global flags */
	flags |= g_malloc_flags;

	/*
	 * Handle large allocations directly, with a separate bigmem cache.
	 *
	 * The backend allocator is pretty nasty on a SMP system.   Use the
	 * slab allocator for one and two page-sized chunks even though we
	 * lose some efficiency.
	 *
	 * NOTE: Please see _slabmemalign(), which assumes that power-of-2
	 *	 allocations up to an including MAX_SLAB_PAGEALIGN
	 *	 can use _slaballoc() and be aligned to the same.  The
	 *	 zone cache can be used for this case, bigalloc does not
	 *	 have to be used.
	 */
	if (size >= ZoneLimit ||
	    ((size & PAGE_MASK) == 0 && size > MAX_SLAB_PAGEALIGN)) {
		bigalloc_t big;
		bigalloc_t *bigp;

		/*
		 * Page-align and cache-color in case of virtually indexed
		 * physically tagged L1 caches (aka SandyBridge).  No sweat
		 * otherwise, so just do it.
		 *
		 * (don't count as excess).
		 */
		size = (size + PAGE_MASK) & ~(size_t)PAGE_MASK;

		/*
		 * If we have overflowed above when rounding to the page
		 * boundary, something has passed us (size_t)[-PAGE_MASK..-1]
		 * so just return NULL, size at this point should be >= 0.
		 */
		if (size == 0)
			return (NULL);

		/*
		 * Force an additional page offset for 8KB-aligned requests
		 * (i.e. 8KB, 16KB, etc) that helps spread data across the
		 * CPU caches at the cost of some dead space in the memory
		 * map.
		 */
		if ((size & (PAGE_SIZE * 2 - 1)) == 0)
			size += PAGE_SIZE;

		/*
		 * Try to reuse a cached big block to avoid mmap'ing.  If it
		 * turns out not to fit our requirements we throw it away
		 * and allocate normally.
		 */
		big = NULL;
		if (size <= BIGCACHE_LIMIT) {
			big = bigcache_find_alloc(size);
			if (big && big->bytes < size) {
				_slabfree(big->base, FASTSLABREALLOC, &big);
				big = NULL;
			}
		}
		if (big) {
			chunk = big->base;
			if (flags & SAFLAG_ZERO)
				bzero(chunk, size);
		} else {
			chunk = _vmem_alloc(size, PAGE_SIZE, flags);
			if (chunk == NULL)
				return(NULL);

			big = _slaballoc(sizeof(struct bigalloc), 0);
			if (big == NULL) {
				_vmem_free(chunk, size);
				return(NULL);
			}
			big->base = chunk;
			big->bytes = size;
		}
		big->active = size;

		bigp = bigalloc_lock(chunk);
		if (big->active < big->bytes) {
			atomic_add_long(&excess_alloc,
					big->bytes - big->active);
		}
		big->next = *bigp;
		*bigp = big;
		bigalloc_unlock(chunk);
		handle_excess_big();

		return(chunk);
	}

	/* Compute allocation zone; zoneindex will panic on excessive sizes */
	zi = zoneindex(&size, &chunking);
	MASSERT(zi < NZONES);

	obj = mtmagazine_alloc(zi, flags);
	if (obj != NULL) {
		if (flags & SAFLAG_ZERO)
			bzero(obj, size);
		return (obj);
	}

	/*
	 * Attempt to allocate out of an existing global zone.  If all zones
	 * are exhausted pull one off the free list or allocate a new one.
	 */
	slgd = &SLGlobalData;

again:
	if (slgd->ZoneAry[zi] == NULL) {
		z = zone_alloc(flags);
		if (z == NULL)
			goto fail;

		/*
		 * How big is the base structure?
		 */
		off = sizeof(struct slzone);

		/*
		 * Align the storage in the zone based on the chunking.
		 *
		 * Guarantee power-of-2 alignment for power-of-2-sized
		 * chunks.  Otherwise align based on the chunking size
		 * (typically 8 or 16 bytes for small allocations).
		 *
		 * NOTE: Allocations >= ZoneLimit are governed by the
		 * bigalloc code and typically only guarantee page-alignment.
		 *
		 * Set initial conditions for UIndex near the zone header
		 * to reduce unecessary page faults, vs semi-randomization
		 * to improve L1 cache saturation.
		 *
		 * NOTE: Please see _slabmemalign(), which assumes that
		 *	 power-of-2 allocations up to an including
		 *	 MAX_SLAB_PAGEALIGN can use _slaballoc()
		 *	 and be aligned to the same.  The zone cache can be
		 *	 used for this case, bigalloc does not have to be
		 *	 used.
		 *
		 *	 ALL power-of-2 requests that fall through to this
		 *	 code use this rule (conditionals above limit this
		 *	 to <= MAX_SLAB_PAGEALIGN).
		 */
		if ((size | (size - 1)) + 1 == (size << 1))
			off = roundup2(off, size);
		else
			off = roundup2(off, chunking);
		z->z_Magic = ZALLOC_SLAB_MAGIC;
		z->z_ZoneIndex = zi;
		z->z_NMax = (ZoneSize - off) / size;
		z->z_NFree = z->z_NMax;
		z->z_BasePtr = (char *)z + off;
		z->z_UIndex = z->z_UEndIndex = 0;
		z->z_ChunkSize = size;
		z->z_FirstFreePg = ZonePageCount;
		if ((z->z_Flags & SLZF_UNOTZEROD) == 0) {
			flags &= ~SAFLAG_ZERO;	/* already zero'd */
			flags |= SAFLAG_PASSIVE;
		}

		/*
		 * Slide the base index for initial allocations out of the
		 * next zone we create so we do not over-weight the lower
		 * part of the cpu memory caches.
		 */
		slgd_lock(slgd);
		z->z_Next = slgd->ZoneAry[zi];
		slgd->ZoneAry[zi] = z;
	} else {
		slgd_lock(slgd);
		z = slgd->ZoneAry[zi];
		if (z == NULL) {
			slgd_unlock(slgd);
			goto again;
		}
	}

	/*
	 * Ok, we have a zone from which at least one chunk is available.
	 */
	MASSERT_WTHUNLK(z->z_NFree > 0, slgd_unlock(slgd));

	/*
	 * Try to cache <count> chunks, up to CACHE_CHUNKS (32 typ)
	 * to avoid unnecessary global lock contention.
	 */
	tp = &thread_mags;
	mp = tp->mags[zi].loaded;
	count = 0;
	if (mp && tp->init >= 0) {
		count = mp->capacity - mp->rounds;
		if (count >= z->z_NFree)
			count = z->z_NFree - 1;
		if (count > CACHE_CHUNKS)
			count = CACHE_CHUNKS;
	}

	/*
	 * Locate a chunk in a free page.  This attempts to localize
	 * reallocations into earlier pages without us having to sort
	 * the chunk list.  A chunk may still overlap a page boundary.
	 */
	while (z->z_FirstFreePg < ZonePageCount) {
		if ((chunk = z->z_PageAry[z->z_FirstFreePg]) != NULL) {
			if (((uintptr_t)chunk & ZoneMask) == 0) {
				slgd_unlock(slgd);
				_mpanic("assertion: corrupt malloc zone");
			}
			z->z_PageAry[z->z_FirstFreePg] = chunk->c_Next;
			--z->z_NFree;

			if (count == 0)
				goto done;
			mp->objects[mp->rounds++] = chunk;
			--count;
			continue;
		}
		++z->z_FirstFreePg;
	}

	/*
	 * No chunks are available but NFree said we had some memory,
	 * so it must be available in the never-before-used-memory
	 * area governed by UIndex.  The consequences are very
	 * serious if our zone got corrupted so we use an explicit
	 * panic rather then a KASSERT.
	 */
	for (;;) {
		chunk = (slchunk_t)(z->z_BasePtr + z->z_UIndex * size);
		--z->z_NFree;
		if (++z->z_UIndex == z->z_NMax)
			z->z_UIndex = 0;
		if (z->z_UIndex == z->z_UEndIndex) {
			if (z->z_NFree != 0) {
				slgd_unlock(slgd);
				_mpanic("slaballoc: corrupted zone");
			}
		}
		if (count == 0)
			break;
		mp->objects[mp->rounds++] = chunk;
		--count;
	}

	if ((z->z_Flags & SLZF_UNOTZEROD) == 0) {
		flags &= ~SAFLAG_ZERO;
		flags |= SAFLAG_PASSIVE;
	}

done:
	/*
	 * Remove us from the ZoneAry[] when we become empty
	 */
	if (z->z_NFree == 0) {
		slgd->ZoneAry[zi] = z->z_Next;
		z->z_Next = NULL;
	}
	slgd_unlock(slgd);
	if (flags & SAFLAG_ZERO)
		bzero(chunk, size);

	return(chunk);
fail:
	return(NULL);
}

/*
 * Reallocate memory within the chunk
 */
static void *
_slabrealloc(void *ptr, size_t size)
{
	bigalloc_t *bigp;
	void *nptr;
	slzone_t z;
	size_t chunking;

	if (ptr == NULL) {
		return(_slaballoc(size, 0));
	}

	if (size == 0)
		size = 1;

	/*
	 * Handle oversized allocations.
	 */
	if ((bigp = bigalloc_check_and_lock(ptr)) != NULL) {
		bigalloc_t big;
		size_t bigbytes;

		while ((big = *bigp) != NULL) {
			if (big->base == ptr) {
				size = (size + PAGE_MASK) & ~(size_t)PAGE_MASK;
				bigbytes = big->bytes;

				/*
				 * If it already fits determine if it makes
				 * sense to shrink/reallocate.  Try to optimize
				 * programs which stupidly make incremental
				 * reallocations larger or smaller by scaling
				 * the allocation.  Also deal with potential
				 * coloring.
				 */
				if (size >= (bigbytes >> 1) &&
				    size <= bigbytes) {
					if (big->active != size) {
						atomic_add_long(&excess_alloc,
								big->active -
								size);
					}
					big->active = size;
					bigalloc_unlock(ptr);
					return(ptr);
				}

				/*
				 * For large reallocations, allocate more space
				 * than we need to try to avoid excessive
				 * reallocations later on.
				 */
				chunking = size + (size >> 3);
				chunking = (chunking + PAGE_MASK) &
					   ~(size_t)PAGE_MASK;

				/*
				 * Try to allocate adjacently in case the
				 * program is idiotically realloc()ing a
				 * huge memory block just slightly bigger.
				 * (llvm's llc tends to do this a lot).
				 *
				 * (MAP_TRYFIXED forces mmap to fail if there
				 *  is already something at the address).
				 */
				if (chunking > bigbytes) {
					char *addr;
					int errno_save = errno;

					addr = mmap((char *)ptr + bigbytes,
						    chunking - bigbytes,
						    PROT_READ|PROT_WRITE,
						    MAP_PRIVATE|MAP_ANON|
						    MAP_TRYFIXED,
						    -1, 0);
					errno = errno_save;
					if (addr == (char *)ptr + bigbytes) {
						atomic_add_long(&excess_alloc,
								big->active -
								big->bytes +
								chunking -
								size);
						big->bytes = chunking;
						big->active = size;
						bigalloc_unlock(ptr);

						return(ptr);
					}
					MASSERT_WTHUNLK(
						(void *)addr == MAP_FAILED,
						bigalloc_unlock(ptr));
				}

				/*
				 * Failed, unlink big and allocate fresh.
				 * (note that we have to leave (big) intact
				 * in case the slaballoc fails).
				 */
				*bigp = big->next;
				bigalloc_unlock(ptr);
				if ((nptr = _slaballoc(size, 0)) == NULL) {
					/* Relink block */
					bigp = bigalloc_lock(ptr);
					big->next = *bigp;
					*bigp = big;
					bigalloc_unlock(ptr);
					return(NULL);
				}
				if (size > bigbytes)
					size = bigbytes;
				bcopy(ptr, nptr, size);
				atomic_add_long(&excess_alloc, big->active -
							       big->bytes);
				_slabfree(ptr, FASTSLABREALLOC, &big);

				return(nptr);
			}
			bigp = &big->next;
		}
		bigalloc_unlock(ptr);
		handle_excess_big();
	}

	/*
	 * Get the original allocation's zone.  If the new request winds
	 * up using the same chunk size we do not have to do anything.
	 *
	 * NOTE: We don't have to lock the globaldata here, the fields we
	 * access here will not change at least as long as we have control
	 * over the allocation.
	 */
	z = (slzone_t)((uintptr_t)ptr & ~(uintptr_t)ZoneMask);
	MASSERT(z->z_Magic == ZALLOC_SLAB_MAGIC);

	/*
	 * Use zoneindex() to chunk-align the new size, as long as the
	 * new size is not too large.
	 */
	if (size < ZoneLimit) {
		zoneindex(&size, &chunking);
		if (z->z_ChunkSize == size) {
			return(ptr);
		}
	}

	/*
	 * Allocate memory for the new request size and copy as appropriate.
	 */
	if ((nptr = _slaballoc(size, 0)) != NULL) {
		if (size > z->z_ChunkSize)
			size = z->z_ChunkSize;
		bcopy(ptr, nptr, size);
		_slabfree(ptr, 0, NULL);
	}

	return(nptr);
}

/*
 * Returns the usable area of an allocated pointer
 */
static size_t
_slabusablesize(const void *ptr)
{
	size_t size;
	bigalloc_t *bigp;
	slzone_t z;

	if (ptr == NULL)
		return 0;

	/*
	 * Handle oversized allocations.
	 */
	if ((bigp = bigalloc_check_and_lock(ptr)) != NULL) {
		bigalloc_t big;

		while ((big = *bigp) != NULL) {
			const char *base = big->base;

			if ((const char *)ptr >= base &&
			    (const char *)ptr < base + big->bytes)
			{
				size = base + big->bytes - (const char *)ptr;

				bigalloc_unlock(ptr);

				return size;
			}
			bigp = &big->next;
		}
		bigalloc_unlock(ptr);
		handle_excess_big();
	}

	/*
	 * Get the original allocation's zone.  If the new request winds
	 * up using the same chunk size we do not have to do anything.
	 *
	 * NOTE: We don't have to lock the globaldata here, the fields we
	 * access here will not change at least as long as we have control
	 * over the allocation.
	 */
	z = (slzone_t)((uintptr_t)ptr & ~(uintptr_t)ZoneMask);
	MASSERT(z->z_Magic == ZALLOC_SLAB_MAGIC);

	size = z->z_ChunkSize -
	       ((const char *)ptr - (const char *)z->z_BasePtr) %
	       z->z_ChunkSize;
	return size;
}

/*
 * free (SLAB ALLOCATOR)
 *
 * Free a memory block previously allocated by malloc.  Note that we do not
 * attempt to uplodate ks_loosememuse as MP races could prevent us from
 * checking memory limits in malloc.
 *
 * flags:
 *	FASTSLABREALLOC		Fast call from realloc, *rbigp already
 *				unlinked.
 *
 * MPSAFE
 */
static void
_slabfree(void *ptr, int flags, bigalloc_t *rbigp)
{
	slzone_t z;
	slchunk_t chunk;
	bigalloc_t big;
	bigalloc_t *bigp;
	slglobaldata_t slgd;
	size_t size;
	int zi;
	int pgno;

	/* Fast realloc path for big allocations */
	if (flags & FASTSLABREALLOC) {
		big = *rbigp;
		goto fastslabrealloc;
	}

	/*
	 * Handle NULL frees and special 0-byte allocations
	 */
	if (ptr == NULL)
		return;

	/*
	 * Handle oversized allocations.
	 */
	if ((bigp = bigalloc_check_and_lock(ptr)) != NULL) {
		while ((big = *bigp) != NULL) {
			if (big->base == ptr) {
				*bigp = big->next;
				atomic_add_long(&excess_alloc, big->active -
							       big->bytes);
				bigalloc_unlock(ptr);

				/*
				 * Try to stash the block we are freeing,
				 * potentially receiving another block in
				 * return which must be freed.
				 */
fastslabrealloc:
				if (big->bytes <= BIGCACHE_LIMIT) {
					big = bigcache_find_free(big);
					if (big == NULL)
						return;
				}
				ptr = big->base;	/* reload */
				size = big->bytes;
				_slabfree(big, 0, NULL);
				_vmem_free(ptr, size);
				return;
			}
			bigp = &big->next;
		}
		bigalloc_unlock(ptr);
		handle_excess_big();
	}

	/*
	 * Zone case.  Figure out the zone based on the fact that it is
	 * ZoneSize aligned.
	 */
	z = (slzone_t)((uintptr_t)ptr & ~(uintptr_t)ZoneMask);
	MASSERT(z->z_Magic == ZALLOC_SLAB_MAGIC);

	size = z->z_ChunkSize;
	zi = z->z_ZoneIndex;

	if (g_malloc_flags & SAFLAG_ZERO)
		bzero(ptr, size);

	if (mtmagazine_free(zi, ptr) == 0)
		return;

	pgno = ((char *)ptr - (char *)z) >> PAGE_SHIFT;
	chunk = ptr;

	/*
	 * Add this free non-zero'd chunk to a linked list for reuse, adjust
	 * z_FirstFreePg.
	 */
	slgd = &SLGlobalData;
	slgd_lock(slgd);

	chunk->c_Next = z->z_PageAry[pgno];
	z->z_PageAry[pgno] = chunk;
	if (z->z_FirstFreePg > pgno)
		z->z_FirstFreePg = pgno;

	/*
	 * Bump the number of free chunks.  If it becomes non-zero the zone
	 * must be added back onto the appropriate list.
	 */
	if (z->z_NFree++ == 0) {
		z->z_Next = slgd->ZoneAry[z->z_ZoneIndex];
		slgd->ZoneAry[z->z_ZoneIndex] = z;
	}

	/*
	 * If the zone becomes totally free we get rid of it.
	 */
	if (z->z_NFree == z->z_NMax) {
		slzone_t *pz;

		pz = &slgd->ZoneAry[z->z_ZoneIndex];
		while (z != *pz)
			pz = &(*pz)->z_Next;
		*pz = z->z_Next;
		z->z_Magic = -1;
		z->z_Next = NULL;
		slgd_unlock(slgd);
		zone_free(z);
	} else {
		slgd_unlock(slgd);
	}
}

/*
 * Allocate and return a magazine.  Return NULL if no magazines are
 * available.
 */
static __inline void *
magazine_alloc(struct magazine *mp)
{
	void *obj;

	if (mp && MAGAZINE_NOTEMPTY(mp)) {
		obj = mp->objects[--mp->rounds];
	} else {
		obj = NULL;
	}
	return (obj);
}

static __inline int
magazine_free(struct magazine *mp, void *p)
{
	if (mp != NULL && MAGAZINE_NOTFULL(mp)) {
		mp->objects[mp->rounds++] = p;
		return 0;
	}

	return -1;
}

static void *
mtmagazine_alloc(int zi, int flags)
{
	thr_mags *tp;
	struct magazine *mp, *emptymag;
	magazine_depot *d;
	void *obj;

	/*
	 * Do not try to access per-thread magazines while the mtmagazine
	 * is being initialized or destroyed.
	 */
	tp = &thread_mags;
	if (tp->init < 0)
		return(NULL);

	/*
	 * Primary per-thread allocation loop
	 */
	for (;;) {
		/*
		 * Make sure we have a magazine available for use.
		 */
		if (tp->newmag == NULL && (flags & SAFLAG_MAGS) == 0) {
			mp = _slaballoc(sizeof(struct magazine),
					SAFLAG_ZERO | SAFLAG_MAGS);
			if (mp == NULL) {
				obj = NULL;
				break;
			}
			if (tp->newmag) {
				_slabfree(mp, 0, NULL);
			} else {
				tp->newmag = mp;
			}
		}

		/*
		 * If the loaded magazine has rounds, allocate and return
		 */
		mp = tp->mags[zi].loaded;
		obj = magazine_alloc(mp);
		if (obj)
			break;

		/*
		 * The prev magazine can only be completely empty or completely
		 * full.  If it is full, swap it with the loaded magazine
		 * and retry.
		 */
		mp = tp->mags[zi].prev;
		if (mp && MAGAZINE_FULL(mp)) {
			MASSERT(mp->rounds != 0);
			swap_mags(&tp->mags[zi]);	/* prev now empty */
			continue;
		}

		/*
		 * If the depot has no loaded magazines ensure that tp->loaded
		 * is not NULL and return NULL.  This will allow _slaballoc()
		 * to cache referals to SLGlobalData in a magazine.
		 */
		d = &depots[zi];
		if (SLIST_EMPTY(&d->full)) {	/* UNLOCKED TEST IS SAFE */
			mp = tp->mags[zi].loaded;
			if (mp == NULL && tp->newmag) {
				mp = tp->newmag;
				tp->newmag = NULL;
				mp->capacity = zonecapacity(zi);
				mp->rounds = 0;
				mp->flags = 0;
				tp->mags[zi].loaded = mp;
			}
			break;
		}

		/*
		 * Cycle: depot(loaded) -> loaded -> prev -> depot(empty)
		 *
		 * If we race and the depot has no full magazines, retry.
		 */
		depot_lock(d);
		mp = SLIST_FIRST(&d->full);
		if (mp) {
			SLIST_REMOVE_HEAD(&d->full, nextmagazine);
			emptymag = tp->mags[zi].prev;
			if (emptymag) {
				SLIST_INSERT_HEAD(&d->empty, emptymag,
						  nextmagazine);
			}
			tp->mags[zi].prev = tp->mags[zi].loaded;
			tp->mags[zi].loaded = mp;
			MASSERT(MAGAZINE_NOTEMPTY(mp));
		}
		depot_unlock(d);
		continue;
	}

	return (obj);
}

static int
mtmagazine_free(int zi, void *ptr)
{
	thr_mags *tp;
	struct magazine *mp, *loadedmag;
	magazine_depot *d;
	int rc = -1;

	/*
	 * Do not try to access per-thread magazines while the mtmagazine
	 * is being initialized or destroyed.
	 */
	tp = &thread_mags;
	if (tp->init < 0)
		return(-1);

	/*
	 * Primary per-thread freeing loop
	 */
	for (;;) {
		/*
		 * Make sure a new magazine is available in case we have
		 * to use it.  Staging the newmag allows us to avoid
		 * some locking/reentrancy complexity.
		 *
		 * Temporarily disable the per-thread caches for this
		 * allocation to avoid reentrancy and/or to avoid a
		 * stack overflow if the [zi] happens to be the same that
		 * would be used to allocate the new magazine.
		 *
		 * WARNING! Calling _slaballoc() can indirectly modify
		 *	    tp->newmag.
		 */
		if (tp->newmag == NULL) {
			mp = _slaballoc(sizeof(struct magazine),
					SAFLAG_ZERO | SAFLAG_MAGS);
			if (tp->newmag && mp)
				_slabfree(mp, 0, NULL);
			else
				tp->newmag = mp;
			if (tp->newmag == NULL) {
				rc = -1;
				break;
			}
		}

		/*
		 * If the loaded magazine has space, free directly to it
		 */
		rc = magazine_free(tp->mags[zi].loaded, ptr);
		if (rc == 0)
			break;

		/*
		 * The prev magazine can only be completely empty or completely
		 * full.  If it is empty, swap it with the loaded magazine
		 * and retry.
		 */
		mp = tp->mags[zi].prev;
		if (mp && MAGAZINE_EMPTY(mp)) {
			MASSERT(mp->rounds == 0);
			swap_mags(&tp->mags[zi]);	/* prev now full */
			continue;
		}

		/*
		 * Try to get an empty magazine from the depot.  Cycle
		 * through depot(empty)->loaded->prev->depot(full).
		 * Retry if an empty magazine was available from the depot.
		 */
		d = &depots[zi];
		depot_lock(d);

		if ((loadedmag = tp->mags[zi].prev) != NULL)
			SLIST_INSERT_HEAD(&d->full, loadedmag, nextmagazine);
		tp->mags[zi].prev = tp->mags[zi].loaded;
		mp = SLIST_FIRST(&d->empty);
		if (mp) {
			tp->mags[zi].loaded = mp;
			SLIST_REMOVE_HEAD(&d->empty, nextmagazine);
			depot_unlock(d);
			MASSERT(MAGAZINE_NOTFULL(mp));
		} else {
			mp = tp->newmag;
			tp->newmag = NULL;
			mp->capacity = zonecapacity(zi);
			mp->rounds = 0;
			mp->flags = 0;
			tp->mags[zi].loaded = mp;
			depot_unlock(d);
		}
	}

	return rc;
}

static void
mtmagazine_init(void)
{
	int error;

	error = _pthread_key_create(&thread_mags_key, mtmagazine_destructor);
	if (error)
		abort();
}

/*
 * This function is only used by the thread exit destructor
 */
static void
mtmagazine_drain(struct magazine *mp)
{
	void *obj;

	nmalloc_sigblockall();
	while (MAGAZINE_NOTEMPTY(mp)) {
		obj = magazine_alloc(mp);
		_slabfree(obj, 0, NULL);
	}
	nmalloc_sigunblockall();
}

/*
 * mtmagazine_destructor()
 *
 * When a thread exits, we reclaim all its resources; all its magazines are
 * drained and the structures are freed.
 *
 * WARNING!  The destructor can be called multiple times if the larger user
 *	     program has its own destructors which run after ours which
 *	     allocate or free memory.
 */
static void
mtmagazine_destructor(void *thrp)
{
	thr_mags *tp = thrp;
	struct magazine *mp;
	int i;

	if (__isexiting)
		return;

	/*
	 * Prevent further use of mtmagazines while we are destructing
	 * them, as well as for any destructors which are run after us
	 * prior to the thread actually being destroyed.
	 */
	tp->init = -1;

	nmalloc_sigblockall();
	for (i = 0; i < NZONES; i++) {
		mp = tp->mags[i].loaded;
		tp->mags[i].loaded = NULL;
		if (mp) {
			if (MAGAZINE_NOTEMPTY(mp))
				mtmagazine_drain(mp);
			_slabfree(mp, 0, NULL);
		}

		mp = tp->mags[i].prev;
		tp->mags[i].prev = NULL;
		if (mp) {
			if (MAGAZINE_NOTEMPTY(mp))
				mtmagazine_drain(mp);
			_slabfree(mp, 0, NULL);
		}
	}
	if (tp->newmag) {
		mp = tp->newmag;
		tp->newmag = NULL;
		_slabfree(mp, 0, NULL);
	}
	nmalloc_sigunblockall();
}

/*
 * zone_alloc()
 *
 * Attempt to allocate a zone from the zone magazine.
 */
static slzone_t
zone_alloc(int flags)
{
	slzone_t z;

	zone_magazine_lock();

	z = magazine_alloc(&zone_magazine);
	if (z == NULL) {
		zone_magazine_unlock();
		z = _vmem_alloc(ZoneSize, ZoneSize, flags);
	} else {
		z->z_Flags |= SLZF_UNOTZEROD;
		zone_magazine_unlock();
	}
	return z;
}

/*
 * Free a zone.
 */
static void
zone_free(void *z)
{
	void *excess[M_ZONE_HYSTERESIS];
	int i;

	zone_magazine_lock();

	bzero(z, sizeof(struct slzone));

	if (opt_madvise)
		madvise(z, ZoneSize, MADV_FREE);

	i = magazine_free(&zone_magazine, z);

	/*
	 * If we failed to free, collect excess magazines; release the zone
	 * magazine lock, and then free to the system via _vmem_free. Re-enable
	 * BURST mode for the magazine.
	 */
	if (i == -1) {
		for (i = 0; i < M_ZONE_HYSTERESIS; ++i) {
			excess[i] = magazine_alloc(&zone_magazine);
			MASSERT_WTHUNLK(excess[i] != NULL,
					zone_magazine_unlock());
		}
		zone_magazine_unlock();

		for (i = 0; i < M_ZONE_HYSTERESIS; ++i)
			_vmem_free(excess[i], ZoneSize);
		_vmem_free(z, ZoneSize);
	} else {
		zone_magazine_unlock();
	}
}

/*
 * _vmem_alloc()
 *
 *	Directly map memory in PAGE_SIZE'd chunks with the specified
 *	alignment.
 *
 *	Alignment must be a multiple of PAGE_SIZE.
 *
 *	Size must be >= alignment.
 */
static void *
_vmem_alloc(size_t size, size_t align, int flags)
{
	static char *addr_hint;
	static int reset_hint = 16;
	char *addr;
	char *save;

	if (--reset_hint <= 0) {
		addr_hint = NULL;
		reset_hint = 16;
	}

	/*
	 * Map anonymous private memory.
	 */
	save = mmap(addr_hint, size, PROT_READ|PROT_WRITE,
		    MAP_PRIVATE|MAP_ANON, -1, 0);
	if (save == MAP_FAILED)
		goto worst_case;
	if (((uintptr_t)save & (align - 1)) == 0)
		return((void *)save);

	addr_hint = (char *)(((size_t)save + (align - 1)) & ~(align - 1));
	munmap(save, size);

	save = mmap(addr_hint, size, PROT_READ|PROT_WRITE,
		    MAP_PRIVATE|MAP_ANON, -1, 0);
	if (save == MAP_FAILED)
		goto worst_case;
	if (((size_t)save & (align - 1)) == 0)
		return((void *)save);
	munmap(save, size);

worst_case:
	save = mmap(NULL, size + align, PROT_READ|PROT_WRITE,
		    MAP_PRIVATE|MAP_ANON, -1, 0);
	if (save == MAP_FAILED)
		return NULL;

	addr = (char *)(((size_t)save + (align - 1)) & ~(align - 1));
	if (save != addr)
		munmap(save, addr - save);
	if (addr + size != save + size + align)
		munmap(addr + size, save + align - addr);

	addr_hint = addr + size;

	return ((void *)addr);
}

/*
 * _vmem_free()
 *
 *	Free a chunk of memory allocated with _vmem_alloc()
 */
static void
_vmem_free(void *ptr, size_t size)
{
	munmap(ptr, size);
}

/*
 * Panic on fatal conditions
 */
static void
_mpanic(const char *ctl, ...)
{
	va_list va;

	if (malloc_panic == 0) {
		malloc_panic = 1;
		va_start(va, ctl);
		vfprintf(stderr, ctl, va);
		fprintf(stderr, "\n");
		fflush(stderr);
		va_end(va);
	}
	abort();
}

__weak_reference(__aligned_alloc, aligned_alloc);
__weak_reference(__malloc, malloc);
__weak_reference(__calloc, calloc);
__weak_reference(__posix_memalign, posix_memalign);
__weak_reference(__realloc, realloc);
__weak_reference(__free, free);
__weak_reference(__malloc_usable_size, malloc_usable_size);
