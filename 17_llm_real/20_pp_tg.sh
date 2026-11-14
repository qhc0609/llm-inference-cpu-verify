#!/usr/bin/env bash
# 20_pp_tg.sh —— 对应文章 10（Prefill 与 Decode）与文章 15（线程规模）
#
# 真模型版。用 llama-bench 在真实权重上测三件事：
#   pp = prompt processing，一次喂 N 个 token，矩阵是"瘦高"的，算力喂得饱
#   tg = text generation，一次算一个 token，矩阵退化成向量，纯搬权重
#   线程数从 1 到 4 时，这两件事各自怎么变（一个吃算力，一个吃带宽）
#
# 微基准版是自己写 GEMM 模拟这个差别；这里是真的跑 qwen2.5，
# 数字由 llama.cpp 自己统计，不是我们算出来的。
#
# 每个配置只跑一次 llama-bench（-o json），人类可读的表格由 RES 行生成，
# 不再为了 .md 表格重跑一遍 —— 那是白花一倍时间。

set -u
. "$(dirname "$0")/lib.sh"

HERE=$(cd "$(dirname "$0")" && pwd)
MODEL=${MODEL:-$Q4}
echo "模型 $(basename "$MODEL")  $(( $(stat -c%s "$MODEL") / 1048576 )) MB"

scan() { python3 "$HERE/bench_parse.py" scan "$1" "$2"; }

# --- A. 线程规模 1 / 2 / 4 ---
"$BIN/llama-bench" -m "$MODEL" -p 512 -n 128 -t 1,2,4 -r 3 -o json 2>/dev/null > "$OUT/bench_threads.json"
scan "$OUT/bench_threads.json" threads

# --- B. 预填充长度扫描：一次喂 16 / 64 / 256 / 512 / 1024 个 token ---
"$BIN/llama-bench" -m "$MODEL" -p 16,64,256,512,1024 -n 0 -t 4 -r 3 -o json 2>/dev/null > "$OUT/bench_pp_scan.json"
scan "$OUT/bench_pp_scan.json" pp_scan

# --- C. 生成长度扫描：16 / 64 / 256 个 token ---
"$BIN/llama-bench" -m "$MODEL" -p 0 -n 16,64,256 -t 4 -r 3 -o json 2>/dev/null > "$OUT/bench_tg_scan.json"
scan "$OUT/bench_tg_scan.json" tg_scan

# --- D. 权重重排（repack）到底值多少 ---
# llama.cpp 默认把量化权重在内存里重新排成适合向量化乘加的布局，
# 代价是多占一份内存（见 10_load.sh），收益是每条指令能多喂几个权重。
for rp in 1 0; do
  "$BIN/llama-bench" -m "$MODEL" -p 512 -n 128 -t 4 -r 3 --repack "$rp" -o json 2>/dev/null \
      > "$OUT/bench_repack_$rp.json"
  scan "$OUT/bench_repack_$rp.json" "repack$rp"
done

echo "--- 明细 $OUT/bench_*.json ---"
