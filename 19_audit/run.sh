#!/bin/bash
# 19_audit · 环境审计与结论复测
# 用法：OUT=/root/audit/results bash run.sh
set -u
cd "$(dirname "$0")"
OUT="${OUT:-$PWD/results}"
mkdir -p "$OUT" /root/audit
CC=${CC:-gcc}
CFLAGS="-O2 -std=gnu11 -Wall -Wextra"

echo "#### 环境快照 ####"
{
  echo "RES|env|kernel=$(uname -r)|nproc=$(nproc)|cpu=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2 | sed 's/^ *//')"
  echo "RES|env|mhz=$(grep -m1 'cpu MHz' /proc/cpuinfo | awk '{print $4}')|thp=$(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null)"
  echo "RES|env|gcc=$($CC -dumpversion)|loadavg=$(cut -d' ' -f1-3 /proc/loadavg)"
  echo "RES|env|steal=$(awk '/^cpu /{print $9}' /proc/stat)|page_size=$(getconf PAGE_SIZE)"
  echo "RES|env|mem_total_mb=$(awk '/MemTotal/{printf "%d", $2/1024}' /proc/meminfo)"
} | tee "$OUT/00_env.txt"

run_one() {
  local name=$1
  local extra="${2:-}"
  echo "#### $name ####"
  if ! $CC $CFLAGS $extra "$name.c" -o "/root/audit/$name" -lpthread -lm 2>"$OUT/$name.build.log"; then
    echo "RES|build|$name|FAILED"; return
  fi
  "/root/audit/$name" 2>&1 | tee "$OUT/$name.txt"
}

# 先测噪声地板：后面所有结论都要跟它比
run_one noise_floor
run_one cache_dip "-O2"
run_one quant_lut "-O3 -march=native"
run_one false_share
run_one thread_proc
run_one attn_scale
run_one cold_warm

echo "#### 汇总 ####"
cat "$OUT"/*.txt | grep '^RES|' | sort -u > "$OUT/audit_summary.txt"
wc -l < "$OUT/audit_summary.txt" | xargs -I{} echo "RES|summary|lines={}"
