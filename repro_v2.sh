#!/bin/bash
# repro_v2.sh —— 第24/25篇在魔搭 DSW（Xeon 8369B / 48MB L3）上的复现
# 与 v1 的差异：
#   1) 本机 SMT 交错（cpu0 sibling = cpu1），四线程必须钉 0,2,4,6 才是 4 个物理核
#   2) 容器内 cgroup 只读，配额实验改用「同核竞争进程」压份额（1/(k+1)），等价且可复现
set -u
ROOT=$(cd "$(dirname "$0")" && pwd)
STAMP=$(date +%Y%m%d_%H%M)
OUT="$ROOT/rerun_$STAMP"
T24=/tmp/s24; T25=/tmp/s25
PIN1=0
PIN4=0,2,4,6
mkdir -p "$OUT" "$T24" "$T25"
say(){ echo "### $*"; }

# ---------- 0 环境自证 ----------
say "环境"
{
  echo "\$ date"; date '+%F %T'
  echo "\$ uname"; uname -r
  echo "\$ os"; grep PRETTY_NAME /etc/os-release
  echo "\$ cpu"; export LC_ALL=C; lscpu | grep -E 'Model name|^CPU\(s\)|Thread|Core\(s\)|Socket|L3|NUMA node'
  echo "nproc=$(nproc)  nproc_all=$(nproc --all)"
  echo "\$ smt"; cat /sys/devices/system/cpu/cpu0/topology/thread_siblings_list
  echo "\$ mem"; free -g | head -2
  echo "\$ gcc"; gcc --version | head -1
  echo "\$ cpufreq"; ls /sys/devices/system/cpu/cpu0/cpufreq 2>&1 | head -2
  echo "\$ thermal"; ls -d /sys/class/thermal/thermal_zone* 2>&1 | head -3
  echo "\$ powercap"; ls /sys/class/powercap 2>&1 | head -3
  echo "\$ msr"; ls /dev/cpu/0/msr 2>&1
  echo "\$ perf"; perf stat -e cycles -- true 2>&1 | tail -2
  echo "\$ cgroup"; stat -fc %T /sys/fs/cgroup; cat /proc/self/cgroup | head -2
  echo "\$ load"; cat /proc/loadavg
} 2>&1 | tee "$OUT/env.txt"

# ---------- 1 编译 ----------
say "编译"
{
  # 统一用 x86-64-v3(AVX2)：-march=native 在 Ice Lake 上会走 AVX-512，实测慢 2.5 倍（见 sec0_isa.txt）
  gcc -O3 -march=x86-64-v3 -pthread "$ROOT/code/pow24.c" -o "$T24/pow24" -lm
  gcc -O3 -march=x86-64-v3 -mavx2 -pthread "$ROOT/code/hot25.c" -o "$T25/hot25" -lm
  gcc -O3 -march=native   -pthread "$ROOT/code/pow24.c" -o "$T24/pow24_native" -lm
  gcc -O3 -march=x86-64-v3 -pthread "$ROOT/tools/probe24.c" -o "$T24/probe24" -lm
  gcc -O3 -march=native   -pthread "$ROOT/tools/probe24.c" -o "$T24/probe24_native" -lm
  ls -l "$T24/pow24" "$T25/hot25" "$T24/pow24_native"
} 2>&1 | tee "$OUT/build.log"
[ -x "$T24/pow24" ] && [ -x "$T25/hot25" ] || { echo "编译失败"; exit 1; }

say "0b perf 与 PMU 可达性"
{
  echo "## 安装 perf：apt 走阿里云镜像会 403，改从清华源直接取包再解压"
  if ! command -v perf >/dev/null 2>&1; then
    mkdir -p /tmp/pf && cd /tmp/pf
    curl -sL -o t.deb "https://mirrors.tuna.tsinghua.edu.cn/ubuntu/pool/main/l/linux/linux-tools-5.15.0-164_5.15.0-164.174_amd64.deb"
    dpkg-deb -x t.deb x
    cp x/usr/lib/linux-tools-5.15.0-164/perf /usr/local/bin/perf && chmod +x /usr/local/bin/perf
  fi
  perf --version 2>&1 | head -1
  echo "## perf_event_paranoid（-1 表示宿主允许）"
  cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo "(无此文件)"
  echo "## 容器能力与 seccomp"
  grep -E 'CapEff|Seccomp' /proc/self/status
  echo "## 硬件事件"
  perf stat -e cycles,instructions -- true 2>&1 | tail -4
  echo "## 软件事件（排除 PMU 因素）"
  perf stat -e task-clock -- true 2>&1 | tail -4
} > "$OUT/sec0_perf.txt" 2>&1

say "S1 schedstat 排队时间（替代 PSI）"
{
  echo "## 1 线程（钉核 0）";            taskset -c $PIN1 "$T24/probe24" wait 1 60 32 0.5
  echo "## 4 线程（钉 0,2,4,6）";        taskset -c $PIN4 "$T24/probe24" wait 4 60 32 0.5
  echo "## 4 线程（全挤核 0）";          taskset -c $PIN1 "$T24/probe24" wait 4 60 32 0.5
  echo "## 4 线程（钉 0-3，SMT 对照）";  taskset -c 0-3 "$T24/probe24" wait 4 20 32 0.5
} > "$OUT/secS1_wait.txt" 2>&1

say "S2 TSC 周期计数（AVX2 vs AVX-512）"
{
  echo "## AVX2（x86-64-v3）";  taskset -c $PIN1 "$T24/probe24" tsc 100 32 | tail -2
  echo "## AVX-512（native）";  taskset -c $PIN1 "$T24/probe24_native" tsc 40 32 | tail -2
} > "$OUT/secS2_tsc.txt" 2>&1

say "0 ISA 对照（AVX-512 vs AVX2）"
{
  echo "### 同一份代码两种 -march，各 15 秒单线程钉核0"
  echo "## -march=native（Ice Lake 走 AVX-512）"
  taskset -c $PIN1 "$T24/pow24_native" sustain 1 15 32 5 2>&1 | tail -4
  echo "## -march=x86-64-v3（AVX2，与原文同路径）"
  taskset -c $PIN1 "$T24/pow24" sustain 1 15 32 5 2>&1 | tail -4
  echo "## cpu MHz 采样"
  grep -m4 'MHz' /proc/cpuinfo
} > "$OUT/sec0_isa.txt" 2>&1

# ---------- 2 第24篇 ----------
say "24-B 短测 5x10s（钉核0）"
{ for i in 1 2 3 4 5; do echo "## round $i"; timeout 25 taskset -c $PIN1 "$T24/pow24" sustain 1 10 32 5; done; } > "$OUT/secB_short.txt" 2>&1

say "24-C 单核 240s p10"
{ timeout 300 taskset -c $PIN1 "$T24/pow24" sustain2 1 240 32 10; } > "$OUT/secC2_single_p10.txt" 2>&1

say "24-D 四物理核 180s p10（0,2,4,6）"
{ timeout 260 taskset -c $PIN4 "$T24/pow24" sustain2 4 180 32 10; } > "$OUT/secD2_quad_p10.txt" 2>&1

say "24-E 恢复探针 120s"
{ timeout 180 taskset -c $PIN1 "$T24/pow24" probe 120 10; } > "$OUT/secE_recover.txt" 2>&1

say "24-F 间歇 40/20 x4"
{ timeout 300 taskset -c $PIN1 "$T24/pow24" burst 4 40 20 4 32; } > "$OUT/secF_burst.txt" 2>&1

say "24-G 份额对照（竞争进程模拟 100/50/25%）"
{
  echo "### 同核竞争：k 个空转进程 + 1 个被测，理论份额 1/(k+1)"
  for k in 0 1 3; do
    pids=""
    for j in $(seq 1 $k); do taskset -c $PIN1 bash -c 'while :; do :; done' & pids="$pids $!"; done
    echo "## k=$k 竞争进程，份额≈$((100/(k+1)))%"
    taskset -c $PIN1 timeout 60 "$T24/pow24" sustain 1 40 32 8 2>&1 | tail -6
    for p in $pids; do kill $p 2>/dev/null; done
    wait 2>/dev/null
  done
} > "$OUT/secG_share.txt" 2>&1

# ---------- 3 第25篇 ----------
say "25-A cache 阶梯"
{
  for kb in 4 16 32 64 128 256 512 1024 2048 4096 8192 16384 32768 65536; do
    timeout 60 taskset -c $PIN1 "$T25/hot25" chase "$kb" 30000000 2>&1 | tail -1
  done
} > "$OUT/secA_cache.txt" 2>&1

say "25-B 顺序带宽"
{
  for mb in 8 128 1024; do for m in read write copy; do for i in 1 2 3; do
    timeout 60 taskset -c $PIN1 "$T25/hot25" stream "$mb" "$m" 2>&1 | tail -1; done; done; done
} > "$OUT/secB_stream.txt" 2>&1

say "25-C GEMV 6 变体 x5"
{
  for v in scalar avx2 avx2x4 mt:1 mt:2 mt:4; do for i in 1 2 3 4 5; do
    case "$v" in
      mt:*) timeout 120 taskset -c $PIN4 "$T25/hot25" gemv 32 "$v" 2>&1 | tail -1 ;;
      *)    timeout 120 taskset -c $PIN1 "$T25/hot25" gemv 32 "$v" 2>&1 | tail -1 ;;
    esac
  done; done
} > "$OUT/secC_gemv.txt" 2>&1

say "25-D GEMM 3 尺寸 x6 变体 x3"
{
  for n in 256 384 512; do for v in naive block:32 block:64 block:128 mt:2 mt:4; do for i in 1 2 3; do
    case "$v" in
      mt:*) timeout 120 taskset -c $PIN4 "$T25/hot25" gemm "$n" "$v" 2>&1 | tail -1 ;;
      *)    timeout 120 taskset -c $PIN1 "$T25/hot25" gemm "$n" "$v" 2>&1 | tail -1 ;;
    esac
  done; done; done
} > "$OUT/secD_gemm.txt" 2>&1

say "25-E 绑核（4 线程）"
{
  echo "## 4 线程全钉 1 个物理核（0）";     for i in 1 2 3 4 5; do taskset -c 0 timeout 120 "$T25/hot25" gemv 32 mt:4 2>&1 | tail -1; done
  echo "## 4 线程钉 4 个物理核（0,2,4,6）"; for i in 1 2 3 4 5; do taskset -c 0,2,4,6 timeout 120 "$T25/hot25" gemv 32 mt:4 2>&1 | tail -1; done
  echo "## 4 线程钉 0-3（=2 物理核 + 超线程）"; for i in 1 2 3 4 5; do taskset -c 0-3 timeout 120 "$T25/hot25" gemv 32 mt:4 2>&1 | tail -1; done
  echo "## 4 线程不钉核（64 逻辑核）";      for i in 1 2 3 4 5; do timeout 120 "$T25/hot25" gemv 32 mt:4 2>&1 | tail -1; done
} > "$OUT/secE_affinity.txt" 2>&1

say "25-F 优化前后"
{
  echo "## before 标量单线程";        for i in 1 2 3 4 5; do taskset -c 0 timeout 120 "$T25/hot25" gemv 32 scalar 2>&1 | tail -1; done
  echo "## after AVX2x4 单线程";      for i in 1 2 3 4 5; do taskset -c 0 timeout 120 "$T25/hot25" gemv 32 avx2x4 2>&1 | tail -1; done
  echo "## after AVX2x4 + 4 物理核";  for i in 1 2 3 4 5; do taskset -c 0,2,4,6 timeout 120 "$T25/hot25" gemv 32 mt:4 2>&1 | tail -1; done
} > "$OUT/secF_before_after.txt" 2>&1

say "25-G 回归 + n=384 边界"
{
  for i in $(seq 1 10); do taskset -c 0 timeout 120 "$T25/hot25" gemv 32 avx2x4 2>&1 | tail -1; done
  echo "## GEMM 一致性 n=256"
  taskset -c 0 timeout 120 "$T25/hot25" gemm 256 naive 2>&1 | tail -1
  taskset -c 0 timeout 120 "$T25/hot25" gemm 256 block:64 2>&1 | tail -1
  taskset -c 0,2,4,6 timeout 120 "$T25/hot25" gemm 256 mt:4 2>&1 | tail -1
  echo "## GEMM n=384（越界 bug 的那个尺寸）"
  for v in naive block:64 mt:4; do
    case "$v" in mt:*) taskset -c 0,2,4,6 timeout 120 "$T25/hot25" gemm 384 "$v" 2>&1 | tail -1 ;;
      *) taskset -c 0 timeout 120 "$T25/hot25" gemm 384 "$v" 2>&1 | tail -1 ;; esac
  done
} > "$OUT/secG_regress.txt" 2>&1

# ---------- 4 汇总 ----------
say "汇总"
{
  echo "=== 环境 ==="; export LC_ALL=C; grep -E "Model name|nproc|PRETTY_NAME|smt" "$OUT/env.txt" 2>/dev/null; cat /sys/devices/system/cpu/cpu0/topology/thread_siblings_list
  echo; echo "=== 24-C 单核 p10 ==="; tail -8 "$OUT/secC2_single_p10.txt"
  echo; echo "=== 24-D 四核 p10 ==="; tail -8 "$OUT/secD2_quad_p10.txt"
  echo; echo "=== 24-G 份额 ==="; grep -E "k=|p10|tok|token" "$OUT/secG_share.txt" | tail -14
  echo; echo "=== 25-C GEMV ==="; cat "$OUT/secC_gemv.txt"
  echo; echo "=== 25-E 绑核 ==="; cat "$OUT/secE_affinity.txt"
  echo; echo "=== 25-F 优化前后 ==="; cat "$OUT/secF_before_after.txt"
} > "$OUT/summary.txt" 2>&1

tar czf "$ROOT/results_$STAMP.tar.gz" -C "$OUT" . 2>/dev/null
say "DONE -> $OUT"
cat "$OUT/summary.txt"
