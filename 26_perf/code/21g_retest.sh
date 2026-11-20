#!/bin/bash
# 21g_retest.sh - 交叉轮转重测：消除"时段偏差"导致的加速比失真
# 原测法把同一档连跑 5~9 次，如果这一档恰好撞上宿主忙，整档数据全废。
# 这里改成每轮把所有档各跑一次，连跑 ROUNDS 轮，比值才可比。
set -u
OUT=/root/lab21/results21b
mkdir -p "$OUT"
cd /root/lab21 || exit 1

ROUNDS=${1:-7}

{
  echo "=== guard ==="
  date -Is
  uptime
  head -1 /proc/loadavg
  nproc
  docker ps --filter name=ohos-compile-env --format '{{.Names}} {{.Status}}' 2>/dev/null || echo "no-docker-cli"
  systemctl is-active lightdm 2>/dev/null || echo "no-lightdm"
  grep -m1 "cpu MHz" /proc/cpuinfo
} > "$OUT/guard.txt" 2>&1

: > "$OUT/rr.txt"
: > "$OUT/progress.txt"

for r in $(seq 1 "$ROUNDS"); do
  # 1) 依赖链 vs 四路并行：轮流跑，同一轮内相邻
  for m in dep par4; do
    echo "RES|load|$m|$r" >> "$OUT/rr.txt"
    taskset -c 2 ./21b_loads "$m" 60000000 >> "$OUT/rr.txt" 2>&1
  done
  # 2) 线程扩展：所有档位在同一轮内依次跑完
  for t in 1 2 3 4 6 8 12; do
    echo "RES|thread|$t|$r" >> "$OUT/rr.txt"
    taskset -c 0-3 ./21e_threads "$t" 262144 1920 >> "$OUT/rr.txt" 2>&1
  done
  echo "round $r ok $(date +%H:%M:%S) load=$(cut -d' ' -f1 /proc/loadavg)" >> "$OUT/progress.txt"
done

echo ALLDONE >> "$OUT/progress.txt"
