#!/bin/bash
# run24b.sh —— 补跑：配额对照（需要 sudo）+ 逐 token 百分位长跑
OUT=/tmp/sched24/results
BIN=/tmp/sched24/pow24
say() { echo "### $*"; }

# ---------------- G 配额对照（cgroup 需要 root） ----------------
say "G 配额对照（重跑）"
{
  echo "### G cgroup 配额 100% / 50% / 25%，各 40 秒单线程"
  echo "## 看点：配额只减少「分到的时间」，cpu_us_per_token 应当基本不变"
  echo 1 | sudo -S -p '' bash -c 'echo +cpu > /sys/fs/cgroup/cgroup.subtree_control; mkdir -p /sys/fs/cgroup/wbexp24'
  for lim in 100000 50000 25000; do
    echo "$lim 100000" | { echo 1 | sudo -S -p '' tee /sys/fs/cgroup/wbexp24/cpu.max > /dev/null; }
    echo "## cpu.max=${lim}/100000"
    echo 1 | sudo -S -p '' bash -c 'echo $$ > /sys/fs/cgroup/wbexp24/cgroup.procs; exec /tmp/sched24/pow24 sustain 1 40 32 8' 2>&1 | tail -6
  done
  echo 1 | sudo -S -p '' rmdir /sys/fs/cgroup/wbexp24 2>/dev/null
} > "$OUT/secG_quota.txt" 2>&1

# ---------------- C2 单核，逐 token 百分位 ----------------
say "C2 单核 p10"
{ echo "### C2 单线程 240 秒，按窗口给每 token CPU 时间的 p10 / p50"; \
  timeout 300 "$BIN" sustain2 1 240 32 10; } > "$OUT/secC2_p10.txt" 2>&1

# ---------------- D2 四核，逐 token 百分位 ----------------
say "D2 四核 p10"
{ echo "### D2 四线程 180 秒，按窗口给每 token CPU 时间的 p10 / p50"; \
  timeout 260 "$BIN" sustain2 4 180 32 10; } > "$OUT/secD2_p10.txt" 2>&1

say "DONE2"
