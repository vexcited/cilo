#ifndef CILO_H
#define CILO_H

#include <stdatomic.h>
#include <stdckdint.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================================================== */
/* allocators.                                                                                                */
/*                                                                                                            */
/*   cilo_arena arena = {};                          zeroed is ready, the first allocation takes a chunk      */
/*   User *u = CILO_NEW(&arena, User, .id = 1);      typed and initialized, inlined down to a pointer bump    */
/*   int *xs = CILO_NEW_ARRAY(&arena, int, n);       zeroed, overflow checked                                 */
/*   cilo_arena_reset(&arena);                       frees everything at once and keeps the chunks            */
/*                                                                                                            */
/* every macro takes a cilo_arena *, dispatched statically to the inlined fast path, or a cilo_allocator *    */
/* for code generic over allocators. the lowercase syntax lives in cilo/dsl.h.                                */
/* ========================================================================================================== */

#define CILO_LIKELY(x) __builtin_expect(!!(x), 1)
#define CILO_UNLIKELY(x) __builtin_expect(!!(x), 0)


#if defined(__SANITIZE_ADDRESS__)
#define CILO_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CILO_ASAN 1
#endif
#endif
#ifndef CILO_ASAN
#define CILO_ASAN 0
#endif

// under asan, free arena space stays poisoned and every allocation is followed by a poisoned redzone, so
// overflows and uses after a rewind are reported. both vanish from normal builds.
#if CILO_ASAN
void __asan_poison_memory_region(void const volatile *addr, size_t size);
void __asan_unpoison_memory_region(void const volatile *addr, size_t size);
#define CILO__POISON(p, n) __asan_poison_memory_region((p), (n))
#define CILO__UNPOISON(p, n) __asan_unpoison_memory_region((p), (n))
#define CILO__REDZONE ((size_t)16)
#else
#define CILO__POISON(p, n) ((void)(p), (void)(n))
#define CILO__UNPOISON(p, n) ((void)(p), (void)(n))
#define CILO__REDZONE ((size_t)0)
#endif

typedef struct cilo_vtable cilo_vtable;
// an allocator is a pointer to its vtable, embedded as the first member of its state. code generic over
// allocators takes a cilo_allocator *, which points at that member.
typedef const cilo_vtable *cilo_allocator;

struct cilo_vtable {
  // old_size 0 allocates, new_size 0 frees, anything else resizes in place or moves. null means out of memory.
  void *(*resize)(cilo_allocator *self, void *ptr, size_t old_size, size_t new_size, size_t align);
};

// called on out of memory and on size overflows, before aborting. it may exit or longjmp, never return.
extern void (*cilo_oom_hook)(size_t size);
[[noreturn, gnu::cold]] void cilo_oom(size_t size);

/* ========================================================================================================== */
/* arenas.                                                                                                    */
/* ========================================================================================================== */

enum : uint8_t { CILO__MAPPED, CILO__BACKED, CILO__BORROWED, CILO__HEAP };

typedef struct cilo__chunk {
  alignas(16) struct cilo__chunk *prev;
  // bytes, this header included.
  size_t size;
  // everything from here on was never handed out, so it is still zero.
  char *dirty;
  uint8_t kind;
} cilo__chunk;

typedef struct cilo_arena {
  // always null, which is how a cilo_allocator * is recognized as an arena and devirtualized.
  cilo_allocator allocator;
  char *cur;
  char *end;
  char *dirty;
  cilo__chunk *chunk;
  // chunks freed by a rewind or a reset, reused before asking for more.
  cilo__chunk *spare;
  // where chunks come from. null follows cilo_arena_defaults: cilo's heap for chunks under 64 KiB, and zeroed
  // pages from the system, which make zeroing free, for bigger ones.
  cilo_allocator *backing;
  // the first chunk, zero means the default, 4 KiB. the next ones double, up to 64 MiB steps.
  size_t min_chunk;
  // no chunk past the first one, out of memory is fatal.
  bool fixed;
} cilo_arena;

[[gnu::cold, gnu::noinline, gnu::returns_nonnull]] void *cilo__arena_grow(cilo_arena *a, size_t size, size_t align);

// a field read on its own, never paired with its neighbour. the previous allocation just stored `cur`, and a
// load pair reading it with `end` cannot take it from the store buffer: on an apple m4 it waits for the store
// to retire, 2.5 ns per allocation against 1.3. a relaxed atomic load is a plain load that compilers never
// pair, on every target.
#define CILO__LOAD_ALONE(field) __atomic_load_n(&(field), __ATOMIC_RELAXED)

[[gnu::always_inline, gnu::malloc, gnu::returns_nonnull, gnu::alloc_size(2), gnu::alloc_align(3)]]
static inline void *cilo_arena_alloc(cilo_arena *a, size_t size, size_t align) {
  if (CILO_ASAN && align < 8) align = 8;
  uintptr_t end = (uintptr_t)CILO__LOAD_ALONE(a->end);
  uintptr_t cur = (uintptr_t)a->cur;
  size_t pad = (size_t)(-cur & (align - 1));
  size_t avail = (size_t)(end - cur);
  // strict so an empty arena, where cur and end are null, always misses. no sum can overflow.
  if (CILO_LIKELY(pad + CILO__REDZONE < avail && size <= avail - pad - CILO__REDZONE)) {
    char *p = a->cur + pad;
    a->cur = p + size + CILO__REDZONE;
    CILO__UNPOISON(p, size);
    return p;
  }
  return cilo__arena_grow(a, size, align);
}

// only memory that was handed out before is cleared, fresh pages are already zero.
[[gnu::always_inline, gnu::malloc, gnu::returns_nonnull, gnu::alloc_size(2), gnu::alloc_align(3)]]
static inline void *cilo_arena_alloc_zeroed(cilo_arena *a, size_t size, size_t align) {
  char *p = cilo_arena_alloc(a, size, align);
  if ((uintptr_t)p < (uintptr_t)a->dirty) {
    size_t stale = (size_t)(a->dirty - p);
    memset(p, 0, stale < size ? stale : size);
  }
  return p;
}

[[gnu::always_inline, gnu::malloc, gnu::returns_nonnull, gnu::alloc_size(2, 3), gnu::alloc_align(4)]]
static inline void *cilo_arena_alloc_array(cilo_arena *a, size_t n, size_t size, size_t align) {
  size_t bytes;
  if (CILO_UNLIKELY(ckd_mul(&bytes, n, size))) cilo_oom(SIZE_MAX);
  return cilo_arena_alloc_zeroed(a, bytes, align);
}

static inline void cilo__arena_lower(cilo_arena *a, char *to) {
  if ((uintptr_t)a->cur > (uintptr_t)a->dirty) a->dirty = a->cur;
  if (CILO_ASAN && a->chunk && a->chunk->kind != CILO__BORROWED) CILO__POISON(to, (size_t)(a->cur - to));
  a->cur = to;
}

// gives the memory back when it is the last allocation, which makes stack like code free.
static inline void cilo_arena_free(cilo_arena *a, void *ptr, size_t size) {
  if (ptr && (char *)ptr + size + CILO__REDZONE == a->cur) cilo__arena_lower(a, ptr);
}

// grows or shrinks in place when `ptr` is the last allocation, so a growing array at the top never copies.
[[gnu::alloc_size(4)]]
void *cilo_arena_realloc(cilo_arena *a, void *ptr, size_t old_size, size_t new_size, size_t align);

// frees every allocation, keeping the chunks for the next ones.
void cilo_arena_reset(cilo_arena *a);
// returns every chunk. the arena keeps its settings and can be used again.
void cilo_arena_drop(cilo_arena *a);
// returns the chunks kept for reuse.
void cilo_arena_trim(cilo_arena *a);

// how arenas that leave `backing` or `min_chunk` zero get their chunks.
typedef struct cilo_arena_config {
  // the backing they take with their first chunk and keep. null leaves them on the built in sources.
  cilo_allocator *backing;
  // their first chunk, zero means 4 KiB.
  size_t min_chunk;
  // with no backing, chunks this big or bigger are zeroed pages mapped from the system, smaller ones come from
  // cilo's heap. zero means 64 KiB, 1 maps every chunk and SIZE_MAX none.
  size_t map_from;
} cilo_arena_config;

// sets the process wide defaults when `config` is not null, and returns the ones in effect before. arenas read
// them when they need a chunk, so set them before the arenas they should apply to start allocating.
cilo_arena_config cilo_arena_defaults(const cilo_arena_config *config);

// an arena starting on `buf`, a stack array for instance, spilling to the system once it is full.
cilo_arena cilo_arena_on(void *buf, size_t size, bool fixed);
#define CILO__ASSERT_ARRAY(x)                                                                                    \
  sizeof(struct {                                                                                                \
    static_assert(!_Generic(&(x), typeof(*(x)) **: 1, default: 0), "this takes an array, not a pointer");        \
    int ok;                                                                                                      \
  })
#define CILO_ARENA_ON(buf) cilo_arena_on(buf, sizeof(buf) + 0 * CILO__ASSERT_ARRAY(buf), false)
#define CILO_ARENA_FIXED(buf) cilo_arena_on(buf, sizeof(buf) + 0 * CILO__ASSERT_ARRAY(buf), true)

typedef struct cilo_stats {
  size_t reserved;
  size_t chunks;
  size_t spare;
} cilo_stats;
[[gnu::pure]] cilo_stats cilo_arena_stats(const cilo_arena *a);

/* marks: everything allocated after a mark goes away at once. */

typedef struct cilo_mark {
  cilo_arena *arena;
  cilo__chunk *chunk;
  char *cur;
} cilo_mark;

void cilo__rewind_slow(cilo_mark m);

static inline cilo_mark cilo_mark_of(cilo_arena *a) { return (cilo_mark){a, a->chunk, a->cur}; }

static inline void cilo_rewind(cilo_mark m) {
  if (CILO_LIKELY(m.arena->chunk == m.chunk)) {
    if (m.cur) cilo__arena_lower(m.arena, m.cur);
  } else {
    cilo__rewind_slow(m);
  }
}

static inline void cilo__rewind_at(cilo_mark *m) { cilo_rewind(*m); }

/* ========================================================================================================== */
/* any allocator. a null vtable is an arena, so arenas behind a cilo_allocator * still take the inlined path. */
/* ========================================================================================================== */

[[gnu::always_inline, gnu::returns_nonnull, gnu::alloc_size(2), gnu::alloc_align(3)]]
static inline void *cilo_alloc(cilo_allocator *a, size_t size, size_t align) {
  if (CILO_LIKELY(!*a)) return cilo_arena_alloc((cilo_arena *)a, size, align);
  void *p = (*a)->resize(a, nullptr, 0, size ? size : 1, align);
  if (CILO_UNLIKELY(!p)) cilo_oom(size);
  return p;
}

[[gnu::always_inline, gnu::returns_nonnull, gnu::alloc_size(2), gnu::alloc_align(3)]]
static inline void *cilo_alloc_zeroed(cilo_allocator *a, size_t size, size_t align) {
  if (CILO_LIKELY(!*a)) return cilo_arena_alloc_zeroed((cilo_arena *)a, size, align);
  return memset(cilo_alloc(a, size, align), 0, size);
}

[[gnu::always_inline, gnu::returns_nonnull, gnu::alloc_size(2, 3), gnu::alloc_align(4)]]
static inline void *cilo_alloc_array(cilo_allocator *a, size_t n, size_t size, size_t align) {
  size_t bytes;
  if (CILO_UNLIKELY(ckd_mul(&bytes, n, size))) cilo_oom(SIZE_MAX);
  return cilo_alloc_zeroed(a, bytes, align);
}

static inline void *cilo_realloc(cilo_allocator *a, void *ptr, size_t old_size, size_t new_size, size_t align) {
  if (CILO_LIKELY(!*a)) return cilo_arena_realloc((cilo_arena *)a, ptr, old_size, new_size, align);
  void *p = (*a)->resize(a, ptr, old_size, new_size, align);
  if (CILO_UNLIKELY(!p && new_size)) cilo_oom(new_size);
  return p;
}

static inline void cilo_free(cilo_allocator *a, void *ptr, size_t size) {
  if (CILO_LIKELY(!*a)) cilo_arena_free((cilo_arena *)a, ptr, size);
  else if (ptr) (*a)->resize(a, ptr, size, 0, 1);
}

static inline char *cilo_strndup(cilo_allocator *a, const char *s, size_t len) {
  char *copy = memcpy(cilo_alloc(a, len + 1, 1), s, len);
  copy[len] = '\0';
  return copy;
}
static inline char *cilo_strdup(cilo_allocator *a, const char *s) { return cilo_strndup(a, s, strlen(s)); }
// formats straight into the arena's free space when it fits, a single pass with no copy.
[[gnu::format(printf, 2, 3), gnu::returns_nonnull]] char *cilo_printf(cilo_allocator *a, const char *fmt, ...);

/* ========================================================================================================== */
/* the general purpose heap: malloc and free, thread safe, with both fast paths inlined.                      */
/*                                                                                                            */
/* blocks up to 128 KiB live in pages carved out of 4 MiB aligned segments, so a pointer finds its page       */
/* with a mask and a shift. each thread owns a heap of size classes, allocating pops a free list and a local  */
/* free pushes it back. frees from other threads go to an atomic list in the page, collected by the owner.    */
/* bigger blocks get a mapping each, freed ones wait in per thread classes served by the same inlined pop.    */
/* heaps are never unmapped, a thread that exits leaves its heap to the next thread, so a late remote free    */
/* never touches dead memory. under asan, everything goes to the system malloc so asan sees each block.       */
/* ========================================================================================================== */

#define CILO__SEG_SHIFT 22
#define CILO__SEG_SIZE ((uintptr_t)1 << CILO__SEG_SHIFT)
#define CILO__MEDIUM_MAX ((size_t)128 << 10)
// classes past CILO__MEDIUM_MAX are cached large blocks, one per mapping, popped by the same inlined path.
#define CILO__LARGE_BIN 185
#define CILO__BINS 257
#define CILO__INLINE_MAX ((size_t)64 << 20)
// segment headers sit at an offset picked from their address, a cache line out of 64. at the segment boundary
// itself, every header would share one cache set and evict the others.
#define CILO__SEG_COLORS 64

typedef struct cilo__block {
  struct cilo__block *next;
} cilo__block;

typedef struct cilo__heap cilo__heap;

// the first cache line belongs to the owner, the second to the threads freeing into the page, so neither side
// steals the other's line on every operation.
typedef struct cilo__page {
  cilo__block *free;
  // live blocks. pages that need the slow local free, full ones or ones holding aligned blocks, also carry the
  // sign bit, so the local free checks all of it with a single compare.
  int32_t used;
  uint32_t capacity;
  uint32_t reserved;
  uint8_t bin;
  uint8_t full;
  uint8_t in_use;
  size_t block_size;
  char *start;
  struct cilo__page *next;
  struct cilo__page *prev;
  uint64_t empty_at;
  alignas(64) _Atomic(cilo__block *) thread_free;
  _Atomic(uint8_t) remote_full;
  _Atomic(uint8_t) queued;
  uint8_t aligned;
  struct cilo__page *remote_next;
} cilo__page;

typedef struct cilo__segment {
  // the owner, fixed for the segment's whole life. null for large blocks, which any thread frees.
  cilo__heap *heap;
  uint32_t page_shift;
  uint32_t page_count;
  uint32_t pages_used;
  uint8_t kind;
  size_t size;
  uint64_t retired_at;
  struct cilo__segment *next;
  cilo__page pages[];
} cilo__segment;

// the start of every heap, all the inlined paths need. the class of a block up to 1 KiB is its size in words,
// and current[0] mirrors class 1 so a size of zero needs no branch.
typedef struct cilo__heap_head {
  cilo__page *current[CILO__BINS];
} cilo__heap_head;

[[gnu::always_inline, gnu::const]] static inline cilo__segment *cilo__segment_of(const void *ptr) {
  uintptr_t p = (uintptr_t)ptr;
  return (cilo__segment *)((p & ~(CILO__SEG_SIZE - 1)) + (((p >> CILO__SEG_SHIFT) & (CILO__SEG_COLORS - 1)) << 7));
}

[[gnu::always_inline]] static inline cilo__page *cilo__page_of(cilo__segment *seg, const void *ptr) {
  return &seg->pages[((uintptr_t)ptr & (CILO__SEG_SIZE - 1)) >> seg->page_shift];
}

[[gnu::cold, gnu::noinline, gnu::malloc, gnu::returns_nonnull]] void *cilo__malloc_slow(size_t size);
[[gnu::noinline]] void cilo__mfree_slow(cilo__segment *seg, cilo__page *page, void *ptr);
[[gnu::malloc, gnu::returns_nonnull, gnu::alloc_size(1)]] void *cilo__calloc_large(size_t size);

// 8 byte steps up to 1 KiB, then eight classes per doubling, at most 6.25% lost to rounding.
[[gnu::const]] static inline uint32_t cilo__bin_of(size_t size) {
  size_t w = (size + 7) >> 3;
  if (w <= 128) return w <= 1 ? 1 : (uint32_t)w;
  w--;
  uint32_t b = (uint32_t)(63 - __builtin_clzll(w));
  return (b << 3) + (uint32_t)((w >> (b - 3)) & 7) + 73;
}

#if defined(__APPLE__) && defined(__aarch64__)
// pthread keys are direct slots behind tpidrro_el0 on apple silicon, so the heap is two loads away instead of a
// call into the thread local variable thunk.
extern _Atomic(size_t) cilo__heap_key;
[[gnu::always_inline]] static inline cilo__heap_head *cilo__heap_get(void) {
  uintptr_t tp;
  __asm__("mrs %0, tpidrro_el0" : "=r"(tp));
  size_t key = atomic_load_explicit(&cilo__heap_key, memory_order_relaxed);
  return key ? ((cilo__heap_head *const *)(tp & ~(uintptr_t)7))[key] : nullptr;
}
#else
// initial exec, so code built as position independent reads it with one load from the GOT instead of a call.
[[gnu::tls_model("initial-exec")]] extern thread_local cilo__heap_head *cilo__tls_heap;
[[gnu::always_inline]] static inline cilo__heap_head *cilo__heap_get(void) { return cilo__tls_heap; }
#endif

#if CILO_ASAN
[[gnu::malloc, gnu::returns_nonnull, gnu::alloc_size(1)]] static inline void *cilo_malloc(size_t size) {
  void *p = malloc(size ? size : 1);
  if (!p) cilo_oom(size);
  return p;
}
static inline void cilo_mfree(void *ptr) { free(ptr); }
[[gnu::malloc, gnu::returns_nonnull, gnu::alloc_size(1, 2)]] static inline void *cilo_calloc(size_t n, size_t size) {
  void *p = calloc(n ? n : 1, size ? size : 1);
  if (!p) cilo_oom(SIZE_MAX);
  return p;
}
#else
// never null: out of memory goes through cilo_oom.
[[gnu::always_inline, gnu::malloc, gnu::returns_nonnull, gnu::alloc_size(1)]]
static inline void *cilo_malloc(size_t size) {
  cilo__heap_head *h = cilo__heap_get();
  if (CILO_LIKELY(h && size <= CILO__INLINE_MAX)) {
    // a constant size, which is every CILO_NEW, picks its class at compile time. small ones are a shift, the
    // rest take a clz.
    size_t bin = __builtin_constant_p(size) || size > 1024 ? cilo__bin_of(size) : (size + 7) >> 3;
    cilo__page *page = h->current[bin];
    cilo__block *b = page->free;
    if (CILO_LIKELY(b)) {
      page->free = b->next;
      page->used++;
      return b;
    }
  }
  return cilo__malloc_slow(size);
}

[[gnu::always_inline]] static inline void cilo_mfree(void *ptr) {
  if (CILO_UNLIKELY(!ptr)) return;
  cilo__segment *seg = cilo__segment_of(ptr);
  cilo__page *page = cilo__page_of(seg, ptr);
  if (CILO_LIKELY(seg->heap == (cilo__heap *)cilo__heap_get() && page->used > 1)) {
    cilo__block *b = ptr;
    b->next = page->free;
    page->free = b;
    page->used--;
    return;
  }
  cilo__mfree_slow(seg, page, ptr);
}

[[gnu::always_inline, gnu::malloc, gnu::returns_nonnull, gnu::alloc_size(1, 2)]]
static inline void *cilo_calloc(size_t n, size_t size) {
  size_t bytes;
  if (CILO_UNLIKELY(ckd_mul(&bytes, n, size))) cilo_oom(SIZE_MAX);
  // large blocks come from fresh pages, already zero.
  if (bytes > CILO__MEDIUM_MAX) return cilo__calloc_large(bytes);
  return memset(cilo_malloc(bytes), 0, bytes);
}
#endif

[[gnu::returns_nonnull, gnu::alloc_size(2)]] void *cilo_mrealloc(void *ptr, size_t size);
// any power of two alignment.
[[gnu::malloc, gnu::returns_nonnull, gnu::alloc_size(2), gnu::alloc_align(1)]]
void *cilo_memalign(size_t align, size_t size);
// how many bytes the block really holds, at least what was asked.
size_t cilo_msize(const void *ptr);
// returns the calling thread's cached empty segments and large blocks to the system.
void cilo_heap_trim(void);

// the heap as an allocator: CILO_NEW(&cilo_heap, T) inlines down to cilo_malloc with its class folded.
typedef struct cilo_heap_allocator {
  cilo_allocator allocator;
} cilo_heap_allocator;
// const, so code generic over allocators that lto specializes for the heap sees its vtable and drops the arena
// path, where gcc would warn about reading an arena out of the heap's one pointer. it is never written.
extern const cilo_heap_allocator cilo__heap_allocator;
#define cilo_heap (*(cilo_heap_allocator *)&cilo__heap_allocator)

// blocks of 16 bytes or more that are a multiple of 16 are 16 aligned, and every type's size is a multiple of
// its alignment, so rounding up to `align` is all most allocations need.
[[gnu::always_inline, gnu::returns_nonnull]]
static inline void *cilo_heap_alloc(cilo_heap_allocator *, size_t size, size_t align) {
  return align <= 16 ? cilo_malloc((size + align - 1) & ~(align - 1)) : cilo_memalign(align, size);
}
[[gnu::always_inline, gnu::malloc, gnu::returns_nonnull, gnu::alloc_size(2), gnu::alloc_align(3)]]
static inline void *cilo_heap_alloc_zeroed(cilo_heap_allocator *h, size_t size, size_t align) {
  if (align <= 16) return cilo_calloc(1, (size + align - 1) & ~(align - 1));
  return memset(cilo_heap_alloc(h, size, align), 0, size);
}
[[gnu::always_inline, gnu::malloc, gnu::returns_nonnull, gnu::alloc_size(2, 3), gnu::alloc_align(4)]]
static inline void *cilo_heap_alloc_array(cilo_heap_allocator *h, size_t n, size_t size, size_t align) {
  size_t bytes;
  if (CILO_UNLIKELY(ckd_mul(&bytes, n, size))) cilo_oom(SIZE_MAX);
  return cilo_heap_alloc_zeroed(h, bytes, align);
}
static inline void *cilo_heap_realloc(cilo_heap_allocator *h, void *ptr, size_t old_size, size_t new_size,
                                      size_t align) {
  if (!new_size) {
    cilo_mfree(ptr);
    return nullptr;
  }
  if (align <= 16) return cilo_mrealloc(ptr, (new_size + align - 1) & ~(align - 1));
  void *p = cilo_heap_alloc(h, new_size, align);
  if (ptr) {
    memcpy(p, ptr, old_size < new_size ? old_size : new_size);
    cilo_mfree(ptr);
  }
  return p;
}
static inline void cilo_heap_free(cilo_heap_allocator *, void *ptr, size_t) { cilo_mfree(ptr); }

/* ========================================================================================================== */
/* the typed layer.                                                                                           */
/* ========================================================================================================== */

#define CILO__CAT_(a, b) a##b
#define CILO__CAT(a, b) CILO__CAT_(a, b)
#define CILO__UNIQUE(x) CILO__CAT(x, __LINE__)

// the arena and heap functions for their types, inlined, and the generic ones for anything else.
#define CILO__SEL(a, f)                                                                                          \
  _Generic((a), cilo_arena *: cilo_arena_##f, cilo_heap_allocator *: cilo_heap_##f, default: cilo_##f)

// an arena, the heap or a cilo_allocator *, as a cilo_allocator *. anything else does not compile.
#define CILO_OF(a)                                                                                               \
  ((cilo_allocator *)(void *)_Generic((a), cilo_arena *: (a), cilo_heap_allocator *: (a), cilo_allocator *: (a)))

#define CILO_ALLOC(a, size, align) CILO__SEL(a, alloc)((a), size, align)
#define CILO_FREE(a, ptr, size) CILO__SEL(a, free)((a), ptr, size)
// the counterpart of CILO_NEW: CILO_DELETE(&cilo_heap, user).
#define CILO_DELETE(a, ptr) CILO_FREE(a, ptr, sizeof *(ptr))

static inline void *cilo__init(void *dst, const void *src, size_t size) { return memcpy(dst, src, size); }

// CILO_NEW(&arena, User, .id = 1, .name = "ada"). the compound literal fills the fresh memory, optimizers turn
// the copy into plain stores, and missing fields are zero.
#define CILO_NEW(a, T, ...)                                                                                      \
  ((typeof(T) *)cilo__init(CILO_ALLOC(a, sizeof(T), alignof(T)), &(typeof(T)){__VA_ARGS__}, sizeof(T)))
#define CILO_NEW_ARRAY(a, T, n) ((typeof(T) *)CILO__SEL(a, alloc_array)((a), (n), sizeof(T), alignof(T)))

#define CILO_STRDUP(a, s) cilo_strdup(CILO_OF(a), s)
#define CILO_PRINTF(a, ...) cilo_printf(CILO_OF(a), __VA_ARGS__)

/* growable arrays, the same shape as every cima T_list. cap below len means items it does not own, which the */
/* first growth copies.                                                                                       */

#define CILO_VEC(T)                                                                                              \
  struct {                                                                                                       \
    T *items;                                                                                                    \
    size_t len;                                                                                                  \
    size_t cap;                                                                                                  \
  }

[[gnu::cold, gnu::noinline]] void *cilo__vec_grow(cilo_allocator *a, void *items, size_t *cap, size_t len,
                                                  size_t need, size_t size, size_t align);

// appends an item initialized like a compound literal and points at it: CILO_PUSH(&arena, &users, .id = 1).
// the pointer is assigned to a compound literal, and an unused assignment does not warn like a bare pointer.
// arrays at the top of an arena grow in place. `items` and `cap` are read alone, so `len`, which the last push
// stored, is never loaded in a pair.
#define CILO_PUSH(a, v, ...)                                                                                     \
  (CILO_RESERVE(a, v, 1), CILO__LOAD_ALONE((v)->items)[(v)->len] = (typeof(*(v)->items)){__VA_ARGS__},          \
   (typeof((v)->items)){} = &(v)->items[(v)->len++])
#define CILO_RESERVE(a, v, n)                                                                                    \
  ((v)->len + (n) <= CILO__LOAD_ALONE((v)->cap)                                                                  \
       ? (void)0                                                                                                 \
       : (void)((v)->items = cilo__vec_grow(CILO_OF(a), (v)->items, &(v)->cap, (v)->len, (v)->len + (n),         \
                                            sizeof *(v)->items, alignof(typeof(*(v)->items)))))

/* pools of one type: a free list on top of an arena, both pops inlined. zeroed is ready. */

typedef struct cilo_pool {
  void *free;
  cilo_arena slabs;
} cilo_pool;

// the free list link lives in the item and goes through memcpy, so it never breaks strict aliasing.
[[gnu::always_inline, gnu::returns_nonnull]]
static inline void *cilo_pool_take(cilo_pool *p, size_t size, size_t align) {
  void *item = p->free;
  if (CILO_LIKELY(item)) {
    CILO__UNPOISON(item, size);
    memcpy(&p->free, item, sizeof(void *));
    return item;
  }
  return cilo_arena_alloc(&p->slabs, size, align);
}

static inline void cilo_pool_give(cilo_pool *p, void *item, size_t size) {
  memcpy(item, &p->free, sizeof(void *));
  p->free = item;
  CILO__POISON((char *)item + sizeof(void *), size - sizeof(void *));
}

static inline void cilo_pool_drop(cilo_pool *p) {
  p->free = nullptr;
  cilo_arena_drop(&p->slabs);
}

// `of` is never set, it only carries T for the macros.
#define CILO_POOL(T)                                                                                             \
  struct {                                                                                                       \
    cilo_pool raw;                                                                                               \
    T *of;                                                                                                       \
  }
#define CILO__MAX(a, b) ((a) > (b) ? (a) : (b))
#define CILO__ITEM_SIZE(pool) CILO__MAX(sizeof *(pool)->of, sizeof(void *))
#define CILO__ITEM_ALIGN(pool) CILO__MAX(alignof(typeof(*(pool)->of)), alignof(void *))
#define CILO_TAKE(pool, ...)                                                                                     \
  ((typeof((pool)->of))cilo__init(cilo_pool_take(&(pool)->raw, CILO__ITEM_SIZE(pool), CILO__ITEM_ALIGN(pool)),  \
                                  &(typeof(*(pool)->of)){__VA_ARGS__}, sizeof *(pool)->of))
#define CILO_GIVE(pool, item)                                                                                    \
  cilo_pool_give(&(pool)->raw, _Generic((item), typeof((pool)->of): (item)), CILO__ITEM_SIZE(pool))
#define CILO_POOL_DROP(pool) cilo_pool_drop(&(pool)->raw)

/* ========================================================================================================== */
/* scopes.                                                                                                    */
/* ========================================================================================================== */

// CILO_REWINDING(&arena) { ... } frees what the block allocated when it ends, break and return included.
#define CILO_REWINDING(a)                                                                                        \
  for (cilo_mark CILO__UNIQUE(cilo__m) [[gnu::cleanup(cilo__rewind_at)]] = cilo_mark_of(a),                      \
                                       *CILO__UNIQUE(cilo__mp) = &CILO__UNIQUE(cilo__m);                         \
       CILO__UNIQUE(cilo__mp); CILO__UNIQUE(cilo__mp) = nullptr)

// CILO_WITH_ARENA(a, .min_chunk = 1 << 20) { ... } declares `cilo_arena *a`, dropped when the block ends.
#define CILO_WITH_ARENA(name, ...)                                                                               \
  for (cilo_arena CILO__UNIQUE(cilo__a) [[gnu::cleanup(cilo_arena_drop)]] = {__VA_ARGS__},                       \
                                        *name = &CILO__UNIQUE(cilo__a);                                          \
       name; name = nullptr)

// four scratch arenas per thread, so a block may name up to three arenas it must not use. they cost nothing
// until used, and are dropped when their thread exits. a runtime switching tasks binds each task's own set, so
// a block may wait.
#define CILO_SCRATCH_COUNT 4
extern thread_local cilo_arena cilo__scratch_own[CILO_SCRATCH_COUNT];
extern thread_local cilo_arena *cilo__scratch_slots;
// binds `slots`, CILO_SCRATCH_COUNT arenas, and returns the previous set. null goes back to the thread's own.
cilo_arena *cilo_scratch_bind(cilo_arena *slots);
// drops the calling thread's own scratch arenas now, which thread exit does too.
void cilo_thread_drop(void);

static inline cilo_arena *cilo__scratch(cilo_arena *const *conflicts, size_t n) {
  cilo_arena *slots = cilo__scratch_slots ? cilo__scratch_slots : cilo__scratch_own;
  for (size_t s = 0; s < CILO_SCRATCH_COUNT; s++) {
    bool taken = false;
    for (size_t i = 0; i < n; i++) taken |= conflicts[i] == &slots[s];
    if (!taken) return &slots[s];
  }
  cilo_oom(0);
}

// CILO_SCRATCH(tmp) { ... } declares `cilo_arena *tmp`, a thread scratch arena rewound when the block ends.
// a function filling an arena it was given lists it, CILO_SCRATCH(tmp, out), so tmp is never that one.
#define CILO_SCRATCH(name, ...)                                                                                  \
  for (cilo_mark CILO__UNIQUE(cilo__m) [[gnu::cleanup(cilo__rewind_at)]] =                                       \
                     cilo_mark_of(cilo__scratch((cilo_arena *const[]){nullptr __VA_OPT__(, ) __VA_ARGS__},       \
                                                sizeof((cilo_arena *const[]){nullptr __VA_OPT__(, ) __VA_ARGS__}) /  \
                                                    sizeof(cilo_arena *))),                                      \
                 *CILO__UNIQUE(cilo__mp) = &CILO__UNIQUE(cilo__m);                                               \
       CILO__UNIQUE(cilo__mp); CILO__UNIQUE(cilo__mp) = nullptr)                                                 \
    for (cilo_arena *name = CILO__UNIQUE(cilo__m).arena; name; name = nullptr)

#ifdef __cplusplus
}
#endif

#endif
