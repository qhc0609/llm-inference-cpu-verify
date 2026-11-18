#!/bin/bash
# run_bw.sh - the five bandwidth / roofline benchmarks (13..17).
# Same discipline as run_all.sh: pause the OpenHarmony compile container so the
# machine is otherwise idle, restore it on any exit path.
set -u
cd "$(dirname "$0")"
OUT=${1:-results}
mkdir -p "$OUT"
CC=${CC:-gcc}
CFLAGS=${CFLAGS:--O3 -march=native -std=gnu11}

restore() { docker unpause ohos-compile-env >/dev/null 2>&1; }
trap restore EXIT INT TERM

echo "== pause compile container =="
docker pause ohos-compile-env >/dev/null 2>&1 || echo "(container not running)"
sleep 2
uptime
echo "== cache hierarchy =="
for i in 0 1 2 3; do
  d=/sys/devices/system/cpu/cpu0/cache/index$i
  [ -d "$d" ] && echo "L$(cat $d/level) $(cat $d/type) size=$(cat $d/size) ways=$(cat $d/ways_of_associativity) line=$(cat $d/coherency_line_size) shared=$(cat $d/shared_cpu_list)"
done
echo "== model name =="
grep -m1 "model name" /proc/cpuinfo

for src in 13_streambw 14_latency_mlp 15_peakflops 16_ai_sweep 17_batch; do
  echo "== build $src =="
  $CC $CFLAGS "$src.c" -o "$src" -lpthread -lm || exit 1
done

for b in 13_streambw 14_latency_mlp 15_peakflops 16_ai_sweep 17_batch; do
  echo "== run $b =="
  ./"$b" | tee "$OUT/$b.txt"
done
echo "== done =="
