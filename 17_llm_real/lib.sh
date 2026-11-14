#!/usr/bin/env bash
# lib.sh —— 真模型实验的公共函数

LLAMA_DIR=${LLAMA_DIR:-/root/llm/llama.cpp}
BIN=$LLAMA_DIR/build/bin
MODELS=${MODELS:-/root/llm/models}
OUT=${OUT:-/root/llm/results}
mkdir -p "$OUT"

Q4=$MODELS/qwen2.5-0.5b-instruct-q4_k_m.gguf
Q8=$MODELS/qwen2.5-0.5b-instruct-q8_0.gguf
Q2=$MODELS/qwen2.5-0.5b-instruct-q2_k.gguf

res() { echo "RES|$*"; }

# 清 page cache：让"冷启动"真的是冷启动。
# 没有这一步，第二次跑接到的全是上一次留在 page cache 里的热数据，
# 测出来的是内存速度，不是磁盘速度。
drop_cache() { sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null; }

# 取进程峰值 RSS（KB）。/usr/bin/time -v 是 procps 提供的，比 /proc 采样准。
peak_rss() { awk -F': ' '/Maximum resident set size/{print $2}' "$1" 2>/dev/null | tr -d ' '; }

# 从 llama.cpp 日志里抠关键字段
grab() { grep -oE "$1 *= *[0-9.]+" "$2" 2>/dev/null | grep -oE "[0-9.]+$" | head -1; }
