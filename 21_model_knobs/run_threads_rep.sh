#!/usr/bin/env bash
# run_threads_rep.sh —— 线程扩展单独重复跑，用来判断 4 线程相对 3 线程的回退是噪声还是真的。
# 每个配置跑一轮（r=4 与 r=1 相减），换一个后缀存文件；跑三轮后取每配置的最小值。
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=${BIN:-/root/llm/llama.cpp/build/bin}
MODELS=${MODELS:-/root/llm/models}
OUT=$HERE/results
mkdir -p "$OUT"
Q4=$MODELS/qwen2.5-0.5b-instruct-q4_k_m.gguf
SUF=${1:?用法: run_threads_rep.sh 后缀}

run() {  # tag args
    local tag=$1 args=$2
    for r in 4 1; do
        perf stat -x, -e task-clock,context-switches,cpu-migrations,page-faults \
            -o "$OUT/st_${tag}${SUF}_r${r}.txt" -- \
            "$BIN/llama-bench" -m "$Q4" $args -r "$r" -o md \
            > "$OUT/bench_${tag}${SUF}_r${r}.txt" 2>&1
    done
    printf "%-16s %s\n" "$tag$SUF" "$(grep task-clock "$OUT/st_${tag}${SUF}_r4.txt" | cut -d, -f1)"
}

for t in 1 2 3 4; do
    run "tg_t$t" "-t $t -p 0 -n 128"
done
for t in 1 2 3 4; do
    run "pp_t$t" "-t $t -p 512 -n 0"
done
