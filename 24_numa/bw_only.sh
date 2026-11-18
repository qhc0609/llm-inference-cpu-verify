#!/bin/bash
# bw_only.sh - STREAM five times over the whole machine, keep every line.
# The article uses max-per-thread-count: on a shared host the slow runs measure
# the host, not the memory controller.
set -u
cd "$(dirname "$0")"
OUT=${1:-results_bw}
mkdir -p "$OUT"
CC=${CC:-gcc}
CFLAGS=${CFLAGS:--O3 -march=native -std=gnu11}
restore() { docker unpause ohos-compile-env >/dev/null 2>&1; }
trap restore EXIT INT TERM
docker pause ohos-compile-env >/dev/null 2>&1 && sleep 2 || echo "(no compile container)"
$CC $CFLAGS -ffast-math 13_streambw.c -o 13_streambw -lpthread -lm || exit 1
ALL=$(cat /sys/devices/system/node/node*/cpulist | tr '\n' ',' | sed 's/,$//')
for i in 1 2 3 4 5; do
  echo "RES|bwrun|$i"
  numactl --membind=0 --physcpubind=$ALL ./13_streambw
done | tee "$OUT/bw_4cpu.txt"
