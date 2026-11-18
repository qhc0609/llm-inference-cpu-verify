#!/bin/bash
# run_dma.sh - build and run the six DMA/shared-memory experiments.
set -u
cd "$(dirname "$0")"
OUT=${1:-results_dma}
mkdir -p "$OUT"
CC=${CC:-gcc}
CFLAGS=${CFLAGS:--O3 -march=native -mclflushopt -std=gnu11}
restore() { docker unpause ohos-compile-env >/dev/null 2>&1; }
trap restore EXIT INT TERM
docker pause ohos-compile-env >/dev/null 2>&1 && sleep 2 && echo "(compile container paused)" || echo "(no compile container)"

$CC $CFLAGS 20b_copy.c  -o 20b_copy  -lpthread -lm || exit 1
$CC $CFLAGS 20c_cache.c -o 20c_cache -lm || exit 1
$CC $CFLAGS 20d_pin.c   -o 20d_pin   -lm || exit 1
$CC $CFLAGS 20e_path.c  -o 20e_path  -lpthread -lm || exit 1
$CC -O2 20f_dmabuf.c    -o 20f_dmabuf || exit 1

bash 20a_topo.sh > "$OUT/topo.txt" 2>&1

for i in 1 2 3 4 5; do
  echo "RES|run|copy|$i"  >> "$OUT/copy.txt"
  ./20b_copy 0           >> "$OUT/copy.txt" 2>&1
done
for i in 1 2 3 4 5; do
  echo "RES|run|cache|$i" >> "$OUT/cache.txt"
  ./20c_cache 0          >> "$OUT/cache.txt" 2>&1
done
for i in 1 2 3 4 5; do
  echo "RES|run|pin|$i"   >> "$OUT/pin.txt"
  ./20d_pin 0            >> "$OUT/pin.txt" 2>&1
done
for i in 1 2 3 4 5; do
  echo "RES|run|path|$i"  >> "$OUT/path.txt"
  ./20e_path 0           >> "$OUT/path.txt" 2>&1
done
for i in 1 2 3 4 5; do
  echo "RES|run|dmabuf|$i" >> "$OUT/dmabuf.txt"
  ./20f_dmabuf 0          >> "$OUT/dmabuf.txt" 2>&1
done
echo "done -> $OUT"
grep -h '^RES|' "$OUT"/*.txt | wc -l
