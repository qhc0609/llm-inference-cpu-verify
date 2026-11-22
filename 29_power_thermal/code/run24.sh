#!/bin/bash
# run24.sh —— 第24篇《CPU功耗与温控优化》跑批
# 安全约束：只在 /tmp/sched24 与 /sys/fs/cgroup/wbexp24 里活动；不改全局 sysctl；退出即清理 cgroup
OUT=/tmp/sched24/results
BIN=/tmp/sched24/pow24
mkdir -p "$OUT"
say() { echo "### $*"; }
SUDO="echo 1 | sudo -S -p ''"

# ---------------- A 接口自证 ----------------
say "A 接口自证"
{
  echo "### A 这台机器到底能读到什么（2026-11-22）"
  echo "\$ uname -r"; uname -r
  echo; echo "\$ ls /sys/devices/system/cpu/cpu0/cpufreq"
  ls /sys/devices/system/cpu/cpu0/cpufreq 2>&1
  echo; echo "\$ ls -d /sys/class/thermal/thermal_zone*"
  ls -d /sys/class/thermal/thermal_zone* 2>&1
  echo; echo "\$ cat /sys/class/thermal/cooling_device*/type"
  for i in 0 1 2 3; do cat /sys/class/thermal/cooling_device$i/type 2>/dev/null; done
  echo; echo "\$ ls /sys/class/powercap"; ls /sys/class/powercap 2>&1
  echo; echo "\$ grep MHz /proc/cpuinfo  连续 5 次"
  for k in 1 2 3 4 5; do grep -m1 MHz /proc/cpuinfo; sleep 0.2; done
  echo; echo "\$ MSR 0x198 (IA32_PERF_STATUS)"
  echo 1 | sudo -S -p '' python3 -c "
import os,struct
for c in (0,1,2,3):
    try:
        fd=os.open('/dev/cpu/%d/msr'%c,os.O_RDONLY)
        v=struct.unpack('<Q',os.pread(fd,8,0x198))[0]
        print('cpu%d raw=0x%x ratio=%d MHz=%d'%(c,v,(v>>8)&0xff,((v>>8)&0xff)*100))
    except Exception as e: print('cpu%d FAIL'%c,e)
" 2>&1
  echo; echo "\$ perf stat -e cycles,instructions -- true"
  echo 1 | sudo -S -p '' perf stat -e cycles,instructions -- true 2>&1 | tail -6
  echo; echo "\$ dmidecode -t processor"
  echo 1 | sudo -S -p '' dmidecode -t processor 2>/dev/null | egrep "Version:|Current Speed|Max Speed|Core Count" | head -8
} > "$OUT/secA_iface.txt" 2>&1

# ---------------- B 短测基线（5 轮 × 10 秒，单线程） ----------------
say "B 短测基线"
{
  echo "### B 五次 10 秒短测，单线程，32 MB 权重（看噪声有多大）"
  for i in 1 2 3 4 5; do
    echo "## round $i"
    timeout 20 "$BIN" sustain 1 10 32 5
  done
} > "$OUT/secB_short.txt" 2>&1

# ---------------- C 单核长跑 180 秒 ----------------
say "C 单核长跑"
{ echo "### C 单线程持续 180 秒，32 MB，窗口 10 秒"; timeout 240 "$BIN" sustain 1 180 32 10; } > "$OUT/secC_single.txt" 2>&1

# ---------------- D 四核长跑 180 秒 ----------------
say "D 四核长跑"
{ echo "### D 四线程持续 180 秒，每线程 32 MB，窗口 10 秒"; timeout 260 "$BIN" sustain 4 180 32 10; } > "$OUT/secD_quad.txt" 2>&1

# ---------------- E 卸载后恢复 120 秒 ----------------
say "E 恢复曲线"
{ echo "### E 四核跑完后立刻测 120 秒，看算力是否回升"; timeout 180 "$BIN" probe 120 10; } > "$OUT/secE_recover.txt" 2>&1

# ---------------- F 间歇负载：40 秒开 / 20 秒停 × 4 ----------------
say "F 间歇负载"
{ echo "### F 四线程 40 秒开 20 秒停，共 4 轮"; timeout 300 "$BIN" burst 4 40 20 4 32; } > "$OUT/secF_burst.txt" 2>&1

# ---------------- G 配额：模拟"被别人限速"，与真降频对照 ----------------
say "G 配额对照"
{
  echo "### G cgroup 配额 100% / 50% / 25%，各 45 秒单线程"
  echo "## 预期：配额只减少「分到的时间」，cpu_us_per_token 不变；真降频会让它变大"
  if [ -f /sys/fs/cgroup/cgroup.controllers ]; then
    echo "+cpu" > /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null
    mkdir -p /sys/fs/cgroup/wbexp24 2>/dev/null
    if [ -d /sys/fs/cgroup/wbexp24 ]; then
      for lim in 100000 50000 25000; do
        echo "$lim 100000" > /sys/fs/cgroup/wbexp24/cpu.max 2>/dev/null
        echo "## cpu.max=$lim/100000"
        timeout 90 bash -c "echo \$\$ > /sys/fs/cgroup/wbexp24/cgroup.procs && exec $BIN sustain 1 45 32 9" 2>&1 | tail -7
      done
      echo "100000 100000" > /sys/fs/cgroup/wbexp24/cpu.max 2>/dev/null
      rmdir /sys/fs/cgroup/wbexp24 2>/dev/null
    else
      echo "cgroup 创建失败（权限不足），本段跳过"
    fi
  else
    echo "没有 cgroup v2，本段跳过"
  fi
} > "$OUT/secG_quota.txt" 2>&1

say "DONE"
