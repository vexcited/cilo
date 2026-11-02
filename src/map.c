#include <cilo/map.h>

static size_t entries_at(size_t cap, size_t ealign) { return (cap + ealign - 1) & ~(ealign - 1); }

static size_t table_bytes(size_t cap, size_t esize, size_t ealign) {
  size_t bytes;
  if (ckd_mul(&bytes, cap, esize) || ckd_add(&bytes, bytes, entries_at(cap, ealign))) cilo_oom(SIZE_MAX);
  return bytes;
}

static size_t table_align(size_t ealign) { return ealign > 16 ? ealign : 16; }

void *cilo__map_claim(cilo_map *m, uint64_t h, size_t esize) {
  size_t mask = (m->cap >> 3) - 1, g = (size_t)(h >> 7) & mask;
  for (size_t step = 1;; step++) {
    uint64_t group;
    memcpy(&group, m->ctrl + g * 8, 8);
    // empty and deleted bytes are the ones with their top bit set.
    uint64_t free = group & CILO__TOPS;
    if (free) {
      size_t i = g * 8 + ((size_t)__builtin_ctzll(free) >> 3);
      if (m->ctrl[i] == CILO__EMPTY) m->growth--;
      m->ctrl[i] = (uint8_t)(h >> 57);
      m->len++;
      return (char *)m->entries + i * esize;
    }
    g = (g + step) & mask;
  }
}

void cilo__map_rehash(cilo_allocator *a, cilo_map *m, size_t esize, size_t ealign, size_t ksize,
                      cilo__key_kind kind) {
  // a table mostly made of deleted slots is rebuilt at the same size, a full one doubles.
  size_t cap = !m->cap ? 8 : m->len * 16 >= m->cap * 7 ? m->cap * 2 : m->cap;
  size_t at = entries_at(cap, ealign);
  uint8_t *ctrl = cilo_alloc(a, table_bytes(cap, esize, ealign), table_align(ealign));
  memset(ctrl, CILO__EMPTY, cap);
  cilo_map next = {.ctrl = ctrl, .entries = ctrl + at, .cap = cap, .growth = cap - cap / 8};
  for (size_t i = 0; i < m->cap; i++) {
    if (m->ctrl[i] & 0x80) continue;
    const char *e = (const char *)m->entries + i * esize;
    memcpy(cilo__map_claim(&next, cilo__key_hash(e, ksize, kind), esize), e, esize);
  }
  if (m->cap) cilo_free(a, m->ctrl, table_bytes(m->cap, esize, ealign));
  *m = next;
}

bool cilo__map_erase(cilo_map *m, void *entry, size_t esize) {
  if (!entry) return false;
  size_t i = (size_t)((char *)entry - (char *)m->entries) / esize;
  uint64_t group;
  memcpy(&group, m->ctrl + (i & ~(size_t)7), 8);
  // a group that still has an empty slot never sent a probe further, so this slot can be empty again.
  if (cilo__match_empty(group)) {
    m->ctrl[i] = CILO__EMPTY;
    m->growth++;
  } else {
    m->ctrl[i] = CILO__DELETED;
  }
  m->len--;
  return true;
}

void *cilo__map_next(const cilo_map *m, const void *entry, size_t esize) {
  size_t i = entry ? (size_t)((const char *)entry - (const char *)m->entries) / esize + 1 : 0;
  for (; i < m->cap; i++)
    if (!(m->ctrl[i] & 0x80)) return (char *)m->entries + i * esize;
  return nullptr;
}

void cilo__map_clear(cilo_map *m) {
  if (!m->cap) return;
  memset(m->ctrl, CILO__EMPTY, m->cap);
  m->len = 0;
  m->growth = m->cap - m->cap / 8;
}

void cilo__map_drop(cilo_allocator *a, cilo_map *m, size_t esize, size_t ealign) {
  if (m->cap) cilo_free(a, m->ctrl, table_bytes(m->cap, esize, ealign));
  *m = (cilo_map){};
}
