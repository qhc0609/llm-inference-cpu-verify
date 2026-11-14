#!/usr/bin/env bash
# 10_load.sh —— 对应文章 16：模型文件到底是怎么进内存的
#
# 真模型版：直接加载 644 MB 的 qwen2.5-0.5b-instruct-q8_0.gguf。
#
# 四组 × 冷热 × 2 次：
#   --load-mode mmap（映射文件） / none（整份读进堆）
#   × 默认开启权重重排（repack） / --no-repack
#
# 为什么把 -c 压到 2048：
#   不压的话，llama-cli 会按训练上限 32768 开 KV cache，光 KV 就 384 MiB，
#   比 644 MB 的权重一半还多，把想看的"权重怎么进内存"完全盖住。
#   KV 随上下文增长这一段单独放在 40_ctx.sh 里测。
#
# 指标分两类，因为它们回收代价不同：
#   rssfile —— 文件页，内核随时能丢，丢了再从磁盘读
#   rssanon —— 匿名页，没有磁盘副本，只能换页或者 OOM

set -u
. "$(dirname "$0")/lib.sh"

HERE=$(cd "$(dirname "$0")" && pwd)
MODEL=${MODEL:-$Q8}
CTX=${CTX:-2048}
SZ=$(stat -c%s "$MODEL" 2>/dev/null || echo 0)
echo "模型 $(basename "$MODEL")  $((SZ/1048576)) MB  ctx=$CTX"

tok() { echo "$1" | grep -oE "[|]$2=[0-9.]+" | head -1 | cut -d= -f2; }
mb()  { awk "BEGIN{printf \"%.1f\", ${1:-0}/1024}"; }

bufsum() { grep -oE "$1 model buffer size = *[0-9.]+ MiB" "$2" 2>/dev/null \
             | grep -oE '[0-9.]+ MiB' | awk '{s+=$1} END{printf "%.0f", s+0}'; }
kvsum()  { grep -oE "CPU KV buffer size = *[0-9.]+ MiB" "$1" 2>/dev/null \
             | grep -oE '[0-9.]+ MiB' | awk '{s+=$1} END{printf "%.0f", s+0}'; }

run() {
  local mode=$1 repack=$2 cold=$3 rep=$4
  local name="lm_${mode}_rp_${repack}_$( [ "$cold" = 1 ] && echo cold || echo hot )_$rep"
  local m
  [ "$cold" = 1 ] && drop_cache
  local args=(-m "$MODEL" -c "$CTX" -p "hello" -n 0 -t 4 -st --no-warmup -v --load-mode "$mode")
  [ "$repack" = 0 ] && args+=(--no-repack)
  m=$(python3 "$HERE/measure.py" "$OUT/load_$name.out" "$OUT/load_$name.err" -- \
        "$BIN/llama-cli" "${args[@]}")
  local rc file anon total wall mapped heap kv
  rc=$(tok "$m" rc)
  if [ "${rc:-1}" != 0 ]; then
    res "load|$name|FAILED|rc=${rc:-?}|see_load_err"
    return
  fi
  file=$(mb "$(tok "$m" rssfile_kb)")
  anon=$(mb "$(tok "$m" rssanon_kb)")
  total=$(mb "$(tok "$m" rss_kb)")
  wall=$(tok "$m" wall_s)
  mapped=$(bufsum "CPU_Mapped" "$OUT/load_$name.err")
  heap=$(bufsum "CPU" "$OUT/load_$name.err")
  kv=$(kvsum "$OUT/load_$name.err")
  res "load|$name|rssfile_mb=$file|rssanon_mb=$anon|rss_mb=$total|wall_s=${wall:-?}|file_mb=$((SZ/1048576))|mapped_mib=${mapped:-0}|heap_mib=${heap:-0}|kvbuf_mib=${kv:-0}"
}

for rep in 1 2; do
  for mode in mmap none; do
    for rp in 1 0; do
      run "$mode" "$rp" 1 "$rep"     # 冷：先清 page cache
      run "$mode" "$rp" 0 "$rep"     # 热：紧接着再跑一次
    done
  done
done

echo "--- 原始日志在 $OUT/load_lm_*.err ---"
grep -hoE "(CPU_Mapped|CPU) model buffer size = *[0-9.]+ MiB" \
     "$OUT"/load_lm_mmap_rp_1_hot_1.err | head -3
