#!/bin/bash
# 用 OpenHarmony NDK 把工作区里的 24 个微基准交叉编译成 x86_64-linux-ohos 静态可执行文件
#
# 需要的环境变量：
#   OH_SDK  OpenHarmony SDK native 目录（默认 /opt/ohos-sdk/linux/15/native）
#   OUT     输出目录（默认 ./bin）
# 用法：
#   OH_SDK=/path/to/linux/15/native bash ohos/build_oh.sh
set -uo pipefail

SDK=${OH_SDK:-/opt/ohos-sdk/linux/15/native}
SRC=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$(pwd)/bin}
CC=$SDK/llvm/bin/clang

[ -x "$CC" ] || { echo "找不到 clang: $CC（用 OH_SDK 指到 linux/15/native）"; exit 2; }

# 静态链接：不依赖 OpenHarmony 的 musl 动态库，initramfs 里直接能跑
CFLAGS="--target=x86_64-linux-ohos --sysroot=$SDK/sysroot -O2 -std=gnu11 -I$SRC -static"
LDLIBS="-lm"

mkdir -p "$OUT"

ok=0; fail=0
build() {          # build <输出名> <源文件> [额外编译选项...]
    local name="$1"; local src="$2"; shift 2
    if $CC $CFLAGS "$@" -o "$OUT/$name" "$src" $LDLIBS > "$OUT/$name.log" 2>&1; then
        printf 'BUILD|%-20s|OK|%s\n' "$name" "$(stat -c%s "$OUT/$name")"
        ok=$((ok+1))
    else
        printf 'BUILD|%-20s|FAIL|%s\n' "$name" "$(tail -n 1 "$OUT/$name.log" | cut -c1-90)"
        fail=$((fail+1))
    fi
}

build topology         "$SRC/01_topology/topology.c"
build cache_lat        "$SRC/03_cache/cache_lat.c"
# NDK 的 clang 15 不认 -fno-if-conversion2，改用 -fno-if-conversion 关掉同一类优化
build branch           "$SRC/04_pipeline/branch.c" -fno-vectorize -fno-if-conversion
build mini_transformer "$SRC/06_infer_flow/mini_transformer.c"
build gemm_block       "$SRC/07_gemm/gemm_block.c"
build attn             "$SRC/08_attention/attn.c"
build quant_gemv       "$SRC/09_quant/quant_gemv.c"
build pd               "$SRC/10_prefill_decode/pd.c"
build thread_cost      "$SRC/11_thread/thread_cost.c"
build sched_lat        "$SRC/12_sched/sched_lat.c"
build affinity         "$SRC/13_affinity/affinity.c"
build capacity         "$SRC/14_biglittle/capacity.c"
# NDK 自带静态 libomp，-fopenmp 可直接过
build omp_scale        "$SRC/15_openmp/omp_scale.c" -fopenmp
build mmap_load        "$SRC/16_mmap_load/mmap_load.c"
build memcpy_recheck   "$SRC/18_recheck/memcpy_recheck.c"
build thread_scale     "$SRC/18_recheck/thread_scale.c"
build affinity_recheck "$SRC/18_recheck/affinity_recheck.c"
build noise_floor      "$SRC/19_audit/noise_floor.c"
build cache_dip        "$SRC/19_audit/cache_dip.c"
build cold_warm        "$SRC/19_audit/cold_warm.c"
build false_share      "$SRC/19_audit/false_share.c"
build thread_proc      "$SRC/19_audit/thread_proc.c"
build quant_lut        "$SRC/19_audit/quant_lut.c"
build attn_scale       "$SRC/19_audit/attn_scale.c"

echo "SUMMARY|ok=$ok|fail=$fail"
