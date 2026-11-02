#define _DEFAULT_SOURCE
#include <cilo/dsl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#endif

typedef struct Node {
  uint64_t value;
  struct Node *next;
} Node;

enum { N = 1'000'000, ROUNDS = 20 };

static double now_ns(void) {
  struct timespec ts;
  timespec_get(&ts, TIME_UTC);
  return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static volatile uint64_t sink;

#define BENCH(label, ops, ...)                                                                                   \
  do {                                                                                                           \
    double best = 1e300;                                                                                         \
    for (int round = 0; round < ROUNDS; round++) {                                                               \
      double start = now_ns();                                                                                   \
      __VA_ARGS__;                                                                                               \
      double took = now_ns() - start;                                                                            \
      if (took < best) best = took;                                                                              \
    }                                                                                                            \
    printf("%-34s %7.2f ns/op\n", label, best / (ops));                                                          \
  } while (0)

static Node *nodes[N];

// one allocation per call, as code that builds its values through small helpers does.
[[gnu::noinline]] static Node *make_node(cilo_arena *a, uint64_t value) { return new(a, Node, .value = value); }
[[gnu::noinline]] static char *copy_name(cilo_arena *a, const char *name) { return cilo_strndup(CILO_OF(a), name, 16); }
typedef vec(uint32_t) U32s;
[[gnu::noinline]] static void push_one(cilo_arena *a, U32s *v, uint32_t x) { push(a, v, x); }

static size_t resident(void) {
#ifdef __APPLE__
  mach_task_basic_info_data_t info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count);
  return info.resident_size;
#else
  long pages = 0, rss = 0;
  FILE *f = fopen("/proc/self/statm", "r");
  if (f) {
    if (fscanf(f, "%ld %ld", &pages, &rss) != 2) rss = 0;
    fclose(f);
  }
  return (size_t)rss * (size_t)sysconf(_SC_PAGESIZE);
#endif
}

enum { ARENAS = 100'000, FRAMES = 5'000, ALIVE = 20'000 };
static cilo_arena alive[ALIVE];

// arenas that live briefly: a small one holding a few nodes and a name, a frame one filled with 32 KiB.
static void arena_lifetimes(void) {
  BENCH("small arenas, 8 nodes and a name", ARENAS, {
    for (int i = 0; i < ARENAS; i++) {
      cilo_arena a = {};
      for (int j = 0; j < 8; j++) nodes[j] = new(&a, Node, .value = (uint64_t)j);
      sink += (uint8_t)astrdup(&a, "a name of about thirty bytes")[0] + nodes[7]->value;
      cilo_arena_drop(&a);
    }
  });
  BENCH("frame arenas, 32 KiB each", FRAMES, {
    for (int i = 0; i < FRAMES; i++) {
      cilo_arena a = {};
      for (int j = 0; j < 256; j++) sink += *(char *)cilo_arena_alloc(&a, 128, 16);
      cilo_arena_drop(&a);
    }
  });
  size_t before = resident();
  double start = now_ns();
  for (int i = 0; i < ALIVE; i++)
    for (int j = 0; j < 16; j++) new(&alive[i], Node, .value = (uint64_t)j)->next = nodes[0];
  double took = now_ns() - start;
  size_t held = resident() - before;
  for (int i = 0; i < ALIVE; i++) cilo_arena_drop(&alive[i]);
  printf("%-34s %7.2f ns/op %5.0f MiB\n", "20k live arenas, 16 nodes each", took / ALIVE,
         (double)held / (1 << 20));
}

/* maps, against the linear probing map civue and vektra2 each carry: nonzero u64 keys, a splitmix finalizer, */
/* fnv over words for strings, three quarters full at most.                                                   */

typedef struct RefSlot {
  uint64_t key, val;
} RefSlot;
typedef struct RefMap {
  RefSlot *slots;
  uint32_t cap, len;
} RefMap;

static uint64_t ref_mix(uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ull;
  return x ^ (x >> 33);
}

static uint64_t ref_hash(const void *p, size_t n) {
  const uint8_t *s = p;
  uint64_t h = 0xcbf29ce484222325ull;
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t w;
    memcpy(&w, s + i, 8);
    h = (h ^ w) * 0x100000001b3ull;
    h ^= h >> 29;
  }
  for (; i < n; i++) h = (h ^ s[i]) * 0x100000001b3ull;
  return ref_mix(h ^ n);
}

static void ref_rehash(RefMap *m, uint32_t cap) {
  RefSlot *old = m->slots;
  uint32_t n = m->cap;
  m->slots = calloc(cap, sizeof(RefSlot));
  m->cap = cap;
  for (uint32_t i = 0; i < n; i++) {
    if (!old[i].key) continue;
    uint32_t at = (uint32_t)ref_mix(old[i].key) & (cap - 1);
    while (m->slots[at].key) at = (at + 1) & (cap - 1);
    m->slots[at] = old[i];
  }
  free(old);
}

static uint64_t *ref_slot(RefMap *m, uint64_t key) {
  if ((m->len + 1) * 4 > m->cap * 3) ref_rehash(m, m->cap ? m->cap * 2 : 16);
  for (uint32_t i = (uint32_t)ref_mix(key) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
    if (m->slots[i].key == key) return &m->slots[i].val;
    if (!m->slots[i].key) {
      m->slots[i].key = key;
      m->len++;
      return &m->slots[i].val;
    }
  }
}

static uint64_t *ref_get(const RefMap *m, uint64_t key) {
  if (!m->cap) return nullptr;
  for (uint32_t i = (uint32_t)ref_mix(key) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
    if (m->slots[i].key == key) return &m->slots[i].val;
    if (!m->slots[i].key) return nullptr;
  }
}

// vektra2's interning without its lock: the hash finds a chain of strings that share it.
typedef struct Interned {
  struct Interned *next;
  size_t n;
  char s[];
} Interned;

static const char *ref_intern(cilo_arena *a, RefMap *m, const char *s, size_t n) {
  uint64_t *slot = ref_slot(m, ref_hash(s, n) | 1);
  for (Interned *e = (Interned *)*slot; e; e = e->next)
    if (e->n == n && memcmp(e->s, s, n) == 0) return e->s;
  Interned *e = cilo_arena_alloc(a, sizeof(Interned) + n + 1, alignof(Interned));
  *e = (Interned){(Interned *)*slot, n};
  memcpy(e->s, s, n);
  e->s[n] = '\0';
  *slot = (uint64_t)e;
  return e->s;
}

// the table on the heap, the strings in an arena, a string's own copy as the value.
typedef map(const char *, const char *) Interner;

static const char *cilo_intern(cilo_arena *a, Interner *m, const char *s, size_t n) {
  const char **known = map_get_n(m, s, n);
  if (known) return *known;
  char *copy = memcpy(cilo_arena_alloc(a, n + 1, 1), s, n);
  copy[n] = '\0';
  return *map_put(&cilo_heap, m, copy) = copy;
}

enum { KEYS = 1 << 20, NAMES = 100'000 };
static uint64_t keys[KEYS];
static char names[NAMES][24];

static void maps(void) {
  uint64_t seed = 1;
  for (int i = 0; i < KEYS; i++) keys[i] = (seed = seed * 6364136223846793005u + 1442695040888963407u) | 1;
  for (int i = 0; i < NAMES; i++) snprintf(names[i], sizeof names[i], "font-family-%d", i * 7);

  BENCH("map u64 insert 1M, reference", KEYS, {
    RefMap m = {};
    for (int i = 0; i < KEYS; i++) *ref_slot(&m, keys[i]) = (uint64_t)i;
    sink += m.len;
    free(m.slots);
  });
  BENCH("map u64 insert 1M, cilo", KEYS, {
    map(uint64_t, uint64_t) m = {};
    for (int i = 0; i < KEYS; i++) *map_put(&cilo_heap, &m, keys[i]) = (uint64_t)i;
    sink += m.raw.len;
    map_drop(&cilo_heap, &m);
  });

  RefMap ref = {};
  map(uint64_t, uint64_t) mine = {};
  for (int i = 0; i < KEYS; i++) {
    *ref_slot(&ref, keys[i]) = (uint64_t)i;
    *map_put(&cilo_heap, &mine, keys[i]) = (uint64_t)i;
  }
  BENCH("map u64 hit, reference", KEYS, {
    for (int i = 0; i < KEYS; i++) sink += *ref_get(&ref, keys[((unsigned)i * 7919) & (KEYS - 1)]);
  });
  BENCH("map u64 hit, cilo", KEYS, {
    for (int i = 0; i < KEYS; i++) sink += *map_get(&mine, keys[((unsigned)i * 7919) & (KEYS - 1)]);
  });
  BENCH("map u64 miss, reference", KEYS, {
    for (int i = 0; i < KEYS; i++) sink += ref_get(&ref, keys[i] + 2) != nullptr;
  });
  BENCH("map u64 miss, cilo", KEYS, {
    for (int i = 0; i < KEYS; i++) sink += map_has(&mine, keys[i] + 2);
  });
  free(ref.slots);
  map_drop(&cilo_heap, &mine);

  // a map that stays in cache, where the probing itself is what costs.
  ref = (RefMap){};
  for (int i = 0; i < 4096; i++) {
    *ref_slot(&ref, keys[i]) = (uint64_t)i;
    *map_put(&cilo_heap, &mine, keys[i]) = (uint64_t)i;
  }
  BENCH("map u64 hit, 4k keys, reference", KEYS, {
    for (int i = 0; i < KEYS; i++) sink += *ref_get(&ref, keys[((unsigned)i * 7919) & 4095]);
  });
  BENCH("map u64 hit, 4k keys, cilo", KEYS, {
    for (int i = 0; i < KEYS; i++) sink += *map_get(&mine, keys[((unsigned)i * 7919) & 4095]);
  });
  free(ref.slots);
  map_drop(&cilo_heap, &mine);

  cilo_arena strings = {};
  BENCH("intern 100k names 10 times, reference", 10 * NAMES, {
    RefMap m = {};
    for (int r = 0; r < 10; r++)
      for (int i = 0; i < NAMES; i++) sink += (uintptr_t)ref_intern(&strings, &m, names[i], strlen(names[i]));
    free(m.slots);
    cilo_arena_reset(&strings);
  });
  BENCH("intern 100k names 10 times, cilo", 10 * NAMES, {
    Interner m = {};
    for (int r = 0; r < 10; r++)
      for (int i = 0; i < NAMES; i++) sink += (uintptr_t)cilo_intern(&strings, &m, names[i], strlen(names[i]));
    map_drop(&cilo_heap, &m);
    cilo_arena_reset(&strings);
  });
  cilo_arena_drop(&strings);
}

int main(void) {
  maps();
  arena_lifetimes();

  BENCH("malloc + free, 16 B nodes", N, {
    for (int i = 0; i < N; i++) {
      nodes[i] = malloc(sizeof(Node));
      *nodes[i] = (Node){.value = (uint64_t)i};
    }
    for (int i = 0; i < N; i++) {
      sink += nodes[i]->value;
      free(nodes[i]);
    }
  });

  cilo_arena arena = {};
  BENCH("arena new + reset, 16 B nodes", N, {
    for (int i = 0; i < N; i++) nodes[i] = new(&arena, Node, .value = (uint64_t)i);
    for (int i = 0; i < N; i++) sink += nodes[i]->value;
    cilo_arena_reset(&arena);
  });

  cilo_allocator *any = CILO_OF(&arena);
  BENCH("same, through cilo_allocator *", N, {
    for (int i = 0; i < N; i++) nodes[i] = new(any, Node, .value = (uint64_t)i);
    for (int i = 0; i < N; i++) sink += nodes[i]->value;
    cilo_arena_reset(&arena);
  });

  BENCH("arena new, one call each", N, {
    for (int i = 0; i < N; i++) nodes[i] = make_node(&arena, (uint64_t)i);
    cilo_arena_reset(&arena);
  });

  BENCH("arena strndup, 16 B, one call each", N, {
    for (int i = 0; i < N; i++) sink += (uint8_t)copy_name(&arena, "font-family-name")[i & 15];
    cilo_arena_reset(&arena);
  });

  BENCH("malloc + free churn, 64 live", N, {
    Node *live[64] = {};
    for (int i = 0; i < N; i++) {
      Node **slot = &live[i & 63];
      free(*slot);
      *slot = malloc(sizeof(Node));
      (*slot)->value = (uint64_t)i;
    }
    for (int i = 0; i < 64; i++) {
      sink += live[i]->value;
      free(live[i]);
    }
  });

  pool(Node) pool = {};
  BENCH("pool take + give churn, 64 live", N, {
    Node *live[64] = {};
    for (int i = 0; i < N; i++) {
      Node **slot = &live[i & 63];
      if (*slot) give(&pool, *slot);
      *slot = take(&pool, .value = (uint64_t)i);
    }
    for (int i = 0; i < 64; i++) {
      sink += live[i]->value;
      give(&pool, live[i]);
    }
  });

  BENCH("realloc doubling, push 10M u32", 10 * N, {
    uint32_t *items = nullptr;
    size_t len = 0, cap = 0;
    for (uint32_t i = 0; i < 10 * N; i++) {
      if (len == cap) items = realloc(items, (cap = cap ? cap * 2 : 16) * sizeof *items);
      items[len++] = i;
    }
    sink += items[len - 1];
    free(items);
  });

  BENCH("arena vec push 10M u32", 10 * N, {
    vec(uint32_t) xs = {};
    for (uint32_t i = 0; i < 10 * N; i++) push(&arena, &xs, i);
    sink += xs.items[xs.len - 1];
    cilo_arena_reset(&arena);
  });

  BENCH("arena vec push, one call each", 10 * N, {
    U32s xs = {};
    for (uint32_t i = 0; i < 10 * N; i++) push_one(&arena, &xs, i);
    sink += xs.items[xs.len - 1];
    cilo_arena_reset(&arena);
  });

  char line[64];
  BENCH("snprintf + strdup", N, {
    for (int i = 0; i < N; i++) {
      snprintf(line, sizeof line, "user-%d", i);
      char *s = strdup(line);
      sink += (uint8_t)s[5];
      free(s);
    }
  });

  BENCH("aprintf into an arena", N, {
    for (int i = 0; i < N; i++) sink += (uint8_t)aprintf(&arena, "user-%d", i)[5];
    cilo_arena_reset(&arena);
  });

  cilo_arena_drop(&arena);
  pool_drop(&pool);
  return 0;
}
