#!/bin/bash
# run23.sh —— 第23篇《大模型CPU调度优化》实验总跑
# 用法: sudo bash run23.sh A|B|C|D|E|F|G|H|K|ALL
#
# 安全约束（必须遵守）：
#   1. 只在 /tmp/sched23 与 /sys/fs/cgroup/wbexp23 里活动，绝不碰用户工程目录
#   2. 不修改任何全局 sysctl，不改内核参数，不重启
#   3. 退出时无条件清理 cgroup 并恢复 subtree_control
#   4. 所有测量都带 timeout，最长 60 秒

set -u
BIN=/tmp/sched23/sched23
OUT=/tmp/sched23/results
CG=/sys/fs/cgroup/wbexp23
mkdir -p "$OUT"

ORIG_SUBTREE=$(cat /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null)
CPU_MAX=$(cat /sys/fs/cgroup/wbexp23/cpu.max 2>/dev/null)

cleanup() {
  if [ -d "$CG" ]; then
    for p in $(cat "$CG/cgroup.procs" 2>/dev/null); do
      echo "$p" > /sys/fs/cgroup/cgroup.procs 2>/dev/null
    done
    for d in "$CG"/*/; do [ -d "$d" ] && rmdir "$d" 2>/dev/null; done
    rmdir "$CG"/* 2>/dev/null
    rmdir "$CG" 2>/dev/null
  fi
  # 只关掉我们打开的那一个控制器，避免影响 systemd 原有布局
  case " $ORIG_SUBTREE " in
    *" cpuset "*) : ;;
    *) echo "-cpuset" > /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null ;;
  esac
}
trap cleanup EXIT

say() { echo "### $*"; }

# ---------- A 延迟分解：一个 decode 步骤里，执行 vs 等 CPU ----------
sec_A() {
  say "A 延迟分解（32MB 权重，单线程，无竞争）"
  for mb in 8 32 96; do
    timeout 60 "$BIN" decode 150 "$mb"
  done
  say "A 同上，但每档重复 3 次看波动"
  for i in 1 2 3; do timeout 60 "$BIN" decode 150 32; done
}

# ---------- B 唤醒延迟：策略 × 竞争 ----------
sec_B() {
  say "B1 空闲基线（两线程同核 ping-pong）"
  timeout 60 "$BIN" pp 8000 0 0
  say "B2 其它核加 2/4 个自旋"
  timeout 60 "$BIN" pp 6000 2 0
  timeout 60 "$BIN" pp 6000 4 0
  say "B3 同核抢（自旋也挤在 cpu0）"
  timeout 60 "$BIN" pp 6000 2 0 1
  timeout 60 "$BIN" pp 6000 3 0 1
  say "B4 策略对比：空闲"
  for p in other batch idle fifo rr; do
    timeout 60 env SCHED23_POLICY=$p "$BIN" pp 6000 0 0
  done
  say "B5 策略对比：同核 3 个自旋"
  for p in other batch idle fifo rr; do
    timeout 60 env SCHED23_POLICY=$p "$BIN" pp 6000 3 0 1
  done
}

# ---------- C 绑核与吞吐 ----------
# 标定：1 线程 20000 次迭代 = 87ms，故 100000 次 ≈ 0.45s；每次配置跑 3 遍取最小值
CIT=100000
sec_C() {
  say "C1 固定每线程工作量，1/2/4 线程，不绑核（调度器自由迁移）"
  for r in 1 2 3; do for t in 1 2 4; do timeout 120 "$BIN" work $t $CIT; done; done
  say "C2 同上，每线程钉到不同核"
  for r in 1 2 3; do for t in 1 2 4; do timeout 120 "$BIN" work $t $CIT 0; done; done
  say "C3 4 线程全部挤在 cpu0（反面案例，4 线程分 1 核）"
  for r in 1 2; do timeout 200 taskset -c 0 "$BIN" work 4 $CIT; done
}

# ---------- D cpuset 隔离 ----------
# 注意：cpuset 必须在 root 层先启用，否则子层的 cpuset.cpus 写入会 EACCES
enable_cg() {
  mkdir -p "$CG"
  echo "+cpu +cpuset" > /sys/fs/cgroup/cgroup.subtree_control 2>/dev/null
  echo "+cpu +cpuset" > "$CG/cgroup.subtree_control" 2>/dev/null
}

sec_D() {
  say "D cpuset 隔离"
  enable_cg
  echo "root subtree: $(cat /sys/fs/cgroup/cgroup.subtree_control)"
  echo "wbexp subtree: $(cat $CG/cgroup.subtree_control)"
  mkdir -p "$CG"/shared "$CG"/iso
  echo "0-3" > "$CG/shared/cpuset.cpus"; echo 0 > "$CG/shared/cpuset.mems"
  echo "2-3" > "$CG/iso/cpuset.cpus";    echo 0 > "$CG/iso/cpuset.mems"
  echo "shared.effective=$(cat $CG/shared/cpuset.cpus.effective 2>/dev/null) iso.effective=$(cat $CG/iso/cpuset.cpus.effective 2>/dev/null)"

  say "D1 共享 cpuset 0-3，无背景负载"
  timeout 60 sh -c "echo \$\$ > $CG/shared/cgroup.procs; exec $BIN decode 120 32"

  say "D2 共享 cpuset 0-3 + cpu0-1 上 2 个背景自旋"
  timeout 25 "$BIN" spin 20 & SP=$!
  timeout 25 taskset -c 1 "$BIN" spin 20 & SP2=$!
  sleep 2
  timeout 60 sh -c "echo \$\$ > $CG/shared/cgroup.procs; exec $BIN decode 120 32"
  kill -9 $SP $SP2 2>/dev/null; wait $SP $SP2 2>/dev/null

  say "D3 隔离到 cpu2-3 + cpu0-1 上同样的 2 个背景自旋"
  timeout 25 "$BIN" spin 20 & SP=$!
  timeout 25 taskset -c 1 "$BIN" spin 20 & SP2=$!
  sleep 2
  timeout 60 sh -c "echo \$\$ > $CG/iso/cgroup.procs; exec $BIN decode 120 32"
  kill -9 $SP $SP2 2>/dev/null; wait $SP $SP2 2>/dev/null

  say "D4 隔离到 cpu2-3，但在同一个 cpuset 里再加一个竞争者"
  timeout 25 sh -c "echo \$\$ > $CG/iso/cgroup.procs; exec $BIN spin 20" & SP=$!
  sleep 2
  timeout 60 sh -c "echo \$\$ > $CG/iso/cgroup.procs; exec $BIN decode 120 32"
  kill -9 $SP 2>/dev/null; wait $SP 2>/dev/null

  say "D5 重复 D3 一次，看稳定性"
  timeout 25 "$BIN" spin 20 & SP=$!
  timeout 25 taskset -c 1 "$BIN" spin 20 & SP2=$!
  sleep 2
  timeout 60 sh -c "echo \$\$ > $CG/iso/cgroup.procs; exec $BIN decode 120 32"
  kill -9 $SP $SP2 2>/dev/null; wait $SP $SP2 2>/dev/null
}

# ---------- E cgroup 配额 ----------
sec_E() {
  say "E cgroup cpu.max 配额（单线程 decode）"
  mkdir -p "$CG"/q
  echo "+cpu" > "$CG/cgroup.subtree_control" 2>/dev/null
  for q in "max 100000" "100000 100000" "50000 100000" "25000 100000" "10000 100000"; do
    echo "$q" > "$CG/q/cpu.max"
    cur=$(cat "$CG/q/cpu.max")
    echo "--- cpu.max = $cur ---"
    timeout 60 sh -c "echo \$\$ > $CG/q/cgroup.procs; exec $BIN decode 120 32"
  done
  say "E 配额下的吞吐（2 线程固定工作量）"
  for q in "max 100000" "200000 100000" "100000 100000" "50000 100000"; do
    echo "$q" > "$CG/q/cpu.max"
    cur=$(cat "$CG/q/cpu.max")
    echo "--- cpu.max = $cur ---"
    timeout 120 sh -c "echo \$\$ > $CG/q/cgroup.procs; exec $BIN work 2 60000"
  done
}

# ---------- F cpu.weight 权重 ----------
# 关键：两个任务必须钉在同一颗核上才谈得上竞争，否则各跑各的，权重毫无作用
ucpu() { awk '/^usage_usec/{print $2}' "$1/cpu.stat" 2>/dev/null; }

sec_F() {
  say "F cpu.weight 权重份额（两个 cgroup 同时压同一颗核，读 cpu.stat）"
  enable_cg
  mkdir -p "$CG"/wa "$CG"/wb
  for d in wa wb; do
    echo "3" > "$CG/$d/cpuset.cpus"; echo 0 > "$CG/$d/cpuset.mems"
  done
  echo "wa.cpus=$(cat $CG/wa/cpuset.cpus.effective) wb.cpus=$(cat $CG/wb/cpuset.cpus.effective)"

  run_pair() {
    echo 100 > "$CG/wa/cpu.weight"
    echo "$1"  > "$CG/wb/cpu.weight"
    u0a=$(ucpu "$CG/wa"); u0b=$(ucpu "$CG/wb")
    timeout 40 sh -c "echo \$\$ > $CG/wa/cgroup.procs; exec $BIN spin 6" & A=$!
    timeout 40 sh -c "echo \$\$ > $CG/wb/cgroup.procs; exec $BIN spin 6" & B=$!
    wait $A $B
    u1a=$(ucpu "$CG/wa"); u1b=$(ucpu "$CG/wb")
    da=$((u1a - u0a)); db=$((u1b - u0b))
    echo "WEIGHT wa=100 wb=$1 wa_usec=$da wb_usec=$db ratio_wb_over_wa=$(awk -v a=$da -v b=$db 'BEGIN{printf "%.3f", b/a}') unit=ratio"
  }

  say "F1 权重 100 : 300"
  run_pair 300
  say "F2 权重 100 : 100（对照）"
  run_pair 100
  say "F3 权重 100 : 10（悬殊）"
  run_pair 10
  say "F4 重复 100 : 300"
  run_pair 300
}

# ---------- G 前台交互 vs 后台 AI ----------
sec_G() {
  say "G 前台交互延迟，后台压着 AI 任务（不同 nice）"
  for n in 0 10 19; do
    echo "--- 后台 decode nice=$n ---"
    timeout 60 nice -n $n taskset -c 0 "$BIN" decode 400 32 >/dev/null 2>&1 &
    BG=$!
    sleep 1
    timeout 60 taskset -c 0 "$BIN" pp 4000 0 0
    kill -9 $BG 2>/dev/null
    wait $BG 2>/dev/null
  done
  say "G SCHED_IDLE 做后台（对前台几乎无影响）"
  timeout 60 env SCHED23_POLICY=idle taskset -c 0 "$BIN" decode 400 32 >/dev/null 2>&1 &
  BG=$!
  sleep 1
  timeout 60 taskset -c 0 "$BIN" pp 4000 0 0
  kill -9 $BG 2>/dev/null
  wait $BG 2>/dev/null
}

# ---------- H 饥饿 ----------
# 用两个独立进程各自 getrusage 报账，避免线程组统计口径混乱
sec_H() {
  say "H 饥饿：SCHED_FIFO 纯自旋压住 cpu3，普通进程还能拿到多少 CPU"
  echo "内核 RT 预算 sched_rt_runtime_us=$(cat /proc/sys/kernel/sched_rt_runtime_us) period=$(cat /proc/sys/kernel/sched_rt_period_us)"
  echo "CONFIG_RT_GROUP_SCHED=$(grep -c 'CONFIG_RT_GROUP_SCHED=y' /boot/config-$(uname -r))"

  say "H1 普通进程独占 cpu3（基线）"
  timeout 30 taskset -c 3 "$BIN" cpuprobe 6

  say "H2 同一颗核上压一个 SCHED_FIFO 纯自旋"
  timeout 30 taskset -c 3 env SCHED23_POLICY=fifo "$BIN" spin 8 & RT=$!
  sleep 1
  timeout 30 taskset -c 3 "$BIN" cpuprobe 6
  wait $RT 2>/dev/null

  say "H3 重复 H2"
  timeout 30 taskset -c 3 env SCHED23_POLICY=fifo "$BIN" spin 8 & RT=$!
  sleep 1
  timeout 30 taskset -c 3 "$BIN" cpuprobe 6
  wait $RT 2>/dev/null

  say "H4 反过来：把普通进程提到 FIFO 高优先级，两个 RT 抢"
  timeout 30 taskset -c 3 env SCHED23_POLICY=fifo SCHED23_PRIO=10 "$BIN" spin 8 & RT=$!
  sleep 1
  timeout 30 taskset -c 3 env SCHED23_POLICY=fifo SCHED23_PRIO=60 "$BIN" cpuprobe 6
  wait $RT 2>/dev/null
}

# ---------- K 综合对比：四种工程手段的吞吐 + P99 ----------
sec_K() {
  say "K 综合：同一套 AI 负载，四种配置"
  mkdir -p "$CG"/kx
  echo "+cpu +cpuset" > "$CG/cgroup.subtree_control" 2>/dev/null
  echo "0-3" > "$CG/kx/cpuset.cpus" 2>/dev/null
  echo "0"   > "$CG/kx/cpuset.mems" 2>/dev/null
  echo "max 100000" > "$CG/kx/cpu.max"

  # 背景干扰：两个自旋压在 cpu0-1
  timeout 90 "$BIN" spin 25 &
  S1=$!
  timeout 90 taskset -c 1 "$BIN" spin 25 &
  S2=$!
  sleep 1

  say "K1 什么都不做（默认策略、不绑核、不隔离）"
  timeout 60 "$BIN" decode 120 32

  say "K2 只绑核（AI 任务钉到 cpu3）"
  timeout 60 taskset -c 3 "$BIN" decode 120 32

  say "K3 只隔离（cpuset 独占 cpu2-3）"
  timeout 60 sh -c "echo \$\$ > $CG/kx/cgroup.procs; exec taskset -c 2-3 $BIN decode 120 32"

  say "K4 隔离 + 实时优先级"
  timeout 60 sh -c "echo \$\$ > $CG/kx/cgroup.procs; exec env SCHED23_POLICY=fifo taskset -c 3 $BIN decode 120 32"

  kill -9 $S1 $S2 2>/dev/null
  wait $S1 2>/dev/null
  wait $S2 2>/dev/null
}

case "${1:-ALL}" in
  A) sec_A ;;
  B) sec_B ;;
  C) sec_C ;;
  D) sec_D ;;
  E) sec_E ;;
  F) sec_F ;;
  G) sec_G ;;
  H) sec_H ;;
  K) sec_K ;;
  ALL) sec_A; sec_B; sec_C; sec_D; sec_E; sec_F; sec_G; sec_H; sec_K ;;
  *) echo "unknown section: $1"; exit 2 ;;
esac
