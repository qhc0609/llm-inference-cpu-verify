#!/bin/bash
# 21h_retest.sh - 用 CPU 时间做主指标的交叉轮转重测
# 关键：每轮输出 PARALLEL（有效并行度 = CPU时间/墙钟），它告诉你这一轮
# 到底有几个核在真干活。PARALLEL 明显低于线程数的轮次直接判废。
set -u
OUT=/root/lab21/results21c
mkdir -p "$OUT"
cd /root/lab21 || exit 1

ROUNDS=${1:-7}

{
  echo "=== guard ==="; date -Is; uptime
  head -1 /proc/loadavg; nproc
  echo "lightdm=$(systemctl is-active lightdm 2>/dev/null)"
  grep -m1 "cpu MHz" /proc/cpuinfo
} > "$OUT/guard.txt" 2>&1

: > "$OUT/rr.txt"; : > "$OUT/progress.txt"

for r in $(seq 1 "$ROUNDS"); do
  for m in dep par4; do
    echo "RES|load|$m|$r" >> "$OUT/rr.txt"
    taskset -c 2 ./21b_loads "$m" 60000000 >> "$OUT/rr.txt" 2>&1
  done
  for t in 1 2 3 4 6 8; do
    echo "RES|thread|$t|$r" >> "$OUT/rr.txt"
    taskset -c 0-3 ./21e_threads "$t" 262144 1920 >> "$OUT/rr.txt" 2>&1
  done
  echo "round $r ok $(date +%H:%M:%S) load=$(cut -d' ' -f1 /proc/loadavg)" >> "$OUT/progress.txt"
done

echo ALLDONE >> "$OUT/progress.txt"
