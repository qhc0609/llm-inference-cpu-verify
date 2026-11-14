#!/usr/bin/env bash
# run_knobs.sh —— 在真模型上逐个拧旋钮，看 CPU 侧的账怎么变。
#
# 和 20_runtime_probe 的区别：那边只回答"一次推理占几颗核"，
# 这边把文章前 15 节里的机制，找回它们在 llama.cpp 里对应的那个旋钮：
#   线程扩展 -> -t           绑核      -> taskset -c
#   超订调度 -> -t 8（4 核）  KV 深度   -> -d
#   批大小   -> -b           权重读取  -> -lm mmap / none
#   量化档位 -> 换 gguf 文件
#
# 每个用例跑 r 次与 1 次，CPU 时间用 perf 的 task-clock（内核在切换时累加），
# 相减得单次计算成本，加载与建页表的一次性开销自动抵消。
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BIN=${BIN:-/root/llm/llama.cpp/build/bin}
MODELS=${MODELS:-/root/llm/models}
OUT=$HERE/results
mkdir -p "$OUT"

Q4=$MODELS/qwen2.5-0.5b-instruct-q4_k_m.gguf
Q2=$MODELS/qwen2.5-0.5b-instruct-q2_k.gguf
Q8=$MODELS/qwen2.5-0.5b-instruct-q8_0.gguf

# run tag model "bench 参数" [taskset 掩码]
run() {
    local tag=$1 m=$2 args=$3 mask=${4:-}
    local pre=""
    [ -n "$mask" ] && pre="taskset -c $mask"
    for r in 4 1; do
        $pre perf stat -x, -e task-clock,context-switches,cpu-migrations,page-faults \
            -o "$OUT/st_${tag}_r${r}.txt" -- \
            "$BIN/llama-bench" -m "$m" $args -r "$r" -o md \
            > "$OUT/bench_${tag}_r${r}.txt" 2>&1
    done
    local tc4 tc1
    tc4=$(grep task-clock "$OUT/st_${tag}_r4.txt" | cut -d, -f1)
    tc1=$(grep task-clock "$OUT/st_${tag}_r1.txt" | cut -d, -f1)
    printf "%-22s tc4=%-10s tc1=%-10s\n" "$tag" "$tc4" "$tc1"
}

echo "===== A 线程扩展：解码 128（4 核机器）====="
for t in 1 2 3 4; do run "tg_t$t" "$Q4" "-t $t -p 0 -n 128"; done
echo "===== B 线程扩展：预填充 512 ====="
for t in 1 2 3 4; do run "pp_t$t" "$Q4" "-t $t -p 512 -n 0"; done

echo "===== C 超订：4 核上开 8 / 16 线程 ====="
run "tg_t8" "$Q4" "-t 8 -p 0 -n 128"
run "tg_t16" "$Q4" "-t 16 -p 0 -n 128"

echo "===== D 绑核：taskset 钉在固定核上 ====="
run "tg_t4_pin" "$Q4" "-t 4 -p 0 -n 128" "0-3"
run "tg_t1_pin" "$Q4" "-t 1 -p 0 -n 128" "0"
run "pp_t4_pin" "$Q4" "-t 4 -p 512 -n 0" "0-3"

echo "===== E 批大小：预填充 512，改 -b ====="
for b in 1 16 64 256 512; do run "pp_b$b" "$Q4" "-t 4 -b $b -p 512 -n 0"; done

echo "===== F KV 深度：解码 128，改 -d ====="
for d in 0 1024 4096; do run "tg_d$d" "$Q4" "-t 4 -d $d -p 0 -n 128"; done

echo "===== G 权重读取方式：mmap 与 read ====="
for lm in mmap none; do run "tg_lm_$lm" "$Q4" "-t 4 -p 0 -n 128" ""
done

echo "===== H 量化档位：解码 128 ====="
for q in q2_k q4_k_m q8_0; do
    case $q in q2_k) f=$Q2;; q4_k_m) f=$Q4;; q8_0) f=$Q8;; esac
    run "tg_$q" "$f" "-t 4 -p 0 -n 128"
done

echo "===== 汇总 ====="
for f in "$OUT"/st_*_r4.txt; do
    b=$(basename "$f" .txt)
    printf "%-24s %s\n" "${b#st_}" "$(grep task-clock "$f" | cut -d, -f1)"
done
