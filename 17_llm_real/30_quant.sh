#!/usr/bin/env bash
# 30_quant.sh —— 对应文章 09：量化省了什么、赔了什么
#
# 真模型版：同一份 Qwen2.5-0.5B-Instruct，三个档位 q2_k / q4_k_m / q8_0。
# 三个指标必须一起看：
#   体积 —— 文件多大
#   速度 —— pp512 与 tg128（量化不只改体积，还改解包指令数）
#   质量 —— 同一份语料上的困惑度（越低越好）
#
# 只报体积会得出"越小越好"；只报速度会漏掉质量塌方。
# 文章里"INT4 比 FP32 慢"那个反直觉结论，这里用真模型重新验一遍。

set -u
. "$(dirname "$0")/lib.sh"

HERE=$(cd "$(dirname "$0")" && pwd)
CORPUS=$OUT/corpus.txt
[ -f "$CORPUS" ] || { echo "缺语料，先跑 05_corpus.sh"; exit 1; }
echo "语料 $(stat -c%s "$CORPUS") 字节"

for m in "$Q2" "$Q4" "$Q8"; do
  [ -f "$m" ] || { echo "跳过（缺文件）: $m"; continue; }
  tag=$(basename "$m" .gguf | sed 's/qwen2.5-0.5b-instruct-//')
  sz=$(stat -c%s "$m")
  echo "===== $tag  $(( sz / 1048576 )) MB ====="

  "$BIN/llama-bench" -m "$m" -p 512 -n 128 -t 4 -r 3 -o json 2>/dev/null > "$OUT/quant_${tag}_bench.json"
  "$BIN/llama-bench" -m "$m" -p 512 -n 128 -t 4 -r 3 -o md   2>/dev/null > "$OUT/quant_${tag}_bench.md"

  "$BIN/llama-perplexity" -m "$m" -f "$CORPUS" -c 512 --chunks 20 -t 4 \
      > "$OUT/quant_${tag}_ppl.log" 2>&1

  # 新版输出 "Final estimate: PPL = 12.3456 +/- 0.78901"；老版是 "PPL = x ± y"
  ppl=$(grep -oE "Final estimate: *PPL *= *[0-9.]+" "$OUT/quant_${tag}_ppl.log" \
          | grep -oE "[0-9.]+$" | tail -1)
  [ -n "${ppl:-}" ] || ppl=$(grep -oE "PPL *= *[0-9.]+" "$OUT/quant_${tag}_ppl.log" \
          | grep -oE "[0-9.]+" | tail -1)
  err=$(grep -oE "PPL *= *[0-9.]+ *[+±/-]+ *[0-9.]+" "$OUT/quant_${tag}_ppl.log" \
          | grep -oE "[0-9.]+$" | tail -1)

  python3 "$HERE/bench_parse.py" quant "$OUT/quant_${tag}_bench.json" "$tag" \
      "$(awk "BEGIN{printf \"%.1f\", $sz/1048576}")" "${ppl:-NA}" "${err:-NA}"
done

echo "--- 明细 $OUT/quant_* ---"
grep -h "Final estimate" "$OUT"/quant_*_ppl.log 2>/dev/null
