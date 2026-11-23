#!/bin/bash
# repro_24_25.sh —— 第24/25篇全量复现（约 40 分钟）
# 用法： bash repro_24_25.sh
# 产物： 当前目录下 rerun_YYYYmmdd_HHMM/ ，含 env.txt / 各段 raw 输出 / summary.txt / results.tar.gz
# 安全： 只在 /tmp/sched24 /tmp/sched25 与 /sys/fs/cgroup/wbexp_repro 里活动

set -u
ROOT=$(cd "$(dirname "$0")" && pwd)
STAMP=$(date +%Y%m%d_%H%M)
OUT="$ROOT/rerun_$STAMP"
T24=/tmp/sched24
T25=/tmp/sched25
mkdir -p "$OUT" "$T24" "$T25"

if [ "$(id -u)" -eq 0 ]; then SUDO=""; else SUDO="sudo -n"; fi
say(){ echo "### $*"; }

# ---------- 0 环境 ----------
say "环境"
{
  echo "\$ date"; date '+%F %T'
  echo "\$ uname -a"; uname -r
  echo "\$ os"; grep PRETTY_NAME /etc/os-release
  echo "\$ cpu"; grep -m1 'model name' /proc/cpuinfo; echo "nproc=$(nproc)"
  echo "\$ mem"; free -g | head -2
  echo "\$ gcc"; gcc --version | head -1
  echo "\$ cgroup"; cat /sys/fs/cgroup/cgroup.controllers 2>/dev/null || echo none
  echo "\$ load"; cat /proc/loadavg
} 2>&1 | tee "$OUT/env.txt"

# ---------- 1 编译 ----------
say "编译"
{
  gcc -O3 -march=native -pthread "$ROOT/29_power_thermal/code/pow24.c" -o "$T24/pow24" -lm
  gcc -O3 -march=native -mavx2 -pthread "$ROOT/30_hotspot/code/hot25.c" -o "$T25/hot25" -lm \
    || gcc -O3 -march=haswell -pthread "$ROOT/30_hotspot/code/hot25.c" -o "$T25/hot25" -lm
  ls -l "$T24/pow24" "$T25/hot25"
} 2>&1 | tee "$OUT/build.log"
[ -x "$T24/pow24" ] || { echo "pow24 编译失败"; exit 1; }
[ -x "$T25/hot25" ] || { echo "hot25 编译失败"; exit 1; }

# ---------- 2 第24篇 ----------
say "24-A 接口自证"
{
  echo "### A 这台机器能读到什么（$(date +%F)）"
  echo "\$ ls /sys/devices/system/cpu/cpu0/cpufreq"
  ls /sys/devices/system/cpu/cpu0/cpufreq 2>&1
  echo; echo "\$ ls -d /sys/class/thermal/thermal_zone*"
  ls -d /sys/class/thermal/thermal_zone* 2>&1
  echo; echo "\$ ls /sys/class/powercap"; ls /sys/class/powercap 2>&1
  echo; echo "\$ grep MHz /proc/cpuinfo 连续 5 次"
  for k in 1 2 3 4 5; do grep -m1 MHz /proc/cpuinfo; sleep 0.2; done
  echo; echo "\$ MSR 0x198"
  $SUDO python3 -c "
import os,struct
for c in (0,1,2,3):
    try:
        fd=os.open('/dev/cpu/%d/msr'%c,os.O_RDONLY)
        v=struct.unpack('<Q',os.pread(fd,8,0x198))[0]
        print('cpu%d raw=0x%x ratio=%d MHz=%d'%(c,v,(v>>8)&0xff,((v>>8)&0xff)*100))
    except Exception as e: print('cpu%d FAIL'%c,e)
" 2>&1
  echo; echo "\$ perf stat -e cycles,instructions -- true"
  $SUDO perf stat -e cycles,instructions -- true 2>&1 | tail -4
} > "$OUT/secA_iface.txt" 2>&1

say "24-B 短测 5x10s"
{ for i in 1 2 3 4 5; do echo "## round $i"; timeout 20 "$T24/pow24" sustain 1 10 32 5; done; } > "$OUT/secB_short.txt" 2>&1

say "24-C 单核 240s（约 4 分钟）"
{ timeout 300 "$T24/pow24" sustain2 1 240 32 10; } > "$OUT/secC2_single_p10.txt" 2>&1

say "24-D 四核 180s（约 3 分钟）"
{ timeout 260 "$T24/pow24" sustain2 4 180 32 10; } > "$OUT/secD2_quad_p10.txt" 2>&1

say "24-E 恢复探针 120s"
{ timeout 180 "$T24/pow24" probe 120 10; } > "$OUT/secE_recover.txt" 2>&1

say "24-F 间歇 40/20 x4（约 4 分钟）"
{ timeout 300 "$T24/pow24" burst 4 40 20 4 32; } > "$OUT/secF_burst.txt" 2>&1

say "24-G cgroup 配额 100/50/25%（约 2 分钟）"
{
  echo "### G cgroup v2 配额，各 40 秒单线程"
  if [ -f /sys/fs/cgroup/cgroup.controllers ]; then
    $SUDO sh -c 'echo +cpu > /sys/fs/cgroup/cgroup.subtree_control; mkdir -p /sys/fs/cgroup/wbexp_repro' 2>/dev/null
    for lim in 100000 50000 25000; do
      $SUDO sh -c "echo '$lim 100000' > /sys/fs/cgroup/wbexp_repro/cpu.max" 2>/dev/null
      echo "## cpu.max=$lim/100000 实际=$(cat /sys/fs/cgroup/wbexp_repro/cpu.max 2>/dev/null)"
      $SUDO bash -c "echo \$\$ > /sys/fs/cgroup/wbexp_repro/cgroup.procs; exec $T24/pow24 sustain 1 40 32 8" 2>&1 | tail -6
    done
    $SUDO rmdir /sys/fs/cgroup/wbexp_repro 2>/dev/null
  else
    echo "没有 cgroup v2，本段跳过"
  fi
} > "$OUT/secG_quota.txt" 2>&1

# ---------- 3 第25篇 ----------
say "25-A cache 阶梯（约 3 分钟）"
{
  for kb in 4 16 32 64 128 256 512 1024 2048 4096 8192 16384 32768 65536; do
    timeout 60 "$T25/hot25" chase "$kb" 30000000 2>&1 | tail -1
  done
} > "$OUT/secA_cache.txt" 2>&1

say "25-B 顺序带宽"
{
  for mb in 8 128; do for m in read write copy; do for i in 1 2 3; do
    timeout 60 "$T25/hot25" stream "$mb" "$m" 2>&1 | tail -1; done; done; done
} > "$OUT/secB_stream.txt" 2>&1

say "25-C GEMV 6 变体 x5（约 4 分钟）"
{
  for v in scalar avx2 avx2x4 mt:1 mt:2 mt:4; do for i in 1 2 3 4 5; do
    timeout 120 "$T25/hot25" gemv 32 "$v" 2>&1 | tail -1; done; done
} > "$OUT/secC_gemv.txt" 2>&1

say "25-D GEMM 3 尺寸 x6 变体 x3（约 8 分钟）"
{
  for n in 256 384 512; do for v in naive block:32 block:64 block:128 mt:2 mt:4; do for i in 1 2 3; do
    timeout 120 "$T25/hot25" gemm "$n" "$v" 2>&1 | tail -1; done; done; done
} > "$OUT/secD_gemm.txt" 2>&1

say "25-E 绑核"
{
  echo "## 钉 1 核"; for i in 1 2 3 4 5; do taskset -c 0 timeout 120 "$T25/hot25" gemv 32 mt:4 2>&1 | tail -1; done
  echo "## 钉 4 核"; for i in 1 2 3 4 5; do taskset -c 0-3 timeout 120 "$T25/hot25" gemv 32 mt:4 2>&1 | tail -1; done
  echo "## 不钉";   for i in 1 2 3 4 5; do timeout 120 "$T25/hot25" gemv 32 mt:4 2>&1 | tail -1; done
} > "$OUT/secE_affinity.txt" 2>&1

say "25-F 优化前后"
{
  echo "## before 标量单线程"; for i in 1 2 3 4 5; do timeout 120 "$T25/hot25" gemv 32 scalar 2>&1 | tail -1; done
  echo "## after AVX2x4 + 4 线程"; for i in 1 2 3 4 5; do taskset -c 0-3 timeout 120 "$T25/hot25" gemv 32 mt:4 2>&1 | tail -1; done
} > "$OUT/secF_before_after.txt" 2>&1

say "25-G 回归"
{
  for i in $(seq 1 10); do timeout 120 "$T25/hot25" gemv 32 avx2x4 2>&1 | tail -1; done
  echo "## GEMM 一致性"
  timeout 120 "$T25/hot25" gemm 256 naive 2>&1 | tail -1
  timeout 120 "$T25/hot25" gemm 256 block:64 2>&1 | tail -1
  timeout 120 "$T25/hot25" gemm 256 mt:4 2>&1 | tail -1
} > "$OUT/secG_regress.txt" 2>&1

# ---------- 4 汇总 ----------
say "汇总"
{
  echo "=== 环境 ==="; grep -E "PRETTY_NAME|model name|nproc" "$OUT/env.txt" 2>/dev/null
  echo; echo "=== 24-C 单核 p10 ==="; tail -6 "$OUT/secC2_single_p10.txt"
  echo; echo "=== 24-D 四核 p10 ==="; tail -6 "$OUT/secD2_quad_p10.txt"
  echo; echo "=== 24-G 配额 ==="; grep -E "cpu.max|cpu_us_per_token|tok" "$OUT/secG_quota.txt" | tail -12
  echo; echo "=== 25-C GEMV ==="; cat "$OUT/secC_gemv.txt"
  echo; echo "=== 25-E 绑核 ==="; cat "$OUT/secE_affinity.txt"
} > "$OUT/summary.txt" 2>&1

tar czf "$ROOT/results_$STAMP.tar.gz" -C "$OUT" . 2>/dev/null
say "DONE -> $OUT"
say "打包 -> $ROOT/results_$STAMP.tar.gz"
cat "$OUT/summary.txt"
