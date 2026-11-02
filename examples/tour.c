#include <cilo/dsl.h>
#include <stdio.h>

typedef struct Entry {
  const char *key;
  int count;
  struct Entry *next;
} Entry;

typedef pool(Entry) Entries;

// a word counter: words live in one arena, entries are recycled through a pool, and formatting uses a scratch
// arena that is gone once the report is printed. nothing is freed one by one.
static Entry *count(cilo_arena *words, Entries *entries, Entry *head, const char *word) {
  for (Entry *e = head; e; e = e->next) {
    if (strcmp(e->key, word) == 0) {
      e->count++;
      return head;
    }
  }
  return take(entries, .key = astrdup(words, word), .count = 1, .next = head);
}

int main(void) {
  const char *text[] = {"the", "cat", "and", "the", "hat", "and", "the", "bat"};

  char stack[4096];
  cilo_arena words = arena_on(stack);
  Entries entries = {};
  Entry *head = nullptr;
  for (size_t i = 0; i < sizeof text / sizeof *text; i++) head = count(&words, &entries, head, text[i]);

  scratch(tmp) {
    vec(const char *) lines = {};
    for (Entry *e = head; e; e = e->next) *push(tmp, &lines) = aprintf(tmp, "%-4s %d", e->key, e->count);
    for (size_t i = lines.len; i-- > 0;) puts(lines.items[i]);
  }

  pool_drop(&entries);
  cilo_arena_drop(&words);
  return 0;
}
