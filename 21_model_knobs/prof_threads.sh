#!/usr/bin/env bash
# prof_threads.sh —— 3 线程与 4 线程各采一次样，看同步占比怎么会跳上去。
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=${BIN:-/root/llm/llama.cpp/build/bin}
MODELS=${MODELS:-/root/llm/models}
OUT=$HERE/results
mkdir -p "$OUT"
Q4=$MODELS/qwen2.5-0.5b-instruct-q4_k_m.gguf

for t in 3 4; do
    perf record -e cpu-clock -F 499 -o "$OUT/rec_tg_t$t.data" -- \
        "$BIN/llama-bench" -m "$Q4" -t "$t" -p 0 -n 128 -r 6 -o md \
        > "$OUT/bench_prof_tg_t$t.txt" 2>&1
    perf report -i "$OUT/rec_tg_t$t.data" --stdio --no-children -s symbol 2>/dev/null \
        | grep -E "^\s+[0-9]+\.[0-9]+%" | head -12 > "$OUT/report_tg_t$t.txt"
    echo "===== -t $t ====="
    cat "$OUT/report_tg_t$t.txt"
done
