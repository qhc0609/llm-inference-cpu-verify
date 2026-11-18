#!/bin/bash
# rep.sh - the same A/B pair, five times, alternating, so that "difference" and
# "noise" can be told apart. Every line is RES|ab|<test>|<rep-cfg>|...
#
#   bash rep.sh results_ab
#
# Alternating A/B/A/B matters here: a plain "run A then run B" cannot separate a
# real effect from a drift (host load, thermal, hypervisor scheduling). With the
# pairs interleaved, the gap between configurations can be read against the
# spread inside one configuration.
set -u
cd "$(dirname "$0")"
OUT=${1:-results_ab}
REPS=${REPS:-5}
mkdir -p "$OUT"
CC=${CC:-gcc}
CFLAGS=${CFLAGS:--O3 -march=native -std=gnu11}

restore() { docker unpause ohos-compile-env >/dev/null 2>&1; }
trap restore EXIT INT TERM
docker pause ohos-compile-env >/dev/null 2>&1 && sleep 2 || echo "(no compile container)"

for src in 19b_latency 19f_decode 13_streambw; do
  $CC $CFLAGS -ffast-math "$src.c" -o "$src" -lpthread -lm || exit 1
done

A0=$(cat /sys/devices/system/node/node0/cpulist | sed 's/[-,].*//')
A1=$(cat /sys/devices/system/node/node0/cpulist | sed 's/.*[-,]//')
B0=$(cat /sys/devices/system/node/node1/cpulist | sed 's/[-,].*//')

# emit <test> <tag> <command...> : rewrite "RES|xxx|" into "RES|ab|test|tag|"
emit() {
  local test=$1 tag=$2; shift 2
  "$@" | sed "s/^RES|[a-z]*|/RES|ab|$test|$tag|/" | tee -a "$OUT/ab.txt"
}

echo "== 1. latency: 64 MiB ring on node0 vs node1, cpu always on node0 =="
for r in $(seq 1 "$REPS"); do
  emit lat "r$r-m0" numactl --membind=0 --physcpubind=$A0 ./19b_latency 64 m0
  emit lat "r$r-m1" numactl --membind=1 --physcpubind=$A0 ./19b_latency 64 m1
done

echo "== 2. read bandwidth, 2 threads: memory node0 vs node1 =="
for r in $(seq 1 "$REPS"); do
  emit bw2 "r$r-m0" env BW_MAXT=2 numactl --membind=0 --physcpubind=$A0,$A1 ./13_streambw
  emit bw2 "r$r-m1" env BW_MAXT=2 numactl --membind=1 --physcpubind=$A0,$A1 ./13_streambw
done

echo "== 3. decode 2 threads: weights node0 vs node1, workers on node0 =="
for r in $(seq 1 "$REPS"); do
  emit dec "r$r-m0" numactl --membind=0 --physcpubind=$A0,$A1 ./19f_decode 2
  emit dec "r$r-m1" numactl --membind=1 --physcpubind=$A0,$A1 ./19f_decode 2
done

echo "== 4. decode 2 threads: both workers local vs one worker per node =="
for r in $(seq 1 "$REPS"); do
  emit dec "r$r-local" numactl --membind=0 --physcpubind=$A0,$A1 ./19f_decode 2
  emit dec "r$r-split" numactl --membind=0 --physcpubind=$A0,$B0 ./19f_decode 2
done

echo "== done =="
