#!/bin/sh
cd /tmp/rk || exit 1
echo "===== F2 burst clean 4x(40on/20off) A76 ====="
./rk24b burst 4 40 20 4 64 4-7

echo "===== H single-thread A55 60s ====="
./rk24b sustain2 1 60 64 10 0

echo "===== I freq sweep: 4 threads on A76, 60s each ====="
for f in 2352000 1800000 1200000 816000; do
    for c in 4 5 6 7; do echo $f > /sys/devices/system/cpu/cpu$c/cpufreq/scaling_max_freq; done
    sleep 2
    echo "-- set max=$f cur=$(cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_cur_freq)"
    ./rk24b sustain2 4 60 64 10 4-7
done
for c in 4 5 6 7; do echo 2352000 > /sys/devices/system/cpu/cpu$c/cpufreq/scaling_max_freq; done
echo "restored cur=$(cat /sys/devices/system/cpu/cpu4/cpufreq/scaling_cur_freq)"
echo "===== EXTRA DONE ====="
