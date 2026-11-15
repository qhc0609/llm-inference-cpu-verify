#!/usr/bin/env bash
# 一键编译全部 15 个 C 验证程序（02 由它自己的脚本编译）
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
CC=${CC:-gcc}
OPT="-O2 -std=gnu11 -I$ROOT"

echo "== 编译器: $($CC --version | head -1)"

cd "$ROOT"
$CC $OPT -o 01_topology/topology        01_topology/topology.c       -lm
$CC $OPT -o 03_cache/cache_lat          03_cache/cache_lat.c         -lm
# 04 必须关掉向量化和条件传送优化，否则编译器会把"分支"抹掉，
# 分支预测就测不出来了——这本身就是文章里要讲的一个坑
$CC $OPT -fno-tree-vectorize -fno-if-conversion -fno-if-conversion2 \
    -o 04_pipeline/branch              04_pipeline/branch.c         -lm
$CC $OPT -o 06_infer_flow/mini_transformer 06_infer_flow/mini_transformer.c -lm
$CC $OPT -o 07_gemm/gemm_block          07_gemm/gemm_block.c         -lm
$CC $OPT -o 08_attention/attn           08_attention/attn.c          -lm
$CC $OPT -o 09_quant/quant_gemv         09_quant/quant_gemv.c        -lm
$CC $OPT -o 10_prefill_decode/pd        10_prefill_decode/pd.c       -lm
$CC $OPT -o 11_thread/thread_cost       11_thread/thread_cost.c      -lpthread
$CC $OPT -o 12_sched/sched_lat          12_sched/sched_lat.c         -lpthread
$CC $OPT -o 13_affinity/affinity        13_affinity/affinity.c       -lpthread
$CC $OPT -o 14_biglittle/capacity       14_biglittle/capacity.c      -lpthread
$CC $OPT -fopenmp -o 15_openmp/omp_scale 15_openmp/omp_scale.c       -lm
$CC $OPT -o 16_mmap_load/mmap_load      16_mmap_load/mmap_load.c     -lm

echo "== 全部编译完成"

# 22_cache_stride 单独编译：它需要 -O3 -march=native（08 例外，必须 -O2），
# 由该目录自己的 run_all.sh 负责，这里不重复。
