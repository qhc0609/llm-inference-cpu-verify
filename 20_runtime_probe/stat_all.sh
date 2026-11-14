#!/usr/bin/env bash
# stat_all.sh —— 用 perf 的 task-clock 做差分，替代 /proc 轮询。
#
# 上一版靠 python 每 20ms 读一次 /proc 累加 utime，问题是线程创建/销毁的瞬间会漏计，
# 而"跑 r 次"和"跑 1 次"漏掉的量不同，差分时误差被放大 r-1 倍。
# perf 的 task-clock 由内核在上下文切换时精确累加，不吃这个亏。
#
# 仍然保留 r 次与 1 次的差分：加载、初始化、建页表的一次性开销照样抵消。
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=${BIN:-/root/llm/llama.cpp/build/bin}
MODELS=${MODELS:-/root/llm/models}
OUT=$HERE/results
mkdir -p "$OUT"

Q4=$MODELS/qwen2.5-0.5b-instruct-q4_k_m.gguf
Q2=$MODELS/qwen2.5-0.5b-instruct-q2_k.gguf
Q8=$MODELS/qwen2.5-0.5b-instruct-q8_0.gguf

run() {  # tag model threads p n r
    local tag=$1 m=$2 th=$3 p=$4 n=$5 r=$6
    perf stat -x, -e task-clock,context-switches,cpu-migrations,page-faults \
        -o "$OUT/stat_$tag.txt" -- \
        "$BIN/llama-bench" -m "$m" -t "$th" -p "$p" -n "$n" -r "$r" -o md \
        > "$OUT/bench_$tag.txt" 2>&1
    local tc
    tc=$(grep task-clock "$OUT/stat_$tag.txt" | cut -d, -f1)
    echo "== $tag  task-clock=${tc}ms"
}

echo "===== q4_k_m：预填充 ====="
run pp4_r5 "$Q4" 4 512 0 5
run pp4_r1 "$Q4" 4 512 0 1
run pp1_r3 "$Q4" 1 512 0 3
run pp1_r1 "$Q4" 1 512 0 1

echo "===== q4_k_m：解码 ====="
run tg4_r5 "$Q4" 4 0 128 5
run tg4_r1 "$Q4" 4 0 128 1
run tg1_r3 "$Q4" 1 0 128 3
run tg1_r1 "$Q4" 1 0 128 1

echo "===== 量化档位：同一条 decode ====="
run tgq2_r5 "$Q2" 4 0 128 5
run tgq2_r1 "$Q2" 4 0 128 1
run tgq8_r5 "$Q8" 4 0 128 5
run tgq8_r1 "$Q8" 4 0 128 1

echo "===== 汇总 ====="
for f in "$OUT"/stat_*.txt; do
    b=$(basename "$f" .txt)
    printf "%-12s %s\n" "${b#stat_}" "$(grep task-clock "$f" | cut -d, -f1)"
done
