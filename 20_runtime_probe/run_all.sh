#!/usr/bin/env bash
# run_all.sh —— 真模型运行时的 CPU 行为采集
#
# 设计要点：
#   1) 不采程序自己打印的计时，采内核给进程记的账（/proc/<pid>/task/*/stat）
#   2) 阶段拆不开就拆用例：同一用例跑 r 次与跑 1 次，相减即得单次成本，
#      加载、初始化、页表建立这些一次性开销在差分里全部抵消
#   3) perf 同时挂一遍，拿上下文切换、迁移、页错误和函数级采样
#
# 产物：results/*.txt（每个用例一份 RES|JSON + llama-bench 原始输出）

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=${BIN:-/root/llm/llama.cpp/build/bin}
MODELS=${MODELS:-/root/llm/models}
M=${M:-$MODELS/qwen2.5-0.5b-instruct-q4_k_m.gguf}
OUT=$HERE/results
mkdir -p "$OUT"

probe() {  # tag threads p n r
    local tag=$1 th=$2 p=$3 n=$4 r=$5
    python3 "$HERE/probe.py" --tag "$tag" --interval 0.02 \
        --raw "$OUT/$tag.raw" \
        --cmd "$BIN/llama-bench -m $M -t $th -p $p -n $n -r $r -o md" \
        > "$OUT/$tag.txt" 2>&1
    echo "== $tag 完成 ($(grep -o '"cpu_ms": [0-9.]*' "$OUT/$tag.txt" | head -1))"
}

echo "===== 4 线程：预填充 ====="
probe pp4_r5 4 512 0 5
probe pp4_r1 4 512 0 1

echo "===== 4 线程：解码 ====="
probe tg4_r5 4 0 128 5
probe tg4_r1 4 0 128 1

echo "===== 1 线程：预填充 ====="
probe pp1_r3 1 512 0 3
probe pp1_r1 1 512 0 1

echo "===== 1 线程：解码 ====="
probe tg1_r3 1 0 128 3
probe tg1_r1 1 0 128 1

echo "===== perf stat：迁移、页错误、切换 ====="
for spec in "pp4:512:0" "tg4:0:128"; do
    tag=${spec%%:*}; rest=${spec#*:}; p=${rest%%:*}; n=${rest##*:}
    perf stat -x, -e task-clock,context-switches,cpu-migrations,page-faults \
        -o "$OUT/stat_$tag.txt" -- \
        "$BIN/llama-bench" -m "$M" -t 4 -p "$p" -n "$n" -r 1 -o md \
        > "$OUT/stat_$tag.log" 2>&1
    echo "== stat_$tag 完成"
done

echo "===== perf 采样：CPU 时间花在哪个函数 ====="
for spec in "pp4:512:0" "tg4:0:128"; do
    tag=${spec%%:*}; rest=${spec#*:}; p=${rest%%:*}; n=${rest##*:}
    perf record -g -e cpu-clock -F 499 -o "$OUT/rec_$tag.data" -- \
        "$BIN/llama-bench" -m "$M" -t 4 -p "$p" -n "$n" -r 1 -o md \
        > "$OUT/rec_$tag.log" 2>&1
    perf report --stdio -i "$OUT/rec_$tag.data" --sort dso,symbol -g none \
        --percent-limit 0.8 > "$OUT/report_$tag.txt" 2>&1
    echo "== report_$tag 完成"
done

echo "===== 汇总 ====="
for f in "$OUT"/*.txt; do
    case $f in *report_*) continue;; esac
    echo "--- $(basename "$f") ---"
    grep '^RES|' "$f" | head -1
done
