#!/system/bin/sh
# 在 OpenHarmony 设备侧跑完所有微基准，结果写成 RES| 行
# 用法（宿主机）：hdc shell sh /data/local/tmp/oh_bench_run.sh
set -u
BIN=/data/local/tmp/oh_bench
OUT=/data/local/tmp/oh_results
rm -rf "$OUT"; mkdir -p "$OUT"

echo "=== 环境自证 ==="
uname -a
echo "nproc: $(nproc 2>/dev/null || echo unknown)"
cat /proc/version
for c in 0 1 2 3; do
  d=/sys/devices/system/cpu/cpu$c/cache
  [ -d "$d" ] && for i in "$d"/index*; do
    echo "CACHE|cpu$c/$(basename $i)|$(cat $i/level 2>/dev/null)|$(cat $i/type 2>/dev/null)|$(cat $i/size 2>/dev/null)"
  done
done | sort -u
echo "THP: $(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo none)"
echo "GOV: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo none)"
echo

run() {
  tag="$1"; shift; bin="$1"; shift
  echo "=== $tag ==="
  if [ ! -x "$bin" ]; then echo "SKIP $bin"; return; fi
  "$bin" "$@" > "$OUT/$tag.txt" 2>&1
  echo "rc=$?"
  cat "$OUT/$tag.txt"
  echo
}

run 01_topology        "$BIN/topology"
run 03_cache           "$BIN/cache_lat" 4
run 04_pipeline        "$BIN/branch"
run 06_infer_flow      "$BIN/mini_transformer" 64
run 07_gemm            "$BIN/gemm_block"
run 08_attention       "$BIN/attn"
run 09_quant           "$BIN/quant_gemv"
run 10_prefill_decode  "$BIN/pd"
run 11_thread          "$BIN/thread_cost"
run 12_sched           "$BIN/sched_lat"
run 13_affinity        "$BIN/affinity"
run 14_biglittle       "$BIN/capacity"
run 15_openmp          "$BIN/omp_scale"
run 16_mmap_load       "$BIN/mmap_load"
run 18_memcpy          "$BIN/memcpy_recheck"
run 18_thread_scale    "$BIN/thread_scale"
run 18_affinity_rc     "$BIN/affinity_recheck"
run 19_noise_floor     "$BIN/noise_floor"
run 19_cache_dip       "$BIN/cache_dip"
run 19_cold_warm       "$BIN/cold_warm"
run 19_false_share     "$BIN/false_share"
run 19_thread_proc     "$BIN/thread_proc"
run 19_quant_lut       "$BIN/quant_lut"
run 19_attn_scale      "$BIN/attn_scale"

echo "=== 汇总 ==="
grep -h "^RES|" "$OUT"/*.txt 2>/dev/null | sort -u | tee "$OUT/summary.txt"
