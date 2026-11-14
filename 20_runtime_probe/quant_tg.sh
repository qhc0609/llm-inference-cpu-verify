#!/usr/bin/env bash
# quant_tg.sh —— 同一句 decode，只换量化档位。
# 目的：看每 token 的 CPU 时间是否严格跟着权重体积走。
# 如果是，decode 就是带宽受限；如果解包开销占主导，曲线会偏离体积比。
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=${BIN:-/root/llm/llama.cpp/build/bin}
MODELS=${MODELS:-/root/llm/models}
OUT=$HERE/results
mkdir -p "$OUT"

for q in q2_k q8_0; do
    M=$MODELS/qwen2.5-0.5b-instruct-$q.gguf
    for r in 5 1; do
        tag=tg4_${q}_r${r}
        python3 "$HERE/probe.py" --tag "$tag" --interval 0.02 \
            --raw "$OUT/$tag.raw" \
            --cmd "$BIN/llama-bench -m $M -t 4 -p 0 -n 128 -r $r -o md" \
            > "$OUT/$tag.txt" 2>&1
        echo "== $tag $(grep -o '"cpu_ms": [0-9.]*' "$OUT/$tag.txt" | head -1)"
    done
done
