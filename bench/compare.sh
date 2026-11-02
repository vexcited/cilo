#!/bin/zsh
set -e
here=${0:A:h}
root=${here:h}
comp=${1:?competitor directory}
out=${2:-/tmp/cilo-compare}
mkdir -p $out
cc=(${CC:-cc} -std=c2x -O2 -DNDEBUG -Wall -Wextra)
# clang outlines repeated sequences of hot loops into calls at -O2 on arm64, which undoes inlining.
print 'int main(void) { return 0; }' | $cc -mno-outline -x c - -o $out/probe 2>/dev/null && cc+=(-mno-outline)
rm -f $out/probe
[[ $OSTYPE == darwin* ]] && cxx=-lc++ || cxx=(-lstdc++ -lm)

$cc -I$root/include -DUSE_CILO $here/compare.c $root/src/cilo.c $root/src/heap.c -o $out/cilo
$cc -I$root/include -DUSE_CILO_CALL $here/compare.c $root/src/cilo.c $root/src/heap.c -o $out/cilo-call
$cc $here/compare.c -o $out/libc
[[ -d $comp/mimalloc-build ]] && $cc -DUSE_MIMALLOC -I$comp/src/mimalloc/include $here/compare.c \
  $comp/mimalloc-build/libmimalloc.a -o $out/mimalloc
[[ -d $comp/jemalloc-build ]] && $cc -DUSE_JEMALLOC -I$comp/jemalloc-build/include $here/compare.c \
  $comp/jemalloc-build/lib/libjemalloc.a -lm -o $out/jemalloc
[[ -d $comp/snmalloc-build ]] && $cc -DUSE_SNMALLOC $here/compare.c $comp/snmalloc-build/libsnmallocshim-static.a \
  $cxx -o $out/snmalloc
[[ -d $comp/src/rpmalloc ]] && $cc -D_GNU_SOURCE -DUSE_RPMALLOC -DENABLE_OVERRIDE=0 -I$comp/src/rpmalloc/rpmalloc $here/compare.c \
  $comp/src/rpmalloc/rpmalloc/rpmalloc.c -o $out/rpmalloc

# rounds interleave the allocators so background load hits them alike, each column keeps its best.
rounds=${3:-3}
bins=(cilo cilo-call mimalloc jemalloc snmalloc rpmalloc libc)
results=()
for round in {1..$rounds}; do
  for bin in $bins; do
    [[ -x $out/$bin ]] && results+=("$($out/$bin)\t$($out/$bin memory)")
  done
done
print "allocator\tstruct\tmixed\tchurn\tmedium\t8 threads\tcross thread\tlarge\tpeak MiB"
print -l $results | awk -F'\t' -v OFS='\t' '
  !($1 in seen) { seen[$1] = 1; order[++n] = $1 }
  { for (i = 2; i <= NF; i++) if (!(($1, i) in best) || $i + 0 < best[$1, i] + 0) best[$1, i] = $i; cols = NF }
  END { for (k = 1; k <= n; k++) { line = order[k]; for (i = 2; i <= cols; i++) line = line OFS best[order[k], i]; print line } }'

