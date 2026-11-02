#include <cilo/dsl.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

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

int main(void) {
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
