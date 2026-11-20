#!/bin/bash
# 22run.sh - 第 22 篇全部实验，一次跑完，结果落 results22/
cd /root/lab22 || exit 1
OUT=results22
mkdir -p "$OUT"

{
  date -Is
  head -1 /proc/loadavg
  grep -m1 "model name" /proc/cpuinfo
  grep -m1 MemTotal /proc/meminfo
  lscpu | grep -E "^L[123]" | tr -s ' '
  echo "lightdm=$(systemctl is-active lightdm)"
  gcc --version | head -1
  echo "peak=$(./22gemm peak 40000000)"
} > "$OUT/env.txt"

# ---- 1. 优化阶梯（512^3，单线程，7 次取最好）
: > "$OUT/ladder.csv"
for k in naive ikj simd1 t1 t2 t4 t6 t8 t4u2 asm block; do
  ./22gemm bench "$k" 512 512 512 1 7 >> "$OUT/ladder.csv"
done

# ---- 2. 关掉自动向量化做对照（证明编译器也出了一半力）
: > "$OUT/novec.csv"
for k in naive ikj simd1 t4; do
  ./22gemm_novec bench "$k" 512 512 512 1 5 >> "$OUT/novec.csv"
done

# ---- 3. 大矩阵：不分块 vs 分块
: > "$OUT/big.csv"
./22gemm bench t4    1024 1024 1024 1 5 >> "$OUT/big.csv"
./22gemm bench block 1024 1024 1024 1 5 >> "$OUT/big.csv"
./22gemm bench t4    896  896  896  1 5 >> "$OUT/big.csv"
./22gemm bench block 896  896  896  1 5 >> "$OUT/big.csv"

# ---- 4. 分块参数扫描（1024^3，MC x KC x NC）
./22gemm block 1024 1024 1024 3 > "$OUT/block.csv"

# ---- 5. 多线程
: > "$OUT/thr.csv"
for t in 1 2 3 4 6 8; do
  ./22gemm bench mt 1024 1024 1024 "$t" 5 >> "$OUT/thr.csv"
done

# ---- 6. 屋顶线：固定 N=K=896（Qwen2.5-0.5B 的隐藏维度），改 M
: > "$OUT/roof.csv"
for m in 1 4 16 64 256 512; do
  ./22gemm bench block "$m" 896 896 1 5 >> "$OUT/roof.csv"
done

# ---- 7. 算子融合：M=N=512，K 从 32 扫到 896
: > "$OUT/fusion.csv"
for k in 32 64 128 256 512 896; do
  ./22gemm fusion 512 512 "$k" 5 >> "$OUT/fusion.csv"
done

# ---- 8. 低精度：Q8_0 量化 GEMV / GEMM
: > "$OUT/int8.csv"
for m in 1 16 64 256; do
  ./22gemm int8 "$m" 896 896 5 >> "$OUT/int8.csv"
done
echo DONE > "$OUT/.done"
