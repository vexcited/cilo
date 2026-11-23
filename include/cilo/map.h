#ifndef CILO_MAP_H
#define CILO_MAP_H

#include "cilo.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================================================== */
/* hashing and maps.                                                                                          */
/*                                                                                                            */
/*   CILO_MAP(const char *, int) ids = {};             zeroed is ready, string keys compare by content        */
/*   *CILO_MAP_PUT(&arena, &ids, "ada") = 1;            inserts or finds, a pointer to the value               */
/*   int *id = CILO_MAP_GET(&ids, "ada");               null when missing                                      */
/*   CILO_MAP_EACH(e, &ids) printf("%s %d\n", e->key, e->val);                                                 */
/*                                                                                                            */
/* open addressing in groups of eight slots, each with a control byte holding seven bits of its hash, matched */
/* eight at a time with word arithmetic: a lookup usually reads one control word and compares one key. like   */
/* every growing cilo call, the ones that insert take the allocator, which must be the same each time.        */
/* ========================================================================================================== */

__extension__ typedef unsigned __int128 cilo__u128;

constexpr uint64_t CILO__H0 = 0x2d358dccaa6c78a5ull;
constexpr uint64_t CILO__H1 = 0x8bb84b93962eacc9ull;

// the high and low halves of a 128 bit product folded together, the mixing step of wyhash.
[[gnu::always_inline]] static inline uint64_t cilo__mum(uint64_t a, uint64_t b) {
  cilo__u128 r = (cilo__u128)a * b;
  return (uint64_t)r ^ (uint64_t)(r >> 64);
}

[[gnu::always_inline]] static inline uint64_t cilo__r64(const unsigned char *p) {
  uint64_t v;
  memcpy(&v, p, 8);
  return v;
}

[[gnu::always_inline]] static inline uint64_t cilo__r32(const unsigned char *p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

// a 64 bit hash of `len` bytes, after wyhash: short keys take two loads and two multiplications. not for
// hashes that leave the process, it may change between versions.
[[gnu::always_inline]] static inline uint64_t cilo_hash(const void *key, size_t len) {
  const unsigned char *p = key;
  uint64_t seed = CILO__H0 ^ cilo__mum(len ^ CILO__H0, CILO__H1), a, b;
  if (CILO_LIKELY(len <= 16)) {
    if (len >= 4) {
      size_t mid = (len >> 3) << 2;
      a = cilo__r32(p) << 32 | cilo__r32(p + mid);
      b = cilo__r32(p + len - 4) << 32 | cilo__r32(p + len - 4 - mid);
    } else if (len) {
      a = (uint64_t)p[0] << 16 | (uint64_t)p[len >> 1] << 8 | p[len - 1];
      b = 0;
    } else {
      a = b = 0;
    }
  } else {
    size_t left = len;
    for (; left > 16; p += 16, left -= 16) seed = cilo__mum(cilo__r64(p) ^ CILO__H1, cilo__r64(p + 8) ^ seed);
    a = cilo__r64(p + left - 16);
    b = cilo__r64(p + left - 8);
  }
  cilo__u128 r = (cilo__u128)(a ^ CILO__H1) * (b ^ seed);
  return cilo__mum((uint64_t)r ^ CILO__H0 ^ len, (uint64_t)(r >> 64) ^ CILO__H1);
}

// one folded multiplication: every output bit depends on every input bit, which is all a table needs.
[[gnu::always_inline]] static inline uint64_t cilo_hash_u64(uint64_t x) { return cilo__mum(x ^ CILO__H0, CILO__H1); }

static inline uint64_t cilo_hash_str(const char *s) { return cilo_hash(s, strlen(s)); }

/* maps. */

typedef struct cilo_map {
  // a control byte per slot: empty, deleted, or seven bits of the hash of a full slot.
  uint8_t *ctrl;
  void *entries;
  size_t len;
  // slots, zero or a power of two from 8.
  size_t cap;
  // insertions left before the table grows, seven eighths of it minus what is full or deleted.
  size_t growth;
} cilo_map;

enum : uint8_t { CILO__EMPTY = 0x80, CILO__DELETED = 0xfe };

constexpr uint64_t CILO__LANES = 0x0101010101010101ull;
constexpr uint64_t CILO__TOPS = 0x8080808080808080ull;

// the slots of a group whose byte is h2. a full byte right after a match may be flagged too, which the key
// compare sorts out, empty and deleted bytes never are.
[[gnu::always_inline]] static inline uint64_t cilo__match(uint64_t group, uint8_t h2) {
  uint64_t x = group ^ (CILO__LANES * h2);
  return (x - CILO__LANES) & ~x & CILO__TOPS;
}
[[gnu::always_inline]] static inline uint64_t cilo__match_empty(uint64_t group) {
  return group & ~(group << 6) & CILO__TOPS;
}

typedef enum : uint8_t { CILO__KEY_BYTES, CILO__KEY_STR } cilo__key_kind;

[[gnu::always_inline]] static inline uint64_t cilo__key_hash(const void *key, size_t size, cilo__key_kind kind) {
  if (kind == CILO__KEY_STR) return cilo_hash_str(*(const char *const *)key);
  if (size == 8) return cilo_hash_u64(cilo__r64(key));
  if (size == 4) return cilo_hash_u64(cilo__r32(key));
  return cilo_hash(key, size);
}

[[gnu::always_inline]] static inline bool cilo__key_eq(const void *entry, const void *key, size_t size,
                                                       cilo__key_kind kind) {
  if (kind == CILO__KEY_STR) return strcmp(*(const char *const *)entry, *(const char *const *)key) == 0;
  return memcmp(entry, key, size) == 0;
}

// the entry holding `key`, whose hash is `h`, or null. a `slice` looks a string key up by its first `len`
// bytes instead, which hold no nul.
[[gnu::always_inline]] static inline void *cilo__map_find(const cilo_map *m, const void *key, uint64_t h,
                                                          size_t esize, size_t ksize, cilo__key_kind kind,
                                                          const char *slice, size_t len) {
  if (CILO_UNLIKELY(!m->cap)) return nullptr;
  size_t mask = (m->cap >> 3) - 1, g = (size_t)(h >> 7) & mask;
  uint8_t h2 = (uint8_t)(h >> 57);
  for (size_t step = 1;; step++) {
    uint64_t group;
    memcpy(&group, m->ctrl + g * 8, 8);
    for (uint64_t hits = cilo__match(group, h2); hits; hits &= hits - 1) {
      char *e = (char *)m->entries + (g * 8 + ((size_t)__builtin_ctzll(hits) >> 3)) * esize;
      // strlen and memcmp read words, strncmp went byte by byte and cost a tenth more on interning.
      if (slice ? strlen(*(const char **)e) == len && memcmp(*(const char **)e, slice, len) == 0
                : cilo__key_eq(e, key, ksize, kind))
        return e;
    }
    if (CILO_LIKELY(cilo__match_empty(group))) return nullptr;
    g = (g + step) & mask;
  }
}

[[gnu::always_inline]] static inline void *cilo__map_lookup(const cilo_map *m, const void *key, size_t esize,
                                                            size_t ksize, cilo__key_kind kind) {
  return cilo__map_find(m, key, cilo__key_hash(key, ksize, kind), esize, ksize, kind, nullptr, 0);
}

[[gnu::always_inline]] static inline void *cilo__map_val(void *entry, size_t offset) {
  return entry ? (char *)entry + offset : nullptr;
}

[[gnu::cold, gnu::noinline]] void cilo__map_rehash(cilo_allocator *a, cilo_map *m, size_t esize, size_t ealign,
                                                   size_t ksize, cilo__key_kind kind);
// a free slot for a key that is not in the map, its control byte set. the caller fills the entry.
[[gnu::returns_nonnull]] void *cilo__map_claim(cilo_map *m, uint64_t h, size_t esize);
bool cilo__map_erase(cilo_map *m, void *entry, size_t esize);
[[gnu::pure]] void *cilo__map_next(const cilo_map *m, const void *entry, size_t esize);
void cilo__map_clear(cilo_map *m);
void cilo__map_drop(cilo_allocator *a, cilo_map *m, size_t esize, size_t ealign);

// finds `key` or inserts it with a zeroed value. returns the entry and sets `*created` when not null.
[[gnu::always_inline]] static inline void *cilo__map_slot(cilo_allocator *a, cilo_map *m, const void *key,
                                                          size_t esize, size_t ealign, size_t ksize,
                                                          cilo__key_kind kind, bool *created) {
  uint64_t h = cilo__key_hash(key, ksize, kind);
  void *e = cilo__map_find(m, key, h, esize, ksize, kind, nullptr, 0);
  if (created) *created = !e;
  if (CILO_LIKELY(e)) return e;
  if (CILO_UNLIKELY(!m->growth)) cilo__map_rehash(a, m, esize, ealign, ksize, kind);
  e = cilo__map_claim(m, h, esize);
  memset(e, 0, esize);
  memcpy(e, key, ksize);
  return e;
}

// `of` is never set, it only carries the entry type for the macros.
#define CILO_MAP(K, V)                                                                                           \
  struct {                                                                                                       \
    cilo_map raw;                                                                                                \
    struct {                                                                                                     \
      K key;                                                                                                     \
      V val;                                                                                                     \
    } *of;                                                                                                       \
  }

#define CILO__MAP_KIND(m)                                                                                        \
  _Generic((m)->of->key, char *: CILO__KEY_STR, const char *: CILO__KEY_STR, default: CILO__KEY_BYTES)
#define CILO__MAP_KEY(m, k) ((typeof((m)->of->key)[1]){k})
#define CILO__MAP_VAL(m, entry) ((typeof(&(m)->of->val))cilo__map_val(entry, offsetof(typeof(*(m)->of), val)))
#define CILO__FIRST(x, ...) x

// the value of `key`, or null.
#define CILO_MAP_GET(m, k)                                                                                       \
  CILO__MAP_VAL(m, cilo__map_lookup(&(m)->raw, CILO__MAP_KEY(m, k), sizeof *(m)->of, sizeof (m)->of->key,       \
                                    CILO__MAP_KIND(m)))
// the value of the string key spelled by the first `len` bytes of `s`, which hold no nul. for interning.
#define CILO_MAP_GET_N(m, s, len)                                                                                \
  CILO__MAP_VAL(m, cilo__map_get_n(&(m)->raw, s, len, sizeof *(m)->of, sizeof (m)->of->key,                     \
                                   _Generic((m)->of->key, char *: 0, const char *: 0)))
#define CILO_MAP_HAS(m, k) (CILO_MAP_GET(m, k) != nullptr)
// the value of `key`, inserted zeroed when missing. a fourth argument, a bool *, tells which.
#define CILO_MAP_PUT(a, m, k, ...)                                                                               \
  CILO__MAP_VAL(m, cilo__map_slot(CILO_OF(a), &(m)->raw, CILO__MAP_KEY(m, k), sizeof *(m)->of,                  \
                                  alignof(typeof(*(m)->of)), sizeof (m)->of->key, CILO__MAP_KIND(m),             \
                                  CILO__FIRST(__VA_ARGS__ __VA_OPT__(, ) nullptr)))
// removes `key`, false when it was not there.
#define CILO_MAP_DEL(m, k)                                                                                       \
  cilo__map_erase(&(m)->raw,                                                                                     \
                  cilo__map_lookup(&(m)->raw, CILO__MAP_KEY(m, k), sizeof *(m)->of, sizeof (m)->of->key,         \
                                   CILO__MAP_KIND(m)),                                                           \
                  sizeof *(m)->of)
// `e` walks the entries, `e->key` and `e->val`, in no particular order. the map must not change meanwhile.
#define CILO_MAP_EACH(e, m)                                                                                      \
  for (typeof((m)->of) e = cilo__map_next(&(m)->raw, nullptr, sizeof *(m)->of); e;                               \
       e = cilo__map_next(&(m)->raw, e, sizeof *(m)->of))
// empties the map and keeps its table.
#define CILO_MAP_CLEAR(m) cilo__map_clear(&(m)->raw)
// gives the table back to `a`, the map is empty and ready again.
#define CILO_MAP_DROP(a, m) cilo__map_drop(CILO_OF(a), &(m)->raw, sizeof *(m)->of, alignof(typeof(*(m)->of)))

// the last argument only checks at compile time that the keys are strings.
[[gnu::always_inline]] static inline void *cilo__map_get_n(const cilo_map *m, const char *s, size_t len,
                                                           size_t esize, size_t ksize, int strings) {
  (void)strings;
  return cilo__map_find(m, nullptr, cilo_hash(s, len), esize, ksize, CILO__KEY_STR, s ? s : "", len);
}

#ifdef __cplusplus
}
#endif

#endif
