#!/bin/bash
# run25.sh —— 第25篇《CPU计算热点与内存瓶颈定位》跑批
# 安全约束：只在 /tmp/sched25 里活动，不写用户工程目录，不改任何全局参数
OUT=/tmp/sched25/results
BIN=/tmp/sched25/hot25
mkdir -p "$OUT"
say() { echo "### $*"; }

# 每个配置跑 5 遍取最小值：宿主机上还有别的虚拟机，单次数字会被噪声吃掉
rep5() { # rep5 <outfile> <desc> <cmd...>
  echo "### $2"
  echo "# 5 次，取最小值"
  for i in 1 2 3 4 5; do "$@" ; done
}

# ---------------- A Cache 层次：随机指针追逐 ----------------
say "A Cache 层次"
{
  echo "### A 随机指针追逐：工作集从 4 KB 涨到 64 MB"
  echo "# kb n reps ns_per_access；L1 32K / L2 256K / L3 6M 是 i5-7300HQ 的规格"
  echo "# 第二个参数是总访问次数（3000 万次），内部按规模折算圈数"
  for kb in 4 16 32 64 128 256 512 1024 2048 4096 8192 16384 32768 65536; do
    timeout 60 "$BIN" chase "$kb" 30000000 2>&1 | tail -1
  done
} > "$OUT/secA_cache.txt" 2>&1

# ---------------- B 顺序带宽 ----------------
say "B 顺序带宽"
{
  echo "### B 顺序读 / 写 / 拷贝，8 MB 与 128 MB 两档"
  for mb in 8 128; do
    for m in read write copy; do
      for i in 1 2 3; do timeout 60 "$BIN" stream "$mb" "$m" 2>&1 | tail -1; done
    done
  done
} > "$OUT/secB_stream.txt" 2>&1

# ---------------- C GEMV 三种实现 + 多线程 ----------------
say "C GEMV"
{
  echo "### C decode 核心算子：32 MB 权重，每种 5 次取最小"
  for v in scalar avx2 avx2x4 mt:1 mt:2 mt:4; do
    for i in 1 2 3 4 5; do timeout 120 "$BIN" gemv 32 "$v" 2>&1 | tail -1; done
  done
} > "$OUT/secC_gemv.txt" 2>&1

# ---------------- D GEMM 分块与多线程 ----------------
say "D GEMM"
{
  echo "### D prefill 核心算子：256/384/512 三档"
  for n in 256 384 512; do
    for v in naive block:32 block:64 block:128 mt:2 mt:4; do
      for i in 1 2 3; do timeout 120 "$BIN" gemm "$n" "$v" 2>&1 | tail -1; done
    done
  done
} > "$OUT/secD_gemm.txt" 2>&1

# ---------------- E 绑核 ----------------
say "E 绑核"
{
  echo "### E 绑核：同样的多线程 GEMV，钉 1 颗核 vs 钉 4 颗核 vs 不钉"
  echo "## 钉 1 颗核"
  for i in 1 2 3 4 5; do taskset -c 0 timeout 120 "$BIN" gemv 32 mt:4 2>&1 | tail -1; done
  echo "## 钉 4 颗核"
  for i in 1 2 3 4 5; do taskset -c 0-3 timeout 120 "$BIN" gemv 32 mt:4 2>&1 | tail -1; done
  echo "## 不钉"
  for i in 1 2 3 4 5; do timeout 120 "$BIN" gemv 32 mt:4 2>&1 | tail -1; done
} > "$OUT/secE_affinity.txt" 2>&1

# ---------------- F 综合：优化前 vs 优化后 ----------------
say "F 综合对比"
{
  echo "### F 同一条 decode 步骤，优化前（朴素标量、单线程）vs 优化后（AVX2×4 + 4 线程）"
  echo "## before"
  for i in 1 2 3 4 5; do timeout 120 "$BIN" gemv 32 scalar 2>&1 | tail -1; done
  echo "## after"
  for i in 1 2 3 4 5; do taskset -c 0-3 timeout 120 "$BIN" gemv 32 mt:4 2>&1 | tail -1; done
} > "$OUT/secF_before_after.txt" 2>&1

# ---------------- G 回归：重复性与正确性 ----------------
say "G 回归"
{
  echo "### G 回归：同一配置连跑 10 次，看 checksum 是否一致、时间离散度"
  for i in $(seq 1 10); do timeout 120 "$BIN" gemv 32 avx2x4 2>&1 | tail -1; done
  echo "## GEMM 一致性（block 与 naive 的 checksum 必须相同）"
  timeout 120 "$BIN" gemm 256 naive 2>&1 | tail -1
  timeout 120 "$BIN" gemm 256 block:64 2>&1 | tail -1
  timeout 120 "$BIN" gemm 256 mt:4 2>&1 | tail -1
} > "$OUT/secG_regress.txt" 2>&1

say "DONE"
