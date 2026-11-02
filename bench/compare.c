#define _DEFAULT_SOURCE
#include <pthread.h>
#include <sys/resource.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(USE_CILO) || defined(USE_CILO_CALL)
#include <cilo/cilo.h>
#if defined(USE_CILO)
#define NAME "cilo"
#define A_MALLOC cilo_malloc
#define A_FREE cilo_mfree
#else
#define NAME "cilo, not inlined"
[[gnu::noinline]] static void *A_MALLOC(size_t size) { return cilo_malloc(size); }
[[gnu::noinline]] static void A_FREE(void *p) { cilo_mfree(p); }
#endif
#elif defined(USE_MIMALLOC)
#include <mimalloc.h>
#define NAME "mimalloc"
#define A_MALLOC mi_malloc
#define A_FREE mi_free
#elif defined(USE_JEMALLOC)
#include <jemalloc/jemalloc.h>
#define NAME "jemalloc"
#define A_MALLOC je_malloc
#define A_FREE je_free
#elif defined(USE_SNMALLOC)
void *sn_malloc(size_t);
void sn_free(void *);
#define NAME "snmalloc"
#define A_MALLOC sn_malloc
#define A_FREE sn_free
#elif defined(USE_RPMALLOC)
#include <rpmalloc.h>
#define NAME "rpmalloc"
#define A_MALLOC rpmalloc
#define A_FREE rpfree
#define THREAD_INIT() rpmalloc_thread_initialize()
#else
#define NAME "libc"
#define A_MALLOC malloc
#define A_FREE free
#endif

#ifndef THREAD_INIT
#define THREAD_INIT() ((void)0)
#endif

typedef struct Node {
  uint64_t value;
  struct Node *next;
} Node;

static double now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

static volatile uint64_t sink;

static uint64_t next_rand(uint64_t *s) {
  *s = *s * 6364136223846793005u + 1442695040888963407u;
  return *s >> 33;
}

enum { N = 1'000'000 };
static void *slots[N];

// a constant size, the way CILO_NEW and most code allocate a struct.
static double fixed_struct(void) {
  double start = now_ns();
  for (int i = 0; i < N; i++) {
    Node *n = A_MALLOC(sizeof(Node));
    n->value = (uint64_t)i;
    slots[i] = n;
  }
  for (int i = 0; i < N; i++) {
    sink += ((Node *)slots[i])->value;
    A_FREE(slots[i]);
  }
  return (now_ns() - start) / N;
}

static double mixed_lifo(void) {
  uint64_t seed = 42;
  double start = now_ns();
  for (int i = 0; i < N; i++) slots[i] = A_MALLOC(8 + next_rand(&seed) % 512);
  for (int i = N - 1; i >= 0; i--) A_FREE(slots[i]);
  return (now_ns() - start) / N;
}

static double churn_with(size_t live, size_t max_size, size_t ops) {
  void **table = calloc(live, sizeof *table);
  uint64_t seed = 7;
  double start = now_ns();
  for (size_t i = 0; i < ops; i++) {
    uint64_t r = next_rand(&seed);
    size_t slot = r % live;
    A_FREE(table[slot]);
    table[slot] = A_MALLOC(1 + (r >> 8) % max_size);
    *(char *)table[slot] = 1;
  }
  for (size_t i = 0; i < live; i++) A_FREE(table[i]);
  double took = (now_ns() - start) / (double)ops;
  free(table);
  return took;
}

static double churn_small(void) { return churn_with(1000, 256, 10 * N); }
static double churn_large(void) {
  void *table[64] = {};
  uint64_t seed = 11;
  double start = now_ns();
  for (int i = 0; i < N / 5; i++) {
    uint64_t r = next_rand(&seed);
    size_t slot = r % 64;
    A_FREE(table[slot]);
    table[slot] = A_MALLOC((128 << 10) + (r >> 8) % (896 << 10));
    ((char *)table[slot])[0] = 1;
  }
  for (int i = 0; i < 64; i++) A_FREE(table[i]);
  return (now_ns() - start) / (N / 5);
}
static double churn_medium(void) { return churn_with(500, 32 << 10, 2 * N); }

typedef struct thread_arg {
  double ns;
} thread_arg;

static void *thread_churn(void *arg) {
  THREAD_INIT();
  ((thread_arg *)arg)->ns = churn_with(1000, 512, 4 * N);
  return nullptr;
}

static double threads_8(void) {
  pthread_t t[8];
  thread_arg args[8];
  double start = now_ns();
  for (int i = 0; i < 8; i++) pthread_create(&t[i], nullptr, thread_churn, &args[i]);
  for (int i = 0; i < 8; i++) pthread_join(t[i], nullptr);
  return (now_ns() - start) / (8.0 * 4 * N);
}

// producers allocate, consumers on other threads free, through single producer single consumer rings.
enum { RING = 4096, PAIRS = 4, PER_PAIR = 2 * N };
typedef struct ring {
  _Atomic(size_t) head;
  char pad[64];
  _Atomic(size_t) tail;
  char pad2[64];
  void *items[RING];
} ring;
static ring rings[PAIRS];

static void *produce(void *arg) {
  THREAD_INIT();
  ring *r = arg;
  for (size_t i = 0; i < PER_PAIR; i++) {
    void *p = A_MALLOC(16 + i % 240);
    size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    while (head - atomic_load_explicit(&r->tail, memory_order_acquire) == RING) {}
    r->items[head % RING] = p;
    atomic_store_explicit(&r->head, head + 1, memory_order_release);
  }
  return nullptr;
}

static void *consume(void *arg) {
  THREAD_INIT();
  ring *r = arg;
  for (size_t i = 0; i < PER_PAIR; i++) {
    size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    while (atomic_load_explicit(&r->head, memory_order_acquire) == tail) {}
    A_FREE(r->items[tail % RING]);
    atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
  }
  return nullptr;
}

static double cross_thread(void) {
  pthread_t p[PAIRS], c[PAIRS];
  memset(rings, 0, sizeof rings);
  double start = now_ns();
  for (int i = 0; i < PAIRS; i++) {
    pthread_create(&p[i], nullptr, produce, &rings[i]);
    pthread_create(&c[i], nullptr, consume, &rings[i]);
  }
  for (int i = 0; i < PAIRS; i++) {
    pthread_join(p[i], nullptr);
    pthread_join(c[i], nullptr);
  }
  return (now_ns() - start) / ((double)PAIRS * PER_PAIR);
}

// every block written like a real program does, then half freed in a scattered order and refilled with bigger
// sizes, so the peak shows overhead and fragmentation instead of how much of an untouched block stays virtual.
static void memory(void) {
  uint64_t seed = 3;
  for (int i = 0; i < N; i++) {
    size_t n = 8 + next_rand(&seed) % 512;
    slots[i] = A_MALLOC(n);
    memset(slots[i], 1, n);
  }
  for (int i = 0; i < N; i += 2) A_FREE(slots[i]);
  for (int i = 0; i < N; i += 2) {
    size_t n = 8 + next_rand(&seed) % 1024;
    slots[i] = A_MALLOC(n);
    memset(slots[i], 1, n);
  }
  for (int i = 0; i < N; i++) sink += *(char *)slots[i];
  for (int i = 0; i < N; i++) A_FREE(slots[i]);
}

static double peak_mib(void) {
  struct rusage usage;
  getrusage(RUSAGE_SELF, &usage);
#if defined(__APPLE__)
  return (double)usage.ru_maxrss / (1 << 20);
#else
  return (double)usage.ru_maxrss / (1 << 10);
#endif
}

static double best_of(double (*bench)(void), int rounds) {
  double best = 1e300;
  for (int i = 0; i < rounds; i++) {
    double t = bench();
    if (t < best) best = t;
  }
  return best;
}

int main(int argc, char **argv) {
#if defined(USE_RPMALLOC)
  rpmalloc_initialize(nullptr);
#endif
  if (argc > 1 && strcmp(argv[1], "memory") == 0) {
    memory();
    printf("%.0f\n", peak_mib());
    return 0;
  }
  printf("%s", NAME);
  printf("\t%.2f", best_of(fixed_struct, 10));
  printf("\t%.2f", best_of(mixed_lifo, 10));
  printf("\t%.2f", best_of(churn_small, 5));
  printf("\t%.2f", best_of(churn_medium, 5));
  printf("\t%.2f", best_of(threads_8, 5));
  printf("\t%.2f", best_of(cross_thread, 7));
  printf("\t%.1f", best_of(churn_large, 3));
  return 0;
}
