#define _DEFAULT_SOURCE
#define _DARWIN_C_SOURCE

#include <cilo/cilo.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#include <unistd.h>
#define CILO__PAGES 1
#else
#define CILO__PAGES 0
#endif

enum : size_t {
  MIN_CHUNK = 4 << 10,
  MAP_FROM = 64 << 10,
  MAX_STEP = 64 << 20,
};

void (*cilo_oom_hook)(size_t size);

void cilo_oom(size_t size) {
  if (cilo_oom_hook) cilo_oom_hook(size);
  fprintf(stderr, "cilo: out of memory allocating %zu bytes\n", size);
  abort();
}

static void *heap_resize(cilo_allocator *self, void *ptr, size_t old_size, size_t new_size, size_t align) {
  return cilo_heap_realloc((cilo_heap_allocator *)self, ptr, old_size, new_size, align);
}

static const cilo_vtable heap_vtable = {heap_resize};
cilo_heap_allocator cilo_heap = {&heap_vtable};

/* chunks. */

static char *chunk_data(cilo__chunk *c) { return (char *)(c + 1); }
static char *chunk_end(cilo__chunk *c) { return (char *)c + c->size; }

static cilo__chunk *chunk_new(cilo_arena *a, size_t bytes) {
  cilo__chunk *c;
  if (a->backing) {
    c = cilo_alloc(a->backing, bytes, alignof(cilo__chunk));
    *c = (cilo__chunk){.size = bytes, .kind = CILO__BACKED};
    c->dirty = chunk_end(c);
    return c;
  }
  if (bytes < MAP_FROM) {
    // a mapping costs two system calls and a whole page at least, small chunks are cheaper on the heap. a heap
    // block is 16 aligned from 16 bytes up, and whatever the size class adds is the chunk's too.
    c = cilo_malloc((bytes + 15) & ~(size_t)15);
    *c = (cilo__chunk){.size = cilo_msize(c), .kind = CILO__HEAP};
    c->dirty = chunk_end(c);
    return c;
  }
#if CILO__PAGES
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  bytes = (bytes + page - 1) & ~(page - 1);
  void *m = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (m == MAP_FAILED) return nullptr;
  c = m;
  *c = (cilo__chunk){.size = bytes, .kind = CILO__MAPPED};
#else
  c = calloc(1, bytes);
  if (!c) return nullptr;
  *c = (cilo__chunk){.size = bytes, .kind = CILO__BACKED};
#endif
  c->dirty = chunk_data(c);
  return c;
}

static void chunk_free(cilo_arena *a, cilo__chunk *c) {
  CILO__UNPOISON(c, c->size);
  switch (c->kind) {
  case CILO__MAPPED:
#if CILO__PAGES
    munmap(c, c->size);
#endif
    break;
  case CILO__BACKED:
    if (a->backing) cilo_free(a->backing, c, c->size);
    else free(c);
    break;
  case CILO__HEAP:
    cilo_mfree(c);
    break;
  case CILO__BORROWED:
    break;
  }
}

static void chunk_enter(cilo_arena *a, cilo__chunk *c) {
  a->chunk = c;
  a->cur = chunk_data(c);
  a->end = chunk_end(c);
  a->dirty = c->dirty;
  if (c->kind != CILO__BORROWED) CILO__POISON(a->cur, (size_t)(a->end - a->cur));
}

static void chunk_leave(cilo_arena *a) {
  cilo__chunk *c = a->chunk;
  c->dirty = (uintptr_t)a->cur > (uintptr_t)a->dirty ? a->cur : a->dirty;
}

static pthread_key_t scratch_key;
static pthread_once_t scratch_once = PTHREAD_ONCE_INIT;

static void scratch_exit(void *) { cilo_thread_drop(); }
static void scratch_make_key(void) { pthread_key_create(&scratch_key, scratch_exit); }

// the first chunk of a thread's own scratch arena registers the drop at thread exit, off the hot path.
static void scratch_watch(cilo_arena *a) {
  uintptr_t p = (uintptr_t)a, own = (uintptr_t)cilo__scratch_own;
  if (p < own || p >= own + sizeof cilo__scratch_own) return;
  pthread_once(&scratch_once, scratch_make_key);
  pthread_setspecific(scratch_key, (void *)1);
}

void *cilo__arena_grow(cilo_arena *a, size_t size, size_t align) {
  if (CILO_ASAN && align < 8) align = 8;
  if (!a->chunk && !a->spare) scratch_watch(a);
  size_t need;
  if (ckd_add(&need, size, sizeof(cilo__chunk) + align + CILO__REDZONE) || a->fixed) cilo_oom(size);
  if (a->chunk) chunk_leave(a);

  cilo__chunk **link = &a->spare, *c = a->spare;
  while (c && c->size < need) {
    link = &c->prev;
    c = c->prev;
  }
  if (c) {
    *link = c->prev;
  } else {
    size_t min = a->min_chunk ? a->min_chunk : MIN_CHUNK;
    size_t bytes = a->chunk ? a->chunk->size * 2 : min;
    if (bytes > MAX_STEP) bytes = MAX_STEP;
    if (bytes < min) bytes = min;
    if (bytes < need) bytes = need;
    if (!(c = chunk_new(a, bytes))) cilo_oom(size);
  }
  c->prev = a->chunk;
  chunk_enter(a, c);
  return cilo_arena_alloc(a, size, align);
}

void cilo__rewind_slow(cilo_mark m) {
  cilo_arena *a = m.arena;
  // only the current chunk has its high water mark in the arena, the ones below saved theirs when it was left.
  if (a->chunk && a->chunk != m.chunk) chunk_leave(a);
  while (a->chunk && a->chunk != m.chunk) {
    cilo__chunk *c = a->chunk;
    a->chunk = c->prev;
    if (c->kind == CILO__BORROWED) continue;
    CILO__POISON(chunk_data(c), (size_t)(chunk_end(c) - chunk_data(c)));
    c->prev = a->spare;
    a->spare = c;
  }
  if (!a->chunk) {
    a->cur = a->end = a->dirty = nullptr;
    return;
  }
  a->end = chunk_end(a->chunk);
  a->dirty = a->chunk->dirty;
  a->cur = m.cur;
  if (a->chunk->kind != CILO__BORROWED) CILO__POISON(a->cur, (size_t)(a->end - a->cur));
}

void *cilo_arena_realloc(cilo_arena *a, void *ptr, size_t old_size, size_t new_size, size_t align) {
  if (!ptr) return new_size ? cilo_arena_alloc(a, new_size, align) : nullptr;
  char *p = ptr;
  if (p + old_size + CILO__REDZONE == a->cur) {
    if (new_size <= old_size) {
      cilo__arena_lower(a, p + new_size + CILO__REDZONE);
      return new_size ? p : (cilo__arena_lower(a, p), nullptr);
    }
    if (new_size + CILO__REDZONE <= (size_t)(a->end - p)) {
      a->cur = p + new_size + CILO__REDZONE;
      CILO__UNPOISON(p, new_size);
      return p;
    }
  } else if (new_size <= old_size) {
    return new_size ? p : nullptr;
  }
  void *q = cilo_arena_alloc(a, new_size, align);
  memcpy(q, p, old_size);
  return q;
}

void cilo_arena_reset(cilo_arena *a) {
  cilo__chunk *bottom = a->chunk;
  while (bottom && bottom->prev) bottom = bottom->prev;
  if (bottom && bottom->kind == CILO__BORROWED) cilo_rewind((cilo_mark){a, bottom, chunk_data(bottom)});
  else cilo_rewind((cilo_mark){a, nullptr, nullptr});
}

void cilo_arena_trim(cilo_arena *a) {
  for (cilo__chunk *c = a->spare, *prev; c; c = prev) {
    prev = c->prev;
    chunk_free(a, c);
  }
  a->spare = nullptr;
}

void cilo_arena_drop(cilo_arena *a) {
  cilo_arena_trim(a);
  for (cilo__chunk *c = a->chunk, *prev; c; c = prev) {
    prev = c->prev;
    chunk_free(a, c);
  }
  *a = (cilo_arena){.backing = a->backing, .min_chunk = a->min_chunk};
}

cilo_arena cilo_arena_on(void *buf, size_t size, bool fixed) {
  uintptr_t start = ((uintptr_t)buf + alignof(cilo__chunk) - 1) & ~(uintptr_t)(alignof(cilo__chunk) - 1);
  size_t lost = (size_t)(start - (uintptr_t)buf);
  cilo_arena a = {.fixed = fixed};
  if (size < lost + sizeof(cilo__chunk) + 16) return a;
  cilo__chunk *c = (cilo__chunk *)((char *)buf + lost);
  *c = (cilo__chunk){.size = size - lost, .kind = CILO__BORROWED};
  c->dirty = chunk_end(c);
  chunk_enter(&a, c);
  return a;
}

cilo_stats cilo_arena_stats(const cilo_arena *a) {
  cilo_stats s = {};
  for (cilo__chunk *c = a->chunk; c; c = c->prev) {
    s.reserved += c->size;
    s.chunks++;
  }
  for (cilo__chunk *c = a->spare; c; c = c->prev) {
    s.reserved += c->size;
    s.spare++;
  }
  return s;
}

/* strings. */

char *cilo_printf(cilo_allocator *a, const char *fmt, ...) {
  va_list ap, again;
  va_start(ap, fmt);
  va_copy(again, ap);
  int n;
  cilo_arena *arena = *a ? nullptr : (cilo_arena *)a;
  if (!CILO_ASAN && arena && arena->cur) {
    size_t avail = (size_t)(arena->end - arena->cur);
    n = vsnprintf(arena->cur, avail, fmt, ap);
    if (n >= 0 && (size_t)n < avail) {
      va_end(again);
      va_end(ap);
      return cilo_arena_alloc(arena, (size_t)n + 1, 1);
    }
  } else {
    n = vsnprintf(nullptr, 0, fmt, ap);
  }
  va_end(ap);
  if (n < 0) n = 0;
  char *s = cilo_alloc(a, (size_t)n + 1, 1);
  vsnprintf(s, (size_t)n + 1, fmt, again);
  va_end(again);
  return s;
}

/* vectors. */

void *cilo__vec_grow(cilo_allocator *a, void *items, size_t *cap, size_t len, size_t need, size_t size,
                     size_t align) {
  size_t want = *cap ? *cap * 2 : (size < 64 ? 64 / size : 1);
  if (want < need) want = need;
  size_t bytes;
  if (ckd_mul(&bytes, want, size)) cilo_oom(SIZE_MAX);
  void *grown;
  if (*cap >= len) {
    grown = cilo_realloc(a, items, *cap * size, bytes, align);
  } else {
    // items it does not own, a static array for instance: copied, never resized.
    grown = cilo_alloc(a, bytes, align);
    if (len) memcpy(grown, items, len * size);
  }
  *cap = want;
  return grown;
}

/* scratch. */

thread_local cilo_arena cilo__scratch_own[CILO_SCRATCH_COUNT];
thread_local cilo_arena *cilo__scratch_slots;

cilo_arena *cilo_scratch_bind(cilo_arena *slots) {
  cilo_arena *previous = cilo__scratch_slots;
  cilo__scratch_slots = slots;
  return previous;
}

void cilo_thread_drop(void) {
  for (size_t i = 0; i < CILO_SCRATCH_COUNT; i++) cilo_arena_drop(&cilo__scratch_own[i]);
}
