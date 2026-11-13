#!/usr/bin/env bash
# 一键跑完所有验证，结果落到 results/
# 用法： ./run_all.sh
set -u
ROOT="$(cd "$(dirname "$0")" && pwd)"
OUT="$ROOT/results"
mkdir -p "$OUT"

exec > >(tee "$OUT/all.log") 2>&1

echo "================ 环境自证 ================"
date -Is
. /etc/os-release 2>/dev/null && echo "OS: $PRETTY_NAME"
uname -a
echo "CPU: $(nproc) 核"
grep -m1 "model name" /proc/cpuinfo || true
echo "gcc: $(gcc --version | head -1)"
echo

run() {
  local tag="$1"; shift
  local bin="$1"; shift
  echo "================ $tag ================"
  if [ ! -x "$bin" ]; then echo "SKIP: $bin 不存在"; return; fi
  local t0=$(date +%s%N)
  "$bin" "$@" 2>&1 | tee "$OUT/$tag.txt"
  local t1=$(date +%s%N)
  echo "WALL $(( (t1-t0)/1000000 )) ms" | tee -a "$OUT/$tag.txt"
  echo
}

run 01_topology        "$ROOT/01_topology/topology"
run 03_cache           "$ROOT/03_cache/cache_lat" 4
run 04_pipeline        "$ROOT/04_pipeline/branch"
run 06_infer_flow      "$ROOT/06_infer_flow/mini_transformer" 64
run 07_gemm            "$ROOT/07_gemm/gemm_block"
run 08_attention       "$ROOT/08_attention/attn"
run 09_quant           "$ROOT/09_quant/quant_gemv"
run 10_prefill_decode  "$ROOT/10_prefill_decode/pd"
run 11_thread          "$ROOT/11_thread/thread_cost"
run 12_sched           "$ROOT/12_sched/sched_lat"
run 13_affinity        "$ROOT/13_affinity/affinity"
run 14_biglittle       "$ROOT/14_biglittle/capacity"
run 15_openmp          "$ROOT/15_openmp/omp_scale"
run 16_mmap_load       "$ROOT/16_mmap_load/mmap_load"

echo "================ 02_c2arm64 ================"
bash "$ROOT/02_c2arm64/build.sh" 2>&1 | tee "$OUT/02_c2arm64.txt"
echo

echo "================ 汇总所有 RES 行 ================"
grep -h "^RES|" "$OUT"/*.txt 2>/dev/null | sort | tee "$OUT/summary.txt"
echo
echo "完成。结果目录: $OUT"
