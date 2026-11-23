#!/bin/sh
# RK3588 第24篇实验：频率与温度全程可读
cd /tmp/rk || exit 1
echo "===== ENV ====="
uname -a
cat /etc/os-release | head -3
echo "-- cpu parts --"
grep -E 'processor|CPU part' /proc/cpuinfo | paste - - | sed 's/\t/ /g'
echo "-- freq table --"
for c in 0 4 6; do echo -n "cpu$c max="; cat /sys/devices/system/cpu/cpu$c/cpufreq/cpuinfo_max_freq; done
for c in 0 4 6; do echo -n "cpu$c gov="; cat /sys/devices/system/cpu/cpu$c/cpufreq/scaling_governor; done
echo "-- thermal zones --"
for z in /sys/class/thermal/thermal_zone*; do echo "$(cat $z/type)=$(cat $z/temp)"; done
echo "-- misc --"
echo -n "psi="; ls /proc/pressure 2>/dev/null || echo none
echo -n "perf="; command -v perf || echo none
echo -n "steal(cpu line)="; head -1 /proc/stat
echo -n "idle_big_temp="; cat /sys/class/thermal/thermal_zone1/temp
echo

echo "===== B single-thread 240s ====="
./rk24 sustain2 1 240 64 10 4

echo "===== C four-thread A76 180s ====="
./rk24 sustain2 4 180 64 10 4-7

echo "===== D four-thread A55 120s ====="
./rk24 sustain2 4 120 64 10 0-3

echo "===== E eight-thread all cores 300s ====="
./rk24 sustain2 8 300 64 10 0-7

echo "===== F burst 4x(40on/20off) A76 ====="
./rk24 burst 4 40 20 4 64 4-7

echo "===== G probe 100s ====="
./rk24 probe 100 5 4

echo "===== DONE ====="
