#!/bin/sh
# RK3588 · 第25篇：一条命令跑完全部实验
# 前置：把 DDR 频率钉住，否则同一份代码会在 7 和 22 GB/s 之间跳（见正文 6.6）
cd /tmp/rk || exit 1
echo "===== 0. 环境 ====="
uname -a
for c in 0 4; do echo "cpu$c: $(cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_cur_freq) kHz"; done
echo "dmc: $(cat /sys/class/devfreq/dmc/governor) $(cat /sys/class/devfreq/dmc/cur_freq)"
echo performance > /sys/class/devfreq/dmc/governor

echo "===== A. 缓存延迟 ====="
for kb in 4 32 256 1024 8192; do ./rk25 chase $kb 20000000; done

echo "===== B. 顺序带宽 ====="
./rk25 stream 256 read
./rk25 stream 256 copy

echo "===== C. GEMV 六变体（各 5 次）====="
for v in scalar neon neonx4; do
    i=0; while [ $i -lt 5 ]; do ./rk25 gemv 64 $v; i=$((i+1)); done
done
i=0; while [ $i -lt 5 ]; do CPUS=4-7 ./rk25 gemv 64 mt:4; i=$((i+1)); done
i=0; while [ $i -lt 3 ]; do CPUS=4-7 ./rk25 gemv 64 mt:2; i=$((i+1)); done
i=0; while [ $i -lt 5 ]; do CPUS=0-3 ./rk25 gemv 64 mt:4; i=$((i+1)); done

echo "===== D. GEMM n=512 五变体 ====="
i=0; while [ $i -lt 3 ]; do ./rk25 gemm 512 naive; i=$((i+1)); done
i=0; while [ $i -lt 3 ]; do ./rk25 gemm 512 block:64; i=$((i+1)); done
i=0; while [ $i -lt 3 ]; do CPUS=4-7 ./rk25 gemm 512 mt:2; i=$((i+1)); done
i=0; while [ $i -lt 3 ]; do CPUS=4-7 ./rk25 gemm 512 mt:4; i=$((i+1)); done
i=0; while [ $i -lt 3 ]; do CPUS=0-3 ./rk25 gemm 512 mt:4; i=$((i+1)); done

echo "===== E. 越界 bug 复现（修复前才不一致）====="
for n in 256 384 512; do ./rk25 gemm $n naive; CPUS=4-7 ./rk25 gemm $n mt:4; done

echo "===== F. 连跑十次看离散度 ====="
i=0; while [ $i -lt 10 ]; do ./rk25 gemv 64 neonx4; i=$((i+1)); done

echo dmc_ondemand > /sys/class/devfreq/dmc/governor
echo "===== DONE ====="
