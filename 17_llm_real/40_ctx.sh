#!/usr/bin/env bash
# 40_ctx.sh —— 对应文章 08：读得越多，为什么越慢
#
# 真模型版，三条互不替代的证据：
#
#   A. 内存代价：不同 -c 启动，读 llama.cpp 自己报告的 KV cache 尺寸。
#      线性增长，斜率由 层数 × kv_heads × head_dim × 2(K和V) 决定。
#      qwen2.5-0.5B：24 层 × 2 个 KV 头 × 64 维 × 2(K/V) × 2 字节(f16)
#                    = 每 token 12 KiB —— 这个式子可以在纸上先算，再跟实测对。
#   B. 时间代价：llama-bench 的 -d 把 KV cache 预填到指定深度再测 tg。
#      只变"已读进多少历史"，模型、线程、量化都不动。
#   C. 端到端：同一问题，短上下文 vs 长上下文，真跑一次生成。
#
# 注意 A 和 B 在量纲上不同：A 是 MiB，B 是 tokens/s。

set -u
. "$(dirname "$0")/lib.sh"

HERE=$(cd "$(dirname "$0")" && pwd)
MODEL=${MODEL:-$Q4}
echo "模型 $(basename "$MODEL")"

echo "===== A. KV Cache 大小 vs 上下文上限 ====="
# 32768 是 qwen2.5-0.5B 的训练上限，也是 llama-cli 不加 -c 时的默认值：
# 这一档能说明"我什么都没干，只是把默认上下文留着"要花多少内存。
for c in 512 1024 2048 4096 32768; do
  log=$OUT/ctx_kv_$c.log
  "$BIN/llama-cli" -m "$MODEL" -c "$c" -p "x" -n 0 -t 4 -st --no-warmup -v >"$log" 2>&1
  # 新版：llama_kv_cache: size =   12.00 MiB (  1024 cells,  24 layers,  1/1 seqs), K (f16): ...
  kv=$(grep -oE "llama_kv_cache: size = *[0-9.]+ MiB" "$log" | grep -oE "[0-9.]+" | head -1)
  cells=$(grep -oE "\( *[0-9]+ cells" "$log" | grep -oE "[0-9]+" | head -1)
  buf=$(grep -oE "compute buffer size *= *[0-9.]+ MiB" "$log" | grep -oE "[0-9.]+" | head -1)
  res "kv_cache|ctx=$c|kv_mib=${kv:-?}|cells=${cells:-?}|compute_buf_mib=${buf:-?}"
done

echo "===== B. KV 深度 vs 解码速度 ====="
"$BIN/llama-bench" -m "$MODEL" -p 0 -n 128 -d 0,512,1024,2048,4096 -t 4 -r 3 -o json 2>/dev/null \
    > "$OUT/ctx_depth.json"
"$BIN/llama-bench" -m "$MODEL" -p 0 -n 128 -d 0,512,1024,2048,4096 -t 4 -r 3 -o md 2>/dev/null \
    > "$OUT/ctx_depth.md"
python3 "$HERE/bench_parse.py" scan "$OUT/ctx_depth.json" depth

echo "===== C. 同一问题，短上下文 vs 长上下文 ====="
for c in 256 4096; do
  log=$OUT/ctx_gen_$c.log
  "$BIN/llama-cli" -m "$MODEL" -c "$c" -t 4 -n 64 --temp 0 -s 42 -st \
      -p "用一句话说明：为什么大模型读的历史越长，生成越慢？" > "$log" 2>&1
  tg=$(grep -oE "Generation: *[0-9.]+ t/s" "$log" | grep -oE "[0-9.]+" | tail -1)
  pp=$(grep -oE "Prompt: *[0-9.]+ t/s" "$log" | grep -oE "[0-9.]+" | tail -1)
  res "gen_ctx|ctx=$c|pp_tps=${pp:-?}|tg_tps=${tg:-?}"
done

echo "--- 明细 $OUT/ctx_* ---"
