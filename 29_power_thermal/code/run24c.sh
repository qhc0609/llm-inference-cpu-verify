#!/bin/bash
# run24c.sh —— 只补跑配额对照（修正 cpu.max 的写法）
OUT=/tmp/sched24/results
BIN=/tmp/sched24/pow24
S() { echo 1 | sudo -S -p '' "$@"; }

{
  echo "### G cgroup 配额 100% / 50% / 25%，各 40 秒单线程"
  echo "## 看点：配额只减少「分到的时间」，每 token 的 CPU 时间应当基本不变"
  S sh -c 'echo +cpu > /sys/fs/cgroup/cgroup.subtree_control; mkdir -p /sys/fs/cgroup/wbexp24'
  for lim in 100000 50000 25000; do
    S sh -c "echo '$lim 100000' > /sys/fs/cgroup/wbexp24/cpu.max"
    echo "## 实际写入: $(S cat /sys/fs/cgroup/wbexp24/cpu.max)"
    S bash -c "echo \$\$ > /sys/fs/cgroup/wbexp24/cgroup.procs; exec $BIN sustain 1 40 32 8" 2>&1 | tail -6
    echo "## 该 cgroup 的累计用量: $(S cat /sys/fs/cgroup/wbexp24/cpu.stat | head -2 | tr '\n' ' ')"
  done
  S rmdir /sys/fs/cgroup/wbexp24 2>/dev/null
} > "$OUT/secG_quota.txt" 2>&1
echo DONE3
