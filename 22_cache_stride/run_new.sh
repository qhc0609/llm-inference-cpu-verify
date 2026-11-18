#!/bin/bash
# run_new.sh - the three new benchmarks (miss-rate model, decode budget, reuse).
# Compile-time and run-time are wrapped the same way as run_all.sh: the
# OpenHarmony compile container is paused so the machine is otherwise idle.
set -u
cd "$(dirname "$0")"
OUT=${1:-results_new}
mkdir -p "$OUT"
CC=${CC:-gcc}
CFLAGS=${CFLAGS:--O3 -march=native -std=gnu11}

restore() { docker unpause ohos-compile-env >/dev/null 2>&1; }
trap restore EXIT INT TERM

echo "== pause compile container =="
docker pause ohos-compile-env >/dev/null 2>&1 || echo "(container not running)"
sleep 2
uptime

echo "== perf: what the counters can and cannot do =="
{ perf stat -e cycles,instructions,cache-references,cache-misses true; echo "exit=$?"; } 2>&1 | tee "$OUT/perf_hw.txt"
{ perf stat -e task-clock,page-faults,context-switches true; echo "exit=$?"; } 2>&1 | tee "$OUT/perf_sw.txt"

for src in 10_missrate 11_decode 12_reuse; do
  echo "== build $src =="
  $CC $CFLAGS "$src.c" -o "$src" -lm || exit 1
done

for b in 10_missrate 11_decode 12_reuse; do
  echo "== run $b =="
  ./"$b" | tee "$OUT/$b.txt"
done
echo "== done =="
