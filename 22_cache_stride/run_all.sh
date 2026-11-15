#!/bin/bash
# run_all.sh - build and run every cache / memory access microbenchmark.
#
# discipline: one benchmark at a time, no other load on the machine,
# every case does one warm-up pass and three timed passes (median).
#
# usage: ./run_all.sh [outdir]
set -u
cd "$(dirname "$0")"
OUT=${1:-results}
mkdir -p "$OUT"

CC=${CC:-gcc}
CFLAGS=${CFLAGS:--O3 -march=native -std=gnu11}
BINS="01_workset 02_stride 03_locality 04_conflict 05_chase 06_prefetch 07_layout 08_gemm 09_threads"

echo "== compiler =="
$CC --version | head -1
echo "== kernel =="
uname -srm
echo "== caches =="
for i in 0 1 2 3; do
  d=/sys/devices/system/cpu/cpu0/cache/index$i
  [ -d "$d" ] && echo "index$i level=$(cat $d/level) $(cat $d/type) size=$(cat $d/size) ways=$(cat $d/ways_of_associativity) line=$(cat $d/coherency_line_size) sets=$(cat $d/number_of_sets) shared=$(cat $d/shared_cpu_list)"
done

for b in $BINS; do
  src=$(ls ${b}*.c 2>/dev/null | head -1)
  [ -z "$src" ] && continue
  echo "== build $src =="
  $CC $CFLAGS "$src" -o "$b" -lpthread -lm || exit 1
done

for b in $BINS; do
  [ -x "$b" ] || continue
  echo "== run $b =="
  ./"$b" | tee "$OUT/$b.txt"
done

echo "== done =="
