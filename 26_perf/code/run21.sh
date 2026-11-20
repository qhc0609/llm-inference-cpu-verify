#!/bin/bash
# run21.sh - 第21篇：大模型 CPU 性能分析的全部实测
# 用法: bash run21.sh [outdir]
set -u
cd "$(dirname "$0")"
OUT=${1:-results21}
mkdir -p "$OUT"

# 停掉宿主机上抢占 CPU 的编译容器，跑完恢复
restore() { docker unpause ohos-compile-env >/dev/null 2>&1; }
trap restore EXIT INT TERM
docker pause ohos-compile-env >/dev/null 2>&1 && sleep 1 && echo "(compile container paused)" || echo "(no compile container)"

CC=${CC:-gcc}
CFLAGS="-O3 -fno-omit-frame-pointer -march=native -std=gnu11"

echo ">>> build"
$CC $CFLAGS 21b_loads.c  -o 21b_loads  -lm || exit 1
$CC $CFLAGS -fno-if-conversion -fno-if-conversion2 21c_branch.c -o 21c_branch -lm || exit 1
$CC $CFLAGS 21d_wss.c    -o 21d_wss    -lm || exit 1
$CC $CFLAGS 21e_threads.c -o 21e_threads -lpthread -lm || exit 1
$CC $CFLAGS 21f_gemv.c   -o 21f_gemv   -lm || exit 1

echo ">>> env"
bash 21a_env.sh > "$OUT/env.txt" 2>&1

# tracefs 可能没挂载
mount -t tracefs nodev /sys/kernel/debug/tracing 2>/dev/null || true

# PMU 能力探测：perf 即使事件不支持也返回 0，必须看输出里有没有 not supported
PMU=0
perf stat -e cycles,instructions true 2>&1 | grep -q "not supported" || PMU=1
CACHE=0
perf stat -e cache-misses true 2>&1 | grep -q "not supported" || CACHE=1
BR=0
perf stat -e branch-misses true 2>&1 | grep -q "not supported" || BR=1
echo "PMU=$PMU CACHE=$CACHE BRANCH=$BR" | tee "$OUT/pmu.txt"
perf stat -e cycles,instructions,cache-misses,branch-misses,L1-dcache-load-misses,LLC-load-misses true > "$OUT/pmu_probe.txt" 2>&1
perf list hw > "$OUT/perf_list_hw.txt" 2>&1
ls /sys/bus/event_source/devices/ > "$OUT/pmu_devices.txt" 2>&1

# 反汇编数指令条数：软件事件环境下估算 IPC 的唯一依据
objdump -d --no-show-raw-insn 21b_loads > "$OUT/21b.dis" 2>&1
objdump -d --no-show-raw-insn 21c_branch > "$OUT/21c.dis" 2>&1
for f in load_dep load_par4 load_chase; do
  n=$(awk "/<$f>:/{flag=1;next}/^$/{flag=0}flag" "$OUT/21b.dis" | grep -cE "^\s+[0-9a-f]+:")
  echo "$f=$n"
done > "$OUT/instr_count.txt"
cat "$OUT/instr_count.txt"

EV_IPC="cycles,instructions"
[ $PMU -eq 0 ] && EV_IPC="task-clock"

# ---------- 1) 三种负载的 IPC ----------
echo ">>> loads"
for m in dep par4; do
  for i in 1 2 3 4 5 6 7 8 9; do
    echo "RES|load|$m|$i"
    perf stat -x, -e $EV_IPC taskset -c 2 ./21b_loads $m 60000000 2>&1
  done
done > "$OUT/loads.txt" 2>&1

for i in 1 2 3 4 5 6 7 8 9; do
  echo "RES|chase|$i"
  perf stat -x, -e $EV_IPC taskset -c 2 ./21b_loads chase 20000000 33554432 8 2>&1
done > "$OUT/chase.txt" 2>&1

# ---------- 2) 分支：排序 vs 随机 ----------
echo ">>> branch"
for o in sorted random; do
  for i in 1 2 3 4 5 6 7 8 9; do
    echo "RES|branch|$o|$i"
    if [ $BR -eq 1 ]; then
      perf stat -x, -e "$EV_IPC,branch-misses,branches" taskset -c 2 ./21c_branch $o 2>&1
    else
      perf stat -x, -e "$EV_IPC" taskset -c 2 ./21c_branch $o 2>&1
    fi
  done
done > "$OUT/branch.txt" 2>&1

# ---------- 3) Cache：工作集台阶 ----------
echo ">>> wss"
: > "$OUT/wss.txt"
for sz in 16384 65536 262144 1048576 4194304 16777216 67108864 268435456; do
  for i in 1 2 3; do
    echo "RES|wss_seq|$sz|$i" >> "$OUT/wss.txt"
    taskset -c 2 ./21d_wss seq $sz >> "$OUT/wss.txt" 2>&1
  done
  for i in 1 2 3; do
    echo "RES|wss_rnd|$sz|$i" >> "$OUT/wss.txt"
    taskset -c 2 ./21d_wss rnd $sz >> "$OUT/wss.txt" 2>&1
  done
done

# 带 PMU 的缺失率（每个尺寸一遍即可）
: > "$OUT/wss_pmu.txt"
if [ $CACHE -eq 1 ]; then
  for sz in 16384 262144 4194304 67108864; do
    echo "RES|wss_pmu|$sz" >> "$OUT/wss_pmu.txt"
    perf stat -x, -e "cycles,instructions,cache-references,cache-misses,L1-dcache-load-misses,LLC-load-misses" \
      ./21d_wss seq $sz >> "$OUT/wss_pmu.txt" 2>&1
  done
fi

# ---------- 4) 线程扩展与上下文切换 ----------
echo ">>> threads"
: > "$OUT/threads.txt"
for t in 1 2 3 4 6 8 12; do
  for i in 1 2 3 4 5; do
    echo "RES|thread|$t|$i" >> "$OUT/threads.txt"
    ./21e_threads $t 262144 1920 >> "$OUT/threads.txt" 2>&1
  done
done

# ---------- 5) ftrace: sched_switch ----------
echo ">>> ftrace"
TR=/sys/kernel/debug/tracing
if [ -d "$TR/events/sched" ]; then
  ( echo 0 > $TR/tracing_on
    echo > $TR/trace
    echo 1 > $TR/events/sched/sched_switch/enable
    echo 1 > $TR/events/sched/sched_wakeup/enable
    echo 1 > $TR/tracing_on
    ./21e_threads 8 262144 2000 > /dev/null 2>&1
    echo 0 > $TR/tracing_on
    echo 0 > $TR/events/sched/sched_switch/enable
    echo 0 > $TR/events/sched/sched_wakeup/enable ) >/dev/null 2>&1
  grep -c "sched_switch" $TR/trace > "$OUT/ftrace_count.txt" 2>/dev/null
  head -40 $TR/trace > "$OUT/ftrace_head.txt" 2>/dev/null
  # 统计：21e_threads 的迁移次数、被抢占次数
  awk '/sched_switch/ && /21e_threads/ {
      if (match($0, /prev_comm=[^ ]+/)) { }
      n++
      if ($0 ~ /prev_state=R/) preempt++
      if (match($0, /==> [^ ]+/)) { dst=substr($0, RSTART+4, RLENGTH-4); if (dst ~ /21e_threads/) toself++ }
  } END { printf "TOTAL=%d\nPREEMPT=%d\nTO_SELF=%d\n", n, preempt, toself }' $TR/trace > "$OUT/ftrace_stats.txt" 2>/dev/null
else
  echo "NO_TRACEFS" > "$OUT/ftrace_count.txt"
fi

# ---------- 6) top 采样 ----------
echo ">>> top"
( top -b -n 1 -d 1 | head -12 ) > "$OUT/top_idle.txt" 2>&1
( ./21f_gemv 40 >/dev/null 2>&1 & sleep 2; top -b -n 1 -d 1 | head -12 ) > "$OUT/top_busy.txt" 2>&1; sleep 1
sleep 3

# ---------- 7) perf record + 火焰图 ----------
echo ">>> perf record"
PEV="cycles"
perf record -e $PEV true >/dev/null 2>&1 || PEV="cpu-clock"
perf record -F 2000 -g --call-graph fp --delay 2500 -e $PEV -o "$OUT/perf.data" -- ./21f_gemv 40 > "$OUT/gemv.txt" 2>&1
perf report -i "$OUT/perf.data" --stdio --no-children --percent-limit 0.5 2>/dev/null | head -40 > "$OUT/perf_report.txt"
perf script -i "$OUT/perf.data" > "$OUT/perf.script" 2>/dev/null

# CPU 利用率：跑 gemv 时采 /proc/stat
( ./21f_gemv 40 >/dev/null 2>&1 & sleep 2
  a=$(awk '/^cpu /{print $2+$3+$4, $5}' /proc/stat); sleep 2
  b=$(awk '/^cpu /{print $2+$3+$4, $5}' /proc/stat)
  echo "$a $b" > "$OUT/cpu_util_raw.txt"; wait ) >/dev/null 2>&1

# perf sched（若可用）
perf sched record -o "$OUT/sched.data" -- ./21e_threads 8 262144 2000 >/dev/null 2>&1 \
  && perf sched latency -i "$OUT/sched.data" 2>/dev/null | head -15 > "$OUT/perf_sched.txt"

echo ">>> done"
ls -la "$OUT" | tail -5
