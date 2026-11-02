#include <cilo/dsl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;
#define CHECK(cond)                                                                                              \
  do {                                                                                                           \
    if (!(cond)) {                                                                                               \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);                                                 \
      failures++;                                                                                                \
    }                                                                                                            \
  } while (0)

typedef struct Big {
  alignas(64) char bytes[200];
} Big;

typedef struct Node {
  int value;
  struct Node *next;
} Node;

static void alignment_and_growth(void) {
  with_arena(a, .min_chunk = 4096) {
    char *c = new(a, char, 'x');
    Big *big = new(a, Big);
    CHECK(*c == 'x');
    CHECK((uintptr_t)big % 64 == 0);
    // a lot more than one chunk, every block intact.
    uint32_t *blocks[512];
    for (uint32_t i = 0; i < 512; i++) {
      blocks[i] = new_array(a, uint32_t, 100);
      for (int j = 0; j < 100; j++) blocks[i][j] = i;
    }
    bool intact = true;
    for (uint32_t i = 0; i < 512; i++) intact &= blocks[i][0] == i && blocks[i][99] == i;
    CHECK(intact);
    CHECK(cilo_arena_stats(a).chunks > 1);
  }
}

static void rewinds_and_zeroing(void) {
  cilo_arena arena = {};
  cilo_mark start = cilo_mark_of(&arena);
  unsigned char *dirty = new_array(&arena, unsigned char, 1000);
  memset(dirty, 0xff, 1000);
  cilo_rewind(start);
  // same memory again, it must come back zeroed even though the arena only tracks a high water mark.
  unsigned char *again = new_array(&arena, unsigned char, 2000);
  CHECK(again == dirty);
  bool zero = true;
  for (int i = 0; i < 2000; i++) zero &= again[i] == 0;
  CHECK(zero);

  // a rewind across chunks keeps them as spares, and a reset reuses them without asking the system.
  rewinding(&arena) {
    for (int i = 0; i < 64; i++) (void)new_array(&arena, char, 32 << 10);
  }
  cilo_stats before = cilo_arena_stats(&arena);
  CHECK(before.spare > 0);
  cilo_arena_reset(&arena);
  for (int i = 0; i < 64; i++) (void)new_array(&arena, char, 32 << 10);
  CHECK(cilo_arena_stats(&arena).reserved == before.reserved);

  // every chunk keeps its own high water mark through a reset, wherever the system mapped it.
  cilo_arena_reset(&arena);
  for (int i = 0; i < 64; i++) memset(new_array(&arena, char, 16 << 10), 0xff, 16 << 10);
  cilo_arena_reset(&arena);
  size_t stale = 0;
  for (int i = 0; i < 64; i++) {
    char *p = new_array(&arena, char, 16 << 10);
    for (int j = 0; j < 16 << 10; j++) stale += p[j] != 0;
  }
  CHECK(stale == 0);
  cilo_arena_drop(&arena);
  CHECK(arena.chunk == nullptr && arena.spare == nullptr);
}

static void stack_first(void) {
  char buf[1024];
  cilo_arena small = arena_on(buf);
  int *x = new(&small, int, 42);
  CHECK((char *)x >= buf && (char *)x < buf + sizeof buf);
  // spills to the system once the buffer is full.
  char *big = new_array(&small, char, 4096);
  CHECK(big < buf || big >= buf + sizeof buf);
  cilo_arena_reset(&small);
  CHECK((char *)new(&small, int) < buf + sizeof buf);
  cilo_arena_drop(&small);
}

static void vectors(void) {
  cilo_arena arena = {.min_chunk = 1 << 20};
  vec(int) xs = {};
  for (int i = 0; i < 1000; i++) push(&arena, &xs, i);
  CHECK(xs.len == 1000 && xs.items[999] == 999);
  // at the top of the arena, growing never moved the items.
  int *before = xs.items;
  for (int i = 0; i < 100000; i++) push(&arena, &xs, i);
  CHECK(xs.items == before);

  // a vec over items it does not own copies them on its first growth.
  int fixed[] = {1, 2, 3};
  vec(int) borrowed = {fixed, 3, 0};
  push(&arena, &borrowed, 4);
  CHECK(borrowed.items != fixed && borrowed.len == 4 && borrowed.items[0] == 1 && borrowed.items[3] == 4);

  vec(Node) nodes = {};
  Node *n = push(&arena, &nodes, .value = 7);
  CHECK(n->value == 7 && n->next == nullptr && nodes.len == 1);
  cilo_arena_drop(&arena);

  // the same macros on the heap through the generic interface.
  vec(double) ds = {};
  for (int i = 0; i < 100; i++) push(&cilo_heap, &ds, i * 0.5);
  CHECK(ds.items[99] == 49.5);
  CILO_FREE(&cilo_heap, ds.items, ds.cap * sizeof *ds.items);
}

static void pools(void) {
  pool(Node) nodes = {};
  Node *a = take(&nodes, .value = 1);
  Node *b = take(&nodes, .value = 2, .next = a);
  CHECK(b->next == a && a->value == 1);
  give(&nodes, a);
  Node *c = take(&nodes, .value = 3);
  CHECK(c == a && c->value == 3 && c->next == nullptr);
  pool_drop(&nodes);
}

static char *greet(cilo_arena *out, const char *name) {
  char *result = nullptr;
  scratch(tmp, out) {
    CHECK(tmp != out);
    char *upper = astrdup(tmp, name);
    for (char *p = upper; *p; p++)
      if (*p >= 'a' && *p <= 'z') *p -= 32;
    result = aprintf(out, "hello %s", upper);
  }
  return result;
}

static void scratch_and_strings(void) {
  scratch(out) {
    char *s = greet(out, "ada");
    CHECK(strcmp(s, "hello ADA") == 0);
    // a long one does not fit the free space and goes through the second pass.
    char *long_one = aprintf(out, "%0*d", 100000, 7);
    CHECK(strlen(long_one) == 100000 && long_one[99999] == '7');
  }
  char *heap = aprintf(&cilo_heap, "%d-%s", 42, "x");
  CHECK(strcmp(heap, "42-x") == 0);
  cilo_mfree(heap);
  cilo_thread_drop();
}

static void *scratch_worker(void *) {
  scratch(tmp) {
    char *s = astrdup(tmp, "dropped at thread exit");
    CHECK(s[0] == 'd');
  }
  return nullptr;
}

static void arena_sources(void) {
  // a zeroed arena starts on a small chunk from the heap, and maps the big ones.
  cilo_arena arena = {};
  (void)new(&arena, int, 1);
  CHECK(arena.chunk->kind == CILO__HEAP);
  CHECK(cilo_arena_stats(&arena).reserved >= 4096 && cilo_arena_stats(&arena).reserved < 8192);
  char *big = new_array(&arena, char, 1 << 20);
  CHECK(arena.chunk->kind == CILO__MAPPED && big[0] == 0 && big[(1 << 20) - 1] == 0);
  // heap chunks are not zero, zeroed allocations clear them.
  cilo_arena_reset(&arena);
  for (int i = 0; i < 100; i++) memset(new_array(&arena, char, 40), 0xff, 40);
  cilo_arena_reset(&arena);
  bool zero = true;
  for (int i = 0; i < 100; i++) {
    char *p = new_array(&arena, char, 40);
    for (int j = 0; j < 40; j++) zero &= p[j] == 0;
  }
  CHECK(zero);
  cilo_arena_drop(&arena);
  CHECK(arena.backing == nullptr);

  // scratch arenas on heap chunks are dropped when their thread exits.
  pthread_t t;
  for (int i = 0; i < 4; i++) {
    pthread_create(&t, nullptr, scratch_worker, nullptr);
    pthread_join(t, nullptr);
  }
}

static void generic_dispatch(void) {
  cilo_arena arena = {};
  cilo_allocator *any = CILO_OF(&arena);
  // through the interface, an arena still hands out consecutive memory.
  char *a = cilo_alloc(any, 8, 8);
  char *b = cilo_alloc(any, 8, 8);
  CHECK(b == a + 8 + CILO__REDZONE);
  cilo_free(any, b, 8);
  CHECK(cilo_alloc(any, 8, 8) == b);
  cilo_arena_drop(&arena);
}

static void heap_basics(void) {
  // every size up to the large ones, each block written and checked, then freed in a scrambled order.
  enum { N = 4000 };
  static unsigned char *blocks[N];
  static size_t sizes[N];
  for (size_t i = 0; i < N; i++) {
    sizes[i] = (i * 2654435761u) % (i % 10 == 0 ? 300000 : 2048);
    blocks[i] = cilo_malloc(sizes[i]);
    CHECK(cilo_msize(blocks[i]) >= sizes[i]);
    memset(blocks[i], (int)(i & 0xff), sizes[i]);
  }
  bool intact = true;
  for (size_t i = 0; i < N; i++)
    for (size_t j = 0; j < sizes[i]; j += 97) intact &= blocks[i][j] == (unsigned char)(i & 0xff);
  CHECK(intact);
  for (size_t i = 0; i < N; i++) cilo_mfree(blocks[(i * 7919) % N]);

  // 16 aligned for sizes that are multiples of 16, and any power of two through memalign.
  bool aligned = true;
  for (size_t size = 16; size <= 4096; size += 16) {
    void *p = cilo_malloc(size);
    aligned &= (uintptr_t)p % 16 == 0;
    cilo_mfree(p);
  }
  CHECK(aligned);
  for (size_t align = 32; align <= (1 << 20); align <<= 1) {
    void *p = cilo_memalign(align, 100);
    CHECK((uintptr_t)p % align == 0);
    memset(p, 1, 100);
    cilo_mfree(p);
  }

  int *grown = cilo_malloc(4 * sizeof(int));
  for (int i = 0; i < 4; i++) grown[i] = i;
  grown = cilo_mrealloc(grown, 100000 * sizeof(int));
  CHECK(grown[3] == 3);
  cilo_mfree(grown);

  unsigned char *zero = cilo_calloc(1 << 20, 1);
  bool zeroed = true;
  for (size_t i = 0; i < 1 << 20; i += 4096) zeroed &= zero[i] == 0;
  CHECK(zeroed);
  cilo_mfree(zero);

  Node *n = new(&cilo_heap, Node, .value = 5);
  CHECK(n->value == 5);
  delete(&cilo_heap, n);
}

#if !CILO_ASAN
// a class freed entirely hands its pages to another class before any segment is mapped, and gets its own back
// with their blocks still threaded.
static void heap_reuse(void) {
  enum { N = 200000, SEGMENTS = 64 };
  static void *blocks[N];
  uintptr_t seen[SEGMENTS];
  size_t count = 0;
  for (size_t i = 0; i < N; i++) {
    blocks[i] = cilo_malloc(48);
    uintptr_t seg = (uintptr_t)blocks[i] & ~(CILO__SEG_SIZE - 1);
    bool known = false;
    for (size_t j = 0; j < count; j++) known |= seen[j] == seg;
    if (!known && count < SEGMENTS) seen[count++] = seg;
  }
  for (size_t i = 0; i < N; i++) cilo_mfree(blocks[i]);
  size_t outside = 0;
  for (size_t i = 0; i < N; i++) {
    blocks[i] = cilo_malloc(40);
    uintptr_t seg = (uintptr_t)blocks[i] & ~(CILO__SEG_SIZE - 1);
    bool known = false;
    for (size_t j = 0; j < count; j++) known |= seen[j] == seg;
    outside += !known;
    memset(blocks[i], 3, 40);
  }
  CHECK(outside == 0);
  for (size_t i = 0; i < N; i++) cilo_mfree(blocks[i]);
  for (size_t i = 0; i < N; i++) blocks[i] = cilo_malloc(48);
  for (size_t i = 0; i < N; i++) cilo_mfree(blocks[i]);
  cilo_heap_trim();
}
#endif

enum { CROSS = 200000 };
static void *cross_blocks[CROSS];

static void *producer(void *) {
  for (size_t i = 0; i < CROSS; i++) {
    cross_blocks[i] = cilo_malloc(16 + i % 200);
    memset(cross_blocks[i], 0xab, 16);
  }
  return nullptr;
}

static void *consumer(void *) {
  for (size_t i = 0; i < CROSS; i++) cilo_mfree(cross_blocks[i]);
  return nullptr;
}

static void *churn(void *arg) {
  uintptr_t seed = (uintptr_t)arg;
  void *live[256] = {};
  for (size_t i = 0; i < 400000; i++) {
    seed = seed * 6364136223846793005u + 1442695040888963407u;
    size_t slot = (seed >> 33) % 256;
    cilo_mfree(live[slot]);
    live[slot] = cilo_malloc((seed >> 40) % 1500);
  }
  for (size_t i = 0; i < 256; i++) cilo_mfree(live[i]);
  return nullptr;
}

// aligned and large blocks freed by a thread that does not own them.
static void *free_all(void *arg) {
  void **blocks = arg;
  for (size_t i = 0; i < 64; i++) cilo_mfree(blocks[i]);
  return nullptr;
}

static void heap_threads(void) {
  void *mixed[64];
  for (size_t i = 0; i < 64; i++) {
    mixed[i] = i % 3 == 0 ? cilo_memalign(256, 300) : i % 3 == 1 ? cilo_malloc(2 << 20) : cilo_malloc(i * 40);
    memset(mixed[i], 7, i % 3 == 1 ? 2 << 20 : 40);
  }
  pthread_t freer;
  pthread_create(&freer, nullptr, free_all, mixed);
  pthread_join(freer, nullptr);
  cilo_heap_trim();

  // blocks freed by another thread, whose owner thread has exited: its heap goes to the next thread.
  for (int round = 0; round < 3; round++) {
    pthread_t t;
    pthread_create(&t, nullptr, producer, nullptr);
    pthread_join(t, nullptr);
    pthread_create(&t, nullptr, consumer, nullptr);
    pthread_join(t, nullptr);
  }
  pthread_t workers[8];
  for (uintptr_t i = 0; i < 8; i++) pthread_create(&workers[i], nullptr, churn, (void *)(i + 1));
  for (int i = 0; i < 8; i++) pthread_join(workers[i], nullptr);
  CHECK(true);
}

int main(void) {
  heap_basics();
#if !CILO_ASAN
  heap_reuse();
#endif
  heap_threads();
  alignment_and_growth();
  rewinds_and_zeroing();
  stack_first();
  vectors();
  pools();
  scratch_and_strings();
  generic_dispatch();
  arena_sources();
  if (failures) return 1;
  puts("ok");
  return 0;
}
