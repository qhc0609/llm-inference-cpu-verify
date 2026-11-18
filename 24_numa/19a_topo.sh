#!/bin/bash
# 19a_topo.sh - read the NUMA topology straight out of the kernel.
# Everything here is a real read of /sys or numactl output; nothing is assumed.
set -u
echo "== uname =="
uname -a

echo "== numactl --hardware =="
numactl --hardware 2>&1

echo "== per-node sysfs =="
for d in /sys/devices/system/node/node[0-9]*; do
  [ -d "$d" ] || continue
  n=$(basename "$d" | tr -dc 0-9)
  echo "RES|node|$n|cpulist|$(cat $d/cpulist 2>/dev/null)"
  echo "RES|node|$n|distance|$(cat $d/distance 2>/dev/null)"
  mt=$(awk '/MemTotal/{print $4}' "$d/meminfo" 2>/dev/null)
  mf=$(awk '/MemFree/{print $4}' "$d/meminfo" 2>/dev/null)
  echo "RES|node|$n|memtotal_kb|${mt:-0}"
  echo "RES|node|$n|memfree_kb|${mf:-0}"
done

echo "== node count =="
NC=$(ls -d /sys/devices/system/node/node[0-9]* 2>/dev/null | wc -l)
echo "RES|nodes|$NC"

echo "== pages =="
echo "RES|page_kb|$(( $(getconf PAGE_SIZE) / 1024 ))"
echo "RES|thp|$(cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null)"

echo "== cpu -> node =="
for c in /sys/devices/system/cpu/cpu[0-9]*; do
  [ -d "$c" ] || continue
  id=$(basename "$c" | tr -dc 0-9)
  ln=$(readlink -f "$c/node"* 2>/dev/null | head -1)
  [ -n "$ln" ] && echo "RES|cpu|$id|node|$(basename "$ln" | tr -dc 0-9)"
done

echo "== numastat =="
numastat 2>&1 | head -20
