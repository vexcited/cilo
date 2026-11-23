# cilo

Allocators for C23, the memory layer under every CiKo library: a thread safe general purpose heap, arenas, pools,
growable arrays, hash maps and scratch space, with a typed macro layer that inlines down to a few instructions.

```c
#include <cilo/dsl.h>

User *u = new(&cilo_heap, User, .id = 1);               // malloc, size class folded at compile time
delete(&cilo_heap, u);

cilo_arena arena = {};                                  // zeroed is ready
User *user = new(&arena, User, .id = 1, .name = "ada"); // a pointer bump, missing fields zero
int *xs = new_array(&arena, int, n);                    // zeroed, overflow checked

vec(User) users = {};
push(&arena, &users, .id = 2)->name = "linus";          // grows in place at the top of the arena

pool(Node) nodes = {};
Node *node = take(&nodes, .value = 1);                  // two loads and a store
give(&nodes, node);

rewinding(&arena) { ... }                               // what the block allocated is gone at its end
scratch(tmp) { char *s = aprintf(tmp, "%d", 42); }      // a thread scratch arena, rewound at the end
cilo_arena_reset(&arena);                               // frees everything, keeps the chunks
```

## Faster than the known allocators

`bench/compare.sh <competitors> [out] [rounds]` builds the same workloads against each allocator, called directly,
without overriding malloc. Nanoseconds per operation, lower is better, `-O2`, the best of 5 interleaved rounds. The
competitors are built from their main branches as of 2026-10-30, in their release configurations. `CC=clang` picks
the compiler on Linux.

macOS 27 on an Apple M4 Pro:

| allocator          | struct   | mixed    | churn    | medium   | 8 threads | cross thread | large   | peak MiB |
| ------------------ | -------- | -------- | -------- | -------- | --------- | ------------ | ------- | -------- |
| **cilo**           | 2.37     | **9.01** | **2.37** | 4.01     | **0.31**  | 8.66         | **8.2** | **455**  |
| cilo, not inlined  | **2.11** | 10.2     | 2.60     | **3.88** | 0.35      | **7.53**     | **8.2** | **455**  |
| mimalloc           | 3.27     | 13.9     | 4.83     | 17.2     | 0.97      | 12.5         | 202     | 489      |
| jemalloc           | 10.2     | 11.4     | 5.24     | 21.3     | 0.71      | 8.20         | 254     | 487      |
| snmalloc           | 4.76     | 20.4     | 4.26     | 17.9     | 0.75      | 11.1         | 1144    | 488      |
| rpmalloc           | 3.45     | 23.0     | 2.68     | 4.95     | 0.38      | 20.2         | 10.5    | 456      |
| libc               | 11.6     | 27.8     | 22.4     | 37.5     | 19.4      | 32.7         | 94.3    | 559      |

Linux 6 arm64, Debian trixie and clang 19 in Docker's virtual machine, on the same M4 Pro:

| allocator          | struct   | mixed    | churn    | medium   | 8 threads | cross thread | large    | peak MiB |
| ------------------ | -------- | -------- | -------- | -------- | --------- | ------------ | -------- | -------- |
| **cilo**           | **3.00** | **11.9** | 2.40     | 4.96     | **0.32**  | **5.88**     | 17.3     | **454**  |
| cilo, not inlined  | 3.96     | **11.9** | 2.38     | **4.36** | 0.33      | 6.65         | **17.0** | **454**  |
| mimalloc           | 4.33     | 20.1     | 6.77     | 17.0     | 1.41      | 19.5         | 141      | 489      |
| jemalloc           | 14.6     | 14.6     | 5.08     | 16.6     | 0.66      | 11.6         | 250      | 490      |
| snmalloc           | 4.67     | 14.5     | 3.86     | 27.5     | 0.68      | 9.48         | 95.4     | 495      |
| rpmalloc           | 5.39     | 17.4     | **2.35** | 8.79     | 0.47      | 13.5         | 19.7     | 459      |
| glibc              | 8.41     | 25.6     | 9.21     | 62.0     | 1.29      | 58.7         | 75.6     | 463      |

- **struct**: 1M allocations of a 16 byte struct, then 1M frees.
- **mixed**: 1M random sizes up to 512 bytes, freed in reverse.
- **churn**: 10M random replacements among 1000 live blocks of up to 256 bytes.
- **medium**: the same with sizes up to 32 KiB.
- **8 threads**: churn on 8 threads at once.
- **cross thread**: 4 producer threads allocate, 4 consumer threads free.
- **large**: 64 live blocks from 128 KiB to 1 MiB.
- **peak MiB**: a run of its own that writes every block, frees half in a scattered order and refills with
  bigger sizes. About 374 MiB are live at the peak and 64 MiB are holes no size class allocator can fill, so the
  rest is rounding and overhead.

The "not inlined" row calls cilo through a function, so the gap to it is what inlining buys. Two columns are
ties rather than wins. Cross thread on macOS moves by about 1 ns from run to run, and cilo and jemalloc trade
places, because every free there moves a cache line between cores. jemalloc keeps freed pointers in arrays
instead of in the blocks, which avoids that. Churn on Linux is within 2% of rpmalloc.

## How the heap works

- **Inlined both ways.** `cilo_malloc` and `cilo_mfree` live in the header. Allocating pops a page's free list:
  thread heap, page, two loads, a store. A constant size, which is every `new`, picks its size class at compile
  time. Up to 1 KiB the class is the size in words, a shift, past that a `clz`.
- **No thread local thunk.** On Apple silicon, pthread keys are slots behind `tpidrro_el0`, so the heap is one
  register read and two loads away. On Linux, `thread_local` uses the initial exec model, one load from the GOT
  even when cilo is built position independent.
- **One compare to free.** A pointer finds its page with a mask and a shift, because 4 MiB segments are aligned.
  Full pages and pages with aligned blocks carry the sign bit in their live count, so the local free tests
  ownership and "anything special" with two compares and no flag loads.
- **Colored headers.** Every header at a 4 MiB boundary lands in the same cache set, and 64 of them evict each
  other: 6.7 ns per touch against 1.7 ns. Each segment's header sits at an offset taken from its own address,
  one cache line out of 64, and block strides from 4 KiB get a line more for the same reason.
- **Fine classes.** 8 byte steps up to 1 KiB, then eight classes per doubling, at most 6.25% lost to rounding.
- **Big pages.** 64 KiB pages up to 1 KiB, 512 KiB pages up to 16 KiB and 4 MiB pages up to 128 KiB, so each holds
  about 32 blocks or more and rarely flips between full and not full, the costly transition of this design.
- **Large blocks on the same path.** Past 128 KiB, a block gets a mapping of its own. Freed ones wait in per thread
  classes that the inlined pop serves like any page, an empty class borrows from the next three, and the thread
  keeps up to 64 MiB of them, counted without touching the fast path. A single path from 128 KiB up has no branch
  to mispredict between two kinds of blocks.
- **Empty pages stay threaded.** An empty page keeps its class and its free list, so allocating the same sizes
  again writes nothing. Rethreading memory that was just freed cost 31 ns per block on Linux against 6 ns for
  reusing it. Empty pages go back to their segment after 50 ms, or right away when a class needs a page that would
  otherwise come from a new segment.
- **Cross thread frees in batches.** A free into another thread's page waits in a small hashed set of per page
  batches. A batch costs a single compare and swap onto the page's atomic list, which the owner takes in one
  exchange. The owner's hot fields and the remote fields sit on separate cache lines.
- **Heaps outlive threads.** A thread that exits leaves its heap, live pages included, to the next thread that
  needs one. A late free from another thread never touches unmapped memory, and nothing is ever adopted page by
  page.
- **No syscalls in waves.** Empty segments are kept 50 ms before going back to the system and large blocks are
  cached, so a program freeing and allocating in waves never maps and unmaps. `cilo_heap_trim()` gives it all
  back now.
- **No transparent huge pages for small blocks.** On Linux, segments of small blocks are advised out of THP: a
  huge page would make 2 MiB resident for a page touched a few KiB at a time, and churn ran 3.40 ns with it
  against 2.19 ns without. Large blocks keep the system's setting.

## How the arenas are fast

- **Static dispatch.** Every macro takes a `cilo_arena *`, `&cilo_heap` or a `cilo_allocator *`. `_Generic` sends
  arenas and the heap to their inlined fast paths at compile time. `new` fills the memory from a compound literal,
  which optimizers turn into plain stores.
- **Devirtualized interface.** Libraries take a `cilo_allocator *`, a pointer to a vtable pointer embedded first in
  the allocator's state. An arena's vtable pointer is null, so the generic path recognizes arenas with one
  compare and inlines them too. A zeroed arena is still valid.
- **Unpaired loads.** The fast path reads `end` with a load of its own. Loading `cur` and `end` with one `ldp`, which
  clang and gcc did on arm64, makes each allocation wait for the store of `cur` by the previous one to retire,
  because a load pair half covered by a pending store cannot take its value from the store buffer. On an M4 that
  is 2.65 ns per allocation against 1.31. `push` reads `items` and `cap` alone for the same reason, so `len` is
  never paired. x86_64 has no load pairs and compiles to the same two loads as before.
- **An aligned cursor.** Every allocation ends on an 8 byte boundary, so `cur` is always 8 aligned and an alignment up to 8 needs no padding: the next allocation waits on one add instead of four. Allocating 16 B nodes went from 1.5 to 0.9 ns, at the price of rounding each allocation up to a multiple of 8 bytes.
- **Small chunks from the heap.** A zeroed arena starts on a 4 KiB chunk from cilo's heap and doubles. From 64 KiB
  on, chunks are zeroed pages mapped from the system. Thousands of small arenas never make a system call and only
  hold what they use.
- **Free zeroing.** Mapped chunks are already zero. Each chunk remembers how far it was ever used, so `new_array`
  only clears memory that was handed out before.
- **No syscalls in steady state.** Rewinds and resets keep chunks as spares for the next allocations.
- **Stack first.** `cilo_arena a = arena_on(buf);` starts on a local array and spills once full. `arena_fixed(buf)`
  never spills.
- **In place growth.** `cilo_arena_realloc` extends the last allocation in place, so `push` on an arena rarely
  copies, and freeing the last allocation gives it back.
- **One pass formatting.** `aprintf` formats straight into the arena's free space and only commits what it wrote.

`bench/bench.c`, `-O2`, the best of 20 rounds, macOS 27 on an Apple M4 Pro. "before" is the previous release,
with paired loads and every chunk mapped.

| arenas and pools                          | libc       | cilo before | cilo       |
| ----------------------------------------- | ---------- | ----------- | ---------- |
| 1M 16 B nodes, allocate then free all     | 11.6 ns/op | 3.56 ns/op  | 2.68 ns/op |
| the same through a `cilo_allocator *`     |            | 3.41 ns/op  | 2.38 ns/op |
| 1M 16 B nodes, one call each              |            | 2.65 ns/op  | 1.31 ns/op |
| 1M 16 B strings, one call each            |            | 1.89 ns/op  | 1.08 ns/op |
| churn with 64 live nodes, pool            | 11.0 ns/op | 1.34 ns/op  | 1.24 ns/op |
| push 10M u32                              | 0.43 ns/op | 0.37 ns/op  | 0.45 ns/op |
| push 10M u32, one call each               |            | 1.92 ns/op  | 0.94 ns/op |
| format a short string                     | 47.0 ns/op | 30.8 ns/op  | 31.6 ns/op |
| a small arena: 8 nodes, a name, dropped   |            | 1522 ns     | 14.0 ns    |
| a frame arena: 32 KiB, dropped            |            | 2832 ns     | 372 ns     |
| 20k live arenas of 16 nodes each          |            | 314 MiB     | 82 MiB     |

The pool row moves between 0.91 and 1.36 ns from run to run in both builds, the usual value is given. One row is
slower. A loop pushing into one vector keeps `len` in a register and never hit the stall, and it now pays a load of
`items` on every push: 0.37 to 0.45 ns. A push per call, or a loop whose items may alias `len`, such as a
`vec(size_t)` or a `vec(char *)`, gains about 1 ns per push.

## Arena defaults

Arenas that leave `backing` or `min_chunk` zero follow process wide defaults, set once at startup:

```c
// 1 KiB first chunks, for programs keeping tens of thousands of tiny arenas.
cilo_arena_defaults(&(cilo_arena_config){.min_chunk = 1 << 10});
// every chunk from the heap, which caches its large blocks, so arenas made and dropped in a loop stay off the system.
cilo_arena_defaults(&(cilo_arena_config){.map_from = SIZE_MAX});
// the previous behaviour: every chunk mapped, the first of 64 KiB.
cilo_arena_defaults(&(cilo_arena_config){.min_chunk = 64 << 10, .map_from = 1});
// any allocator, taken by each arena with its first chunk and kept.
cilo_arena_defaults(&(cilo_arena_config){.backing = CILO_OF(&my_allocator)});
```

It returns the defaults it replaced, and `cilo_arena_defaults(nullptr)` only reads them.

## Maps and hashing

`cilo/map.h`, included by `cilo/dsl.h`, has a typed hash map and the hash under it:

```c
map(const char *, int) ids = {};                        // zeroed is ready
*map_put(&arena, &ids, "ada") = 1;                      // finds or inserts, a pointer to the value
int *id = map_get(&ids, "ada");                         // null when missing
map_put(&cilo_heap, &points, ((Point){1, 2}), &created); // any key without padding, created says which
const char **known = map_get_n(&names, text, len);      // a string key from a slice, for interning
map_each(e, &ids) printf("%s %d\n", e->key, e->val);
map_del(&ids, "ada");
map_drop(&arena, &ids);
```

- **Groups of eight.** Each slot has a control byte, empty, deleted, or seven bits of its key's hash. A lookup
  matches the eight bytes of a group at once with word arithmetic, so it usually reads one control word and
  compares one key, and a miss stops at the first group with an empty slot. Tables are seven eighths full at most
  and deletions need no shifting.
- **Any key.** Keys compare by their bytes, `char *` and `const char *` keys by content. A compound literal key
  goes in parentheses because of its commas.
- **Allocators like `push`.** The calls that insert take an arena, `&cilo_heap` or any `cilo_allocator *`, the
  same one every time. Inserting may move the table, so a value pointer lasts until the next insertion.
- **`cilo_hash`** is a 64 bit hash after wyhash, inlined, two loads and two multiplications for keys up to 16
  bytes. `cilo_hash_u64` folds one multiplication. Neither is for hashes that leave the process.

Against the linear probing map civue and vektra2 each carry, nonzero `uint64_t` keys, a splitmix finalizer, FNV
for strings and three quarters full at most, from `bench/bench.c`:

| maps                                     | reference  | cilo       |
| ---------------------------------------- | ---------- | ---------- |
| insert 1M `uint64_t` keys                | 27.5 ns/op | 17.0 ns/op |
| hit among 1M keys, random order          | 11.1 ns/op | 9.0 ns/op  |
| miss among 1M keys                       | 19.7 ns/op | 2.4 ns/op  |
| hit among 4k keys, in cache              | 1.26 ns/op | 1.65 ns/op |
| intern 100k names, each 10 times         | 14.6 ns/op | 15.0 ns/op |

Inserts and misses are where the groups pay. Hits among a million keys are cache misses for both, and moved
between a tie and this lead from run to run. In cache, the reference's shorter path wins, the group match is the
price of fast misses and deletions. Interning is a tie, it is mostly the string compare.

## Build flags

Apple clang's machine outliner, on by default at `-O2` on arm64, turns repeated sequences of inlined fast paths
back into calls inside hot loops. The CMake target adds `-mno-outline` to cilo and its users,
`-DCILO_NO_OUTLINE=OFF` keeps the default.

## Safety without cost

- Sizes are checked with `ckd_mul`. Out of memory calls `cilo_oom_hook`, then aborts, so allocations never return
  null and never need a check.
- Under AddressSanitizer, free arena space stays poisoned and every arena allocation gets a redzone, so reads
  after a rewind and overflows are reported. The heap hands every block to the system malloc instead, so ASan
  sees each one. Build the library and its users with the same sanitizer.
- `pool(T)`, `vec(T)`, `give` and `delete` are typed. Giving a `Node *` back to a `pool(Leaf)` does not compile, and
  `arena_on` refuses a pointer where it needs an array.
- The tests pass under ASan, TSan and UBSan on macOS and Linux, with clang and gcc, cross thread frees and thread
  exits included. The plain build passes on x86_64 Linux too.

## Scratch arenas

`scratch(tmp) { ... }` borrows one of four thread local arenas and rewinds it at the end of the block. A function
that fills arenas it was given names them, `scratch(tmp, out)`, so the scratch is never one of them. A runtime that
switches tasks on one thread binds each task's own set with `cilo_scratch_bind`, which cinc does on every switch,
so a scratch block may wait. They are dropped when their thread exits.

## Names

`cilo/dsl.h` defines the lowercase names for applications. They are function like, so a variable called `new` or
`push` still works. Libraries stick to the `CILO_` ones and `cilo/dsl_end.h` drops them again.

| dsl           | full                                        |
| ------------- | ------------------------------------------- |
| `new`         | `CILO_NEW(a, T, ...)`                       |
| `new_array`   | `CILO_NEW_ARRAY(a, T, n)`                   |
| `delete`      | `CILO_DELETE(a, ptr)`                       |
| `vec`         | `CILO_VEC(T)`                               |
| `push`        | `CILO_PUSH(a, v, ...)`                      |
| `reserve`     | `CILO_RESERVE(a, v, n)`                     |
| `pool`        | `CILO_POOL(T)`                              |
| `take`        | `CILO_TAKE(pool, ...)`                      |
| `give`        | `CILO_GIVE(pool, item)`                     |
| `pool_drop`   | `CILO_POOL_DROP(pool)`                      |
| `rewinding`   | `CILO_REWINDING(a)`                         |
| `with_arena`  | `CILO_WITH_ARENA(name, options...)`         |
| `scratch`     | `CILO_SCRATCH(name, conflicts...)`          |
| `arena_on`    | `CILO_ARENA_ON(buf)`                        |
| `arena_fixed` | `CILO_ARENA_FIXED(buf)`                     |
| `astrdup`     | `CILO_STRDUP(a, s)`                         |
| `aprintf`     | `CILO_PRINTF(a, fmt, ...)`                  |
| `map`         | `CILO_MAP(K, V)`                            |
| `map_get`     | `CILO_MAP_GET(m, key)`                      |
| `map_get_n`   | `CILO_MAP_GET_N(m, s, len)`                 |
| `map_has`     | `CILO_MAP_HAS(m, key)`                      |
| `map_put`     | `CILO_MAP_PUT(a, m, key, created?)`         |
| `map_del`     | `CILO_MAP_DEL(m, key)`                      |
| `map_each`    | `CILO_MAP_EACH(e, m)`                       |
| `map_clear`   | `CILO_MAP_CLEAR(m)`                         |
| `map_drop`    | `CILO_MAP_DROP(a, m)`                       |

## Limits

- `cilo_mfree` only takes blocks from `cilo_malloc`. cilo does not replace the system `malloc` yet.
- Arenas and pools are single threaded by design, each thread or task owns its own. The heap is thread safe.
- `push` and `take` initialize like a compound literal. To copy a whole struct, write `*push(a, &v) = value`.
- A thread that frees into other threads' pages and then sleeps keeps its batches unpublished, at most 64 of up
  to 63 blocks each. They go out on its next allocation slow path, `cilo_heap_trim` or its exit.
- `cilo_memalign` takes alignments up to 2 MiB.
- macOS and Linux only.
- Benchmarks are microbenchmarks on one machine, not a full suite like mimalloc-bench.
