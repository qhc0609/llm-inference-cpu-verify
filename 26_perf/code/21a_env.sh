#!/bin/bash
# 21a_env.sh - 采集 openEuler 实验机的性能分析环境能力
set -u
echo "=== OS ==="
cat /etc/openEuler-release 2>/dev/null || cat /etc/os-release | head -3
uname -r
echo "=== CPU ==="
lscpu | egrep "Model name|^CPU\(s\)|Thread|Core|Socket|NUMA node\(s\)|MHz|cache"
echo "=== MEM ==="
free -m | head -2
echo "=== PERF ==="
which perf || echo "NO_PERF"
perf --version 2>/dev/null || echo "NO_PERF_VERSION"
echo "--- paranoid ---"
cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo "NO_PARANOID"
echo "--- kptr_restrict / nmi_watchdog ---"
cat /proc/sys/kernel/kptr_restrict 2>/dev/null
cat /proc/sys/kernel/nmi_watchdog 2>/dev/null
echo "--- cpuinfo flags (pmu) ---"
grep -o -m1 -E "arch_perfmon|pdcm|rdtscp|constant_tsc|nonstop_tsc" /proc/cpuinfo | sort -u
echo "--- pmu devices ---"
ls /sys/bus/event_source/devices/ 2>/dev/null | tr '\n' ' '
echo
echo "--- perf stat smoke test ---"
perf stat -e cycles,instructions true 2>&1 | tail -12
echo "--- perf stat cache/branch test ---"
perf stat -e cache-misses,branch-misses,L1-dcache-load-misses true 2>&1 | tail -12
echo "=== TRACING ==="
ls /sys/kernel/debug/tracing/ 2>/dev/null | head -5 || echo "NO_TRACEFS"
which trace-cmd || echo "NO_TRACECMD"
mount | grep -c debugfs
echo "--- available filters sched ---"
ls /sys/kernel/debug/tracing/events/sched/ 2>/dev/null | head -5
echo "=== TOOLS ==="
for t in gcc make htop top python3 git perf bpftrace funcgraph stackcollapse-perf.pl flamegraph.pl; do
  printf "%-22s %s\n" "$t" "$(command -v $t || echo -)"
done
gcc --version | head -1
echo "=== LLAMA ==="
ls -d /root/llama.cpp /opt/llama.cpp 2>/dev/null || echo "NO_LLAMA_DIR"
find / -maxdepth 3 -name "llama-cli" -o -maxdepth 3 -name "main" -type f 2>/dev/null | head -3
echo "=== CONTAINERS ==="
docker ps --format '{{.Names}}' 2>/dev/null | head -5 || echo "NO_DOCKER"
echo "=== DISK/THERMAL ==="
df -h / | tail -1
