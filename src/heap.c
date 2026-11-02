#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE

#include <cilo/cilo.h>

#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#if CILO_ASAN
#if defined(__APPLE__)
#include <malloc/malloc.h>
#define usable_size malloc_size
#else
#include <malloc.h>
#define usable_size malloc_usable_size
#endif

void *cilo_mrealloc(void *ptr, size_t size) {
  void *p = realloc(ptr, size ? size : 1);
  if (!p) cilo_oom(size);
  return p;
}

void *cilo_memalign(size_t align, size_t size) {
  void *p = nullptr;
  if (posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align, size ? size : 1)) cilo_oom(size);
  return p;
}

size_t cilo_msize(const void *ptr) { return ptr ? usable_size((void *)ptr) : 0; }
void cilo_heap_trim(void) {}
void *cilo__malloc_slow(size_t size) { return cilo_malloc(size); }
void cilo__mfree_slow(cilo__segment *, cilo__page *, void *ptr) { free(ptr); }
void *cilo__calloc_large(size_t size) { return cilo_calloc(1, size); }

#else

// every page holds at least 32 blocks, so pages rarely flip between full and not full.
enum : uint8_t { KIND_SMALL, KIND_MEDIUM, KIND_BIG, KIND_LARGE };

enum : size_t {
  SMALL_SHIFT = 16,
  MEDIUM_SHIFT = 19,
  SMALL_MAX = 1 << 10,
  MEDIUM_MAX = 16 << 10,
  // blocks threaded onto a free list at once. more means fewer slow paths, less means fewer pages touched.
  EXTEND_BYTES = 16 << 10,
  // empty segments always kept, and how long more, or empty pages, are kept before going back to the system.
  SPARE_SEGMENTS = 8,
  SPARE_NS = 50'000'000,
  // remote frees are batched per page in a small hashed set, each batch published with one compare and swap.
  OUTBOX = 64,
  OUTBOX_SLOTS = 64,
  // freed large blocks kept per thread.
  LARGE_CACHE = 64 << 20,
  LARGE_BORROW = 3,
  LARGE_BINS = CILO__BINS - CILO__LARGE_BIN,
  FULL_SCAN = 4,
};

#define LIVE_MASK 0x7fffffffu
#define SLOW_BIT 0x80000000u

typedef struct outbox {
  cilo__page *page;
  cilo__segment *segment;
  cilo__block *head;
  cilo__block *tail;
  uint32_t count;
} outbox;

typedef struct queue {
  cilo__page *first;
  cilo__page *last;
} queue;

struct cilo__heap {
  cilo__heap_head head;
  queue bins[CILO__LARGE_BIN];
  queue full[CILO__LARGE_BIN];
  // pages that were full when another thread freed into them, on a line of its own.
  alignas(64) _Atomic(cilo__page *) remote;
  alignas(64) cilo__segment *segments[3];
  cilo__segment *spare;
  size_t spare_count;
  // the large classes, whose free lists are the cached blocks. the inlined pop counts them up in `used`, the free
  // counts down, so `used` is minus the number cached.
  cilo__page large[LARGE_BINS];
  // the cached bytes as of the last count of each class. pops since then only make it an overestimate.
  size_t large_bytes;
  uint32_t large_counted[LARGE_BINS];
  uint64_t outbox_used;
  outbox outbox[OUTBOX_SLOTS];
  cilo__heap *next_free;
  // empty pages still in their class, at most this many, and when they were last swept.
  size_t empty_hint;
  uint64_t swept_at;
};

static void outbox_flush(cilo__heap *h);

static cilo__page empty_page;

/* the thread's heap. */

#if defined(__APPLE__) && defined(__aarch64__)
_Atomic(size_t) cilo__heap_key;
#else
[[gnu::tls_model("initial-exec")]] thread_local cilo__heap_head *cilo__tls_heap;
#endif

static pthread_key_t heap_key;
static pthread_once_t heap_key_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t heaps_lock = PTHREAD_MUTEX_INITIALIZER;
static cilo__heap *free_heaps;

static void *os_map(size_t size) {
  void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  return p == MAP_FAILED ? nullptr : p;
}

static size_t os_page(void) {
  static size_t page;
  if (!page) page = (size_t)sysconf(_SC_PAGESIZE);
  return page;
}

static uintptr_t align_up(uintptr_t x, uintptr_t a) { return (x + a - 1) & ~(a - 1); }

// a mapping aligned to the segment size. the hint right after the previous segment is usually aligned already,
// which saves the two trims.
static _Atomic(uintptr_t) map_hint;

static void *os_map_segment(size_t size) {
  uintptr_t hint = atomic_load_explicit(&map_hint, memory_order_relaxed);
  if (hint) {
    void *p = mmap((void *)hint, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p != MAP_FAILED) {
      if (((uintptr_t)p & (CILO__SEG_SIZE - 1)) == 0) {
        atomic_store_explicit(&map_hint, (uintptr_t)p + align_up(size, CILO__SEG_SIZE), memory_order_relaxed);
        return p;
      }
      munmap(p, size);
    }
  }
  size_t total = size + CILO__SEG_SIZE;
  char *m = os_map(total);
  if (!m) return nullptr;
  char *start = (char *)align_up((uintptr_t)m, CILO__SEG_SIZE);
  if (start > m) munmap(m, (size_t)(start - m));
  size_t tail = (size_t)(m + total - (start + size));
  if (tail) munmap(start + size, tail);
  atomic_store_explicit(&map_hint, (uintptr_t)start + align_up(size, CILO__SEG_SIZE), memory_order_relaxed);
  return start;
}

static cilo__heap *heap_peek(void) { return (cilo__heap *)cilo__heap_get(); }

static void heap_set(cilo__heap *h) {
  pthread_setspecific(heap_key, h);
#if !(defined(__APPLE__) && defined(__aarch64__))
  cilo__tls_heap = &h->head;
#endif
}

static char *segment_base(const cilo__segment *seg) { return (char *)((uintptr_t)seg & ~(CILO__SEG_SIZE - 1)); }

static void segment_unmap(cilo__segment *seg) { munmap(segment_base(seg), seg->size); }

static void heap_flush(cilo__heap *h) {
  for (cilo__segment *s = h->spare, *next; s; s = next) {
    next = s->next;
    segment_unmap(s);
  }
  for (size_t c = 0; c < LARGE_BINS; c++) {
    for (cilo__block *b = h->large[c].free, *next; b; b = next) {
      next = b->next;
      segment_unmap(cilo__segment_of(b));
    }
    h->large[c].free = nullptr;
    h->large[c].used = 0;
    h->large_counted[c] = 0;
  }
  h->large_bytes = 0;
  h->spare = nullptr;
  h->spare_count = 0;
}

// a thread that exits hands its heap, live pages included, to the next thread that needs one.
static void heap_release(void *arg) {
  cilo__heap *h = arg;
  outbox_flush(h);
  heap_flush(h);
#if !(defined(__APPLE__) && defined(__aarch64__))
  cilo__tls_heap = nullptr;
#endif
  pthread_mutex_lock(&heaps_lock);
  h->next_free = free_heaps;
  free_heaps = h;
  pthread_mutex_unlock(&heaps_lock);
}

static void heap_make_key(void) {
  pthread_key_create(&heap_key, heap_release);
#if defined(__APPLE__) && defined(__aarch64__)
  atomic_store_explicit(&cilo__heap_key, (size_t)heap_key, memory_order_release);
#endif
}

[[gnu::noinline]] static cilo__heap *heap_create(void) {
  cilo__heap *h;
  pthread_once(&heap_key_once, heap_make_key);
  pthread_mutex_lock(&heaps_lock);
  h = free_heaps;
  if (h) free_heaps = h->next_free;
  pthread_mutex_unlock(&heaps_lock);
  if (!h) {
    if (!(h = os_map(align_up(sizeof *h, os_page())))) cilo_oom(sizeof *h);
    for (size_t i = 0; i < CILO__LARGE_BIN; i++) h->head.current[i] = &empty_page;
    for (size_t i = 0; i < LARGE_BINS; i++) h->head.current[CILO__LARGE_BIN + i] = &h->large[i];
  }
  heap_set(h);
  return h;
}

static cilo__heap *heap_acquire(void) {
  cilo__heap *h = heap_peek();
  return CILO_LIKELY(h) ? h : heap_create();
}

/* size classes. */

static size_t bin_size(uint32_t bin) {
  if (bin <= 128) return (size_t)bin * 8;
  uint32_t b = (bin - 73) >> 3, m = (bin - 73) & 7;
  return ((size_t)(9 + m) << (b - 3)) * 8;
}

// the first page of a bin is the current one.
static void sync_current(cilo__heap *h, uint32_t bin) {
  cilo__page *page = h->bins[bin].first ? h->bins[bin].first : &empty_page;
  h->head.current[bin] = page;
  if (bin == 1) h->head.current[0] = page;
}

/* queues. */

static void queue_remove(queue *q, cilo__page *p) {
  if (p->prev) p->prev->next = p->next;
  else q->first = p->next;
  if (p->next) p->next->prev = p->prev;
  else q->last = p->prev;
  p->next = p->prev = nullptr;
}

static void queue_push_front(queue *q, cilo__page *p) {
  p->prev = nullptr;
  p->next = q->first;
  if (q->first) q->first->prev = p;
  else q->last = p;
  q->first = p;
}

static void queue_push_back(queue *q, cilo__page *p) {
  p->next = nullptr;
  p->prev = q->last;
  if (q->last) q->last->next = p;
  else q->first = p;
  q->last = p;
}

/* pages. */

static void page_flags(cilo__page *p) {
  uint32_t used = (uint32_t)p->used & LIVE_MASK;
  if (p->full || p->aligned) used |= SLOW_BIT;
  p->used = (int32_t)used;
}

static uint32_t page_live(const cilo__page *p) { return (uint32_t)p->used & LIVE_MASK; }

static cilo__segment *segment_of(const void *p) { return cilo__segment_of(p); }

// moves the frees of other threads onto the page's own list. the walk pulls in every block another core just
// wrote in one tight loop, measured faster than leaving each miss to the pop that needs it.
static void page_collect(cilo__page *p) {
  if (!atomic_load_explicit(&p->thread_free, memory_order_relaxed)) return;
  cilo__block *list = atomic_exchange_explicit(&p->thread_free, nullptr, memory_order_acquire);
  if (!list) return;
  uint32_t n = 1;
  cilo__block *tail = list;
  while (tail->next) {
    tail = tail->next;
    n++;
  }
  tail->next = p->free;
  p->free = list;
  p->used = (int32_t)((uint32_t)p->used - n);
}

static void page_extend(cilo__page *p) {
  size_t bs = p->block_size;
  uint32_t n = p->reserved - p->capacity;
  if (!n) return;
  uint32_t step = bs * 8 >= EXTEND_BYTES ? 8 : (uint32_t)(EXTEND_BYTES / bs);
  if (n > step) n = step;
  char *first = p->start + (size_t)p->capacity * bs;
  char *last = first + (size_t)(n - 1) * bs;
  for (char *b = first; b < last; b += bs) ((cilo__block *)b)->next = (cilo__block *)(b + bs);
  ((cilo__block *)last)->next = p->free;
  p->free = (cilo__block *)first;
  p->capacity += n;
}

static void page_unfull(cilo__heap *h, cilo__page *p) {
  queue_remove(&h->full[p->bin], p);
  p->full = 0;
  atomic_store_explicit(&p->remote_full, 0, memory_order_relaxed);
  page_flags(p);
  queue_push_back(&h->bins[p->bin], p);
  if (h->bins[p->bin].first == p) sync_current(h, p->bin);
}

static void page_mark_full(cilo__heap *h, cilo__page *p) {
  queue_remove(&h->bins[p->bin], p);
  queue_push_back(&h->full[p->bin], p);
  if (h->head.current[p->bin] == p) sync_current(h, p->bin);
  p->full = 1;
  page_flags(p);
  // pairs with free_remote: a thread that freed just before this store is seen by the collect below.
  atomic_store_explicit(&p->remote_full, 1, memory_order_seq_cst);
  if (atomic_load_explicit(&p->thread_free, memory_order_seq_cst)) {
    page_collect(p);
    page_unfull(h, p);
  }
}

static cilo__segment *segment_new(cilo__heap *h, uint8_t kind) {
  cilo__segment **link = &h->spare, *seg = h->spare;
  while (seg && seg->kind != kind) {
    link = &seg->next;
    seg = seg->next;
  }
  if (seg) {
    *link = seg->next;
    h->spare_count--;
  } else {
    void *base = os_map_segment(CILO__SEG_SIZE);
    if (!base) cilo_oom(CILO__SEG_SIZE);
#if defined(MADV_NOHUGEPAGE)
    // pages of blocks are touched a few KiB at a time. a transparent huge page would make all 2 MiB resident,
    // and was measured slower for small blocks.
    madvise(base, CILO__SEG_SIZE, MADV_NOHUGEPAGE);
#endif
    seg = segment_of(base);
    uint32_t shift = kind == KIND_SMALL ? SMALL_SHIFT : kind == KIND_MEDIUM ? MEDIUM_SHIFT : CILO__SEG_SHIFT;
    *seg = (cilo__segment){.heap = h, .page_shift = shift, .page_count = (uint32_t)(CILO__SEG_SIZE >> shift),
                           .kind = kind, .size = CILO__SEG_SIZE};
  }
  seg->next = h->segments[kind];
  h->segments[kind] = seg;
  return seg;
}

// a coarse clock is enough to age pages and segments, and costs a few nanoseconds.
static uint64_t now_ns(void) {
#if defined(__APPLE__)
  return clock_gettime_nsec_np(CLOCK_UPTIME_RAW_APPROX);
#else
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
  return (uint64_t)ts.tv_sec * 1'000'000'000u + (uint64_t)ts.tv_nsec;
#endif
}

// the newest spares come first. past SPARE_SEGMENTS, the ones idle for SPARE_NS go back to the system, so a
// program freeing and allocating in waves never maps and unmaps between waves.
static void spare_purge(cilo__heap *h, uint64_t now) {
  if (h->spare_count <= SPARE_SEGMENTS) return;
  cilo__segment **link = &h->spare;
  for (size_t i = 0; i < SPARE_SEGMENTS && *link; i++) link = &(*link)->next;
  while (*link) {
    cilo__segment *s = *link;
    if (now - s->retired_at < SPARE_NS) {
      link = &s->next;
      continue;
    }
    *link = s->next;
    h->spare_count--;
    segment_unmap(s);
  }
}

static void segment_retire(cilo__heap *h, cilo__segment *seg) {
  cilo__segment **link = &h->segments[seg->kind];
  while (*link != seg) link = &(*link)->next;
  *link = seg->next;
  uint64_t now = now_ns();
  seg->retired_at = now;
  seg->next = h->spare;
  h->spare = seg;
  h->spare_count++;
  spare_purge(h, now);
}

static cilo__segment *segment_with_room(cilo__heap *h, uint8_t kind) {
  cilo__segment *seg = h->segments[kind];
  while (seg && seg->pages_used == seg->page_count) seg = seg->next;
  return seg;
}

static void heap_sweep(cilo__heap *h, uint64_t now, uint64_t age);

static cilo__page *page_fresh(cilo__heap *h, uint32_t bin) {
  size_t bs = bin_size(bin);
  uint8_t kind = bs <= SMALL_MAX ? KIND_SMALL : bs <= MEDIUM_MAX ? KIND_MEDIUM : KIND_BIG;
  // classes from 4 KiB are multiples of 4 KiB, a line more spreads their first lines over the cache sets.
  if (bs >= 4096) bs += 128;
  cilo__segment *seg = segment_with_room(h, kind);
  // empty pages of other classes go back to the segments before a new one is mapped.
  if (!seg && h->empty_hint) {
    heap_sweep(h, now_ns(), 0);
    seg = segment_with_room(h, kind);
  }
  if (!seg) seg = segment_new(h, kind);
  // a slot that last held this class still has all its blocks threaded, it comes back without a single write.
  uint32_t i = seg->page_count, first = seg->page_count;
  for (uint32_t j = 0; j < seg->page_count && i == seg->page_count; j++) {
    if (seg->pages[j].in_use) continue;
    if (first == seg->page_count) first = j;
    if (seg->pages[j].bin == bin && seg->pages[j].capacity) i = j;
  }
  if (i == seg->page_count) i = first;
  cilo__page *p = &seg->pages[i];
  seg->pages_used++;
  if (p->bin == bin && p->capacity) {
    p->in_use = 1;
    p->next = p->prev = nullptr;
    return p;
  }
  size_t header = align_up(sizeof(cilo__segment) + seg->page_count * sizeof(cilo__page), 128);
  char *base = segment_base(seg) + ((size_t)i << seg->page_shift);
  char *start = i ? base : (char *)seg + header;
  char *end = base + ((size_t)1 << seg->page_shift);
  *p = (cilo__page){.bin = (uint8_t)bin, .in_use = 1, .block_size = bs, .start = start,
                    .reserved = (uint32_t)((size_t)(end - start) / bs)};
  return p;
}

static void drain_remote(cilo__heap *h) {
  cilo__page *p = atomic_exchange_explicit(&h->remote, nullptr, memory_order_acquire);
  while (p) {
    cilo__page *next = p->remote_next;
    atomic_store_explicit(&p->queued, 0, memory_order_seq_cst);
    page_collect(p);
    if (p->full && p->free) page_unfull(h, p);
    p = next;
  }
}

// an empty page leaves its class. its blocks stay threaded, so the class gets it back for free if it comes first.
static void page_retire(cilo__heap *h, cilo__page *p) {
  queue *q = &h->bins[p->bin];
  // a page still listed as remote must leave that list before it is reused. its last remote free was
  // collected, and that thread listed the page before publishing the block, so the drain finds it.
  if (atomic_load_explicit(&p->queued, memory_order_acquire)) drain_remote(h);
  queue_remove(q, p);
  if (h->head.current[p->bin] == p) sync_current(h, p->bin);
  p->in_use = 0;
  p->aligned = 0;
  page_flags(p);
  cilo__segment *seg = segment_of(p);
  if (--seg->pages_used == 0) segment_retire(h, seg);
}

// pages past the first two of their class that have been empty for `age` leave it.
static void heap_sweep(cilo__heap *h, uint64_t now, uint64_t age) {
  h->swept_at = now;
  h->empty_hint = 0;
  for (uint32_t bin = 1; bin < CILO__LARGE_BIN; bin++) {
    cilo__page *p = h->bins[bin].first;
    if (p) p = p->next;
    if (p) p = p->next;
    for (cilo__page *next; p; p = next) {
      next = p->next;
      if (page_live(p)) continue;
      if (now - p->empty_at >= age) page_retire(h, p);
      else h->empty_hint++;
    }
  }
}

// the first two pages of a class stay even when empty, so a loop allocating and freeing a few blocks never
// recycles pages. the others stay too for a while with their blocks threaded, which saves rethreading them all
// when the program allocates the same sizes again.
static void page_empty(cilo__heap *h, cilo__page *p) {
  queue *q = &h->bins[p->bin];
  if (q->first == p || q->first->next == p) return;
  uint64_t now = now_ns();
  p->empty_at = now;
  h->empty_hint++;
  if (now - h->swept_at >= SPARE_NS) heap_sweep(h, now, SPARE_NS);
}

static cilo__page *page_find(cilo__heap *h, uint32_t bin) {
  queue *q = &h->bins[bin];
  for (cilo__page *p = q->first, *next; p; p = next) {
    next = p->next;
    page_collect(p);
    if (!p->free && p->capacity < p->reserved) page_extend(p);
    if (p->free) {
      if (q->first != p) {
        queue_remove(q, p);
        queue_push_front(q, p);
      }
      sync_current(h, bin);
      return p;
    }
    page_mark_full(h, p);
    // marking may have found remote frees and put it back at the end, the loop reaches it again.
    if (!next) next = p->full ? nullptr : p;
  }
  // a remote free racing with the full mark leaves a page nobody announced, a few are checked each time.
  queue *f = &h->full[bin];
  for (size_t i = 0; i < FULL_SCAN && f->first; i++) {
    cilo__page *p = f->first;
    page_collect(p);
    if (p->free) {
      page_unfull(h, p);
      queue_remove(q, p);
      queue_push_front(q, p);
      sync_current(h, bin);
      return p;
    }
    queue_remove(f, p);
    queue_push_back(f, p);
  }
  cilo__page *p = page_fresh(h, bin);
  queue_push_front(q, p);
  page_extend(p);
  sync_current(h, bin);
  return p;
}

/* large blocks: one per mapping, cached per thread instead of unmapped. */

// the block follows the colored header, at its alignment.
static size_t large_header(size_t color, size_t align) {
  return align_up(color + sizeof(cilo__segment) + sizeof(cilo__page), align < 128 ? 128 : align);
}

// the largest header, with the last color.
static const size_t large_room =
    (((CILO__SEG_COLORS - 1) << 7) + sizeof(cilo__segment) + sizeof(cilo__page) + 127) & ~(size_t)127;

// every block of a large class maps the same size, enough for its class size after any colored header.
static size_t large_mapping(uint32_t bin) { return align_up(large_room + bin_size(bin), os_page()); }

static char *large_start(cilo__segment *seg, size_t align) {
  return segment_base(seg) + large_header((size_t)((char *)seg - segment_base(seg)), align);
}

static void large_init(cilo__segment *seg, char *start, uint32_t bin, size_t mapped) {
  *seg = (cilo__segment){.page_shift = CILO__SEG_SHIFT, .page_count = 1, .kind = KIND_LARGE, .size = mapped};
  seg->pages[0] = (cilo__page){.used = 1, .in_use = 1, .bin = (uint8_t)bin, .start = start,
                               .block_size = (size_t)(segment_base(seg) + mapped - start)};
}

[[gnu::noinline]] static void *large_alloc(size_t size, size_t align, bool zero) {
  size_t extra = large_header((CILO__SEG_COLORS - 1) << 7, align) - large_room, need;
  if (extra >= CILO__SEG_SIZE || ckd_add(&need, size, extra)) cilo_oom(size);
  cilo__heap *h = heap_acquire();
  uint32_t bin = need > CILO__INLINE_MAX ? 0 : need > CILO__MEDIUM_MAX ? cilo__bin_of(need) : CILO__LARGE_BIN;
  // an empty class borrows from the next ones up before mapping, the block goes back to its own class later.
  for (uint32_t b = bin; bin && b < CILO__BINS && b <= bin + LARGE_BORROW; b++) {
    cilo__page *cache = &h->large[b - CILO__LARGE_BIN];
    cilo__block *block = cache->free;
    if (!block) continue;
    cache->free = block->next;
    cache->used++;
    cilo__segment *seg = segment_of(block);
    char *start = large_start(seg, align);
    if (start != (char *)block) large_init(seg, start, b, seg->size);
    if (zero) memset(start, 0, size);
    return start;
  }
  size_t mapped;
  if (bin) mapped = large_mapping(bin);
  else if (ckd_add(&mapped, need, large_room) || (mapped = align_up(mapped, os_page())) < need) cilo_oom(size);
  char *base = os_map_segment(mapped);
  if (!base) cilo_oom(size);
  cilo__segment *seg = segment_of(base);
  char *start = large_start(seg, align);
  large_init(seg, start, bin, mapped);
  return start;
}

static uint32_t large_cached(const cilo__page *cache) { return (uint32_t)-cache->used; }

[[gnu::noinline]] static bool large_recount(cilo__heap *h, size_t size) {
  h->large_bytes = 0;
  for (uint32_t j = 0; j < LARGE_BINS; j++) {
    h->large_counted[j] = large_cached(&h->large[j]);
    h->large_bytes += (size_t)h->large_counted[j] * large_mapping(CILO__LARGE_BIN + j);
  }
  return h->large_bytes + size <= LARGE_CACHE;
}

static bool large_room_for(cilo__heap *h, uint32_t i, size_t size) {
  h->large_bytes -= (size_t)(h->large_counted[i] - large_cached(&h->large[i])) * size;
  h->large_counted[i] = large_cached(&h->large[i]);
  return h->large_bytes + size <= LARGE_CACHE || large_recount(h, size);
}

// any thread frees a large block into its own cache, with the standard layout so the inlined pop can use it.
[[gnu::noinline]] static void large_free(cilo__segment *seg) {
  uint32_t bin = seg->pages[0].bin;
  if (!bin) {
    segment_unmap(seg);
    return;
  }
  cilo__heap *h = heap_acquire();
  uint32_t i = bin - CILO__LARGE_BIN;
  cilo__page *cache = &h->large[i];
  if (!large_room_for(h, i, seg->size)) {
    segment_unmap(seg);
    return;
  }
  char *start = large_start(seg, 0);
  if (seg->pages[0].start != start) large_init(seg, start, bin, seg->size);
  cilo__block *b = (cilo__block *)start;
  b->next = cache->free;
  cache->free = b;
  cache->used--;
  h->large_counted[i]++;
  h->large_bytes += seg->size;
}

/* the slow paths. */

[[gnu::noinline]] static void *small_alloc(size_t size) {
  cilo__heap *h = heap_acquire();
  if (h->outbox_used) outbox_flush(h);
  if (atomic_load_explicit(&h->remote, memory_order_relaxed)) drain_remote(h);
  cilo__page *p = page_find(h, cilo__bin_of(size));
  cilo__block *b = p->free;
  p->free = b->next;
  p->used++;
  return b;
}

void *cilo__malloc_slow(size_t size) {
  return size > CILO__MEDIUM_MAX ? large_alloc(size, 0, false) : small_alloc(size);
}

void *cilo__calloc_large(size_t size) { return large_alloc(size, 0, true); }

static cilo__block *block_start(const cilo__page *p, void *ptr) {
  size_t offset = (size_t)((char *)ptr - p->start);
  return (cilo__block *)(p->start + offset - offset % p->block_size);
}

// publishes blocks freed into another thread's page. the announcement comes first, so once the blocks are
// published this thread never touches the page again, and the owner may recycle it right away.
static void publish_remote(cilo__segment *seg, cilo__page *p, cilo__block *first, cilo__block *last) {
  if (atomic_load_explicit(&p->remote_full, memory_order_seq_cst) &&
      !atomic_exchange_explicit(&p->queued, 1, memory_order_acq_rel)) {
    cilo__heap *owner = seg->heap;
    cilo__page *head = atomic_load_explicit(&owner->remote, memory_order_relaxed);
    do p->remote_next = head;
    while (!atomic_compare_exchange_weak_explicit(&owner->remote, &head, p, memory_order_release,
                                                  memory_order_relaxed));
  }
  cilo__block *head = atomic_load_explicit(&p->thread_free, memory_order_relaxed);
  do last->next = head;
  while (!atomic_compare_exchange_weak_explicit(&p->thread_free, &head, first, memory_order_seq_cst,
                                                memory_order_relaxed));
}

static void outbox_send(cilo__heap *h, size_t slot) {
  outbox *o = &h->outbox[slot];
  publish_remote(o->segment, o->page, o->head, o->tail);
  *o = (outbox){};
  h->outbox_used &= ~((uint64_t)1 << slot);
}

static void outbox_flush(cilo__heap *h) {
  while (h->outbox_used) outbox_send(h, (size_t)__builtin_ctzll(h->outbox_used));
}

// frees from one thread into another's pages arrive in runs, a consumer freeing a producer's blocks for
// instance, so they wait in a batch per page and cost one atomic per batch. a batch never mixes pages, which
// keeps it publishable at once.
static void free_remote(cilo__heap *h, cilo__segment *seg, cilo__page *p, cilo__block *b) {
  if (!h) h = heap_acquire();
  size_t slot = (size_t)(((uintptr_t)p * 0x9e3779b97f4a7c15u) >> 58);
  outbox *o = &h->outbox[slot];
  if (o->page != p) {
    if (o->page) outbox_send(h, slot);
    *o = (outbox){.page = p, .segment = seg, .tail = b};
    h->outbox_used |= (uint64_t)1 << slot;
  }
  b->next = o->head;
  o->head = b;
  if (++o->count >= OUTBOX) outbox_send(h, slot);
}

[[gnu::noinline]] static void small_free(cilo__segment *seg, cilo__page *p, void *ptr) {
  cilo__block *b = p->aligned ? block_start(p, ptr) : ptr;
  cilo__heap *h = heap_peek();
  if (seg->heap != h) {
    free_remote(h, seg, p, b);
    return;
  }
  b->next = p->free;
  p->free = b;
  p->used--;
  if (p->full) page_unfull(h, p);
  if (page_live(p) == 0) page_empty(h, p);
}

void cilo__mfree_slow(cilo__segment *seg, cilo__page *p, void *ptr) {
  if (seg->kind == KIND_LARGE) large_free(seg);
  else small_free(seg, p, ptr);
}

/* the rest of the malloc family. */

size_t cilo_msize(const void *ptr) {
  if (!ptr) return 0;
  cilo__segment *seg = segment_of(ptr);
  cilo__page *p = cilo__page_of(seg, ptr);
  if (seg->kind == KIND_LARGE) return (size_t)(p->start + p->block_size - (const char *)ptr);
  const char *start = p->aligned ? (const char *)block_start(p, (void *)ptr) : ptr;
  return (size_t)(start + p->block_size - (const char *)ptr);
}

void *cilo_mrealloc(void *ptr, size_t size) {
  if (!ptr) return cilo_malloc(size);
  size_t usable = cilo_msize(ptr);
  if (size <= usable && size >= usable / 2) return ptr;
  void *p = cilo_malloc(size);
  memcpy(p, ptr, usable < size ? usable : size);
  cilo_mfree(ptr);
  return p;
}

void *cilo_memalign(size_t align, size_t size) {
  if (align & (align - 1)) cilo_oom(size);
  if (align <= 16) return cilo_malloc((size + 15) & ~(size_t)15);
  size_t padded;
  if (ckd_add(&padded, size, align - 1)) cilo_oom(size);
  if (padded > CILO__MEDIUM_MAX) return large_alloc(size, align, false);
  char *raw = cilo_malloc(padded);
  cilo__page *p = cilo__page_of(segment_of(raw), raw);
  // the page now holds interior pointers, so its local frees take the slow path that finds block starts.
  p->aligned = 1;
  page_flags(p);
  return (void *)align_up((uintptr_t)raw, align);
}

void cilo_heap_trim(void) {
  cilo__heap *h = heap_peek();
  if (!h) return;
  outbox_flush(h);
  drain_remote(h);
  for (uint32_t bin = 1; bin < CILO__LARGE_BIN; bin++) {
    for (cilo__page *p = h->bins[bin].first, *next; p; p = next) {
      next = p->next;
      page_collect(p);
      if (!page_live(p) && (h->bins[bin].first != p || p->next)) page_retire(h, p);
    }
  }
  h->empty_hint = 0;
  heap_flush(h);
}

#endif
