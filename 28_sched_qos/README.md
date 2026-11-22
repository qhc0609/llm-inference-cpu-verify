# 大模型推理的 CPU 调度：把「等 CPU」从毫秒压到微秒

第 23 篇配套实验。一台 4 vCPU 虚拟机上跑一个最小的 decode 循环（32 MB 权重，每 token 过一遍），
用 futex ping-pong 量唤醒延迟，用 `/proc/self/schedstat` 把「在执行」和「在排队」拆开，
然后把五类旋钮——绑核、cpuset、`cpu.max` 配额、`cpu.weight` 权重、调度策略——逐个拧一遍。

平台：Ubuntu 24.04.5 / 4 vCPU（i5-7300HQ）/ 内核 6.8.0-149（EEVDF）/ gcc 13.3.0。

## 结论速览

| 测什么 | 关键数字 |
|---|---|
| 无竞争的 decode（32 MB 权重） | 每 token p50 **15.74 ms**，排队等待只占 **0.02%**，等效带宽 1.95 GB/s |
| 唤醒延迟（8000 次 ping-pong） | p50 **12.13 µs** / p99 13.23 µs / p99.9 14.45 µs |
| 同一颗核上 3 个自旋压着 | `SCHED_OTHER` p99.9 **14187 µs**；`SCHED_FIFO` **11.12 µs**，差 **1276 倍** |
| 4 线程固定工作量 | 不绑核 880.7 ms → 绑核 **756.2 ms**（快 14%）；4 线程全挤 1 核 **1545.5 ms** |
| cpuset 隔离 | 「等 CPU」占比 3.96% → **0.09%**；同一 cpuset 里再加一个竞争者回到 5.45% |
| `cpu.max` 配额 | 50% → 每 token 34.29 ms（等待占 50.81%）；10% → **242.45 ms**（90.67%） |
| `cpu.weight` 权重 | 100:300 实测 **1:2.68**；100:10 实测 **1:0.10**；100:100 实测 1:1.00 |
| 饥饿 | FIFO 纯自旋压住一颗核，普通进程 6 秒只拿到 **5.55% / 5.09%**（正好是 950/1000 的余量） |
| 综合（有干扰） | 什么都不做 27.81 ms = 36 token/s → 只做 cpuset 隔离 **20.01 ms = 50 token/s**（+39%）；隔离 + 实时优先级反而 31.49 ms（−12%） |

最后一行是全篇最反直觉的一条：把 AI 任务提到 `SCHED_FIFO`，在「长任务 + 有竞争者」的场合**更慢**，
因为撞上了内核的 RT 节流（每 1 秒只给实时任务 950 毫秒）。

## 目录

```
code/sched23.c        实验主程序：pp/decode/work/spin/cpuprobe 五种模式
code/run23.sh         A–K 分段跑批（需要 root，只在 /tmp 与 cgroup/wbexp23 里活动）
code/build_med23.py   把原始行解析成 RES| 汇总，并算等效带宽
results/sec*.txt      正文数字的来源，原始输出未经修改
results/summary.txt   build_med23.py 的产物，同一批数字的另一次表述
results/env.txt       环境自证（uname/lscpu/cgroup/RT 预算）
results/rerun_busy/   同一天在宿主较忙时的重跑，用来证明「单次对比不可信」
figs/                 四张图（配额堆叠、竞争长尾、策略对比、综合）
REPORT.md             完整实验报告：方法、全部表格、踩坑记录、可信度边界
```

## 复现

```bash
gcc -O2 -std=gnu11 -pthread -o sched23 sched23.c
sudo bash run23.sh A          # 单段：A/B/C/D/E/F/G/H/K
sudo bash run23.sh ALL        # 全跑，约 5 分钟
python3 build_med23.py ../results > ../results/summary.txt
```

单条也可以直接手跑，例如「同核抢 + 实时策略」：

```bash
SCHED23_POLICY=fifo sched23 pp 6000 3 0 1
```

两条必须知道的运行前提：

1. `run23.sh` 必须是 **LF** 换行。CRLF 会让 `mkdir -p "$OUT"` 建出一个名字叫 `results\r` 的目录，
   后面所有结果都写进那个目录，看起来像「什么都没跑」。
2. cpuset 子组要先在根层打开控制器，否则写 `cpuset.cpus` 报 `EACCES`：
   `echo "+cpu +cpuset" > /sys/fs/cgroup/cgroup.subtree_control`。脚本里已经做了，但手工试的时候容易忘。

## 四条口径说明

**1. 这是虚拟机，微秒级结论只看分布。** 宿主是共享的 4 核，vCPU 靠时间片切。
同一个配置连跑三次，唤醒延迟的 p99 能差 3 倍。所以本目录只报 p50 / p99 / p99.9，
并且只对「多次重复方向一致」的结论下判断。`results/rerun_busy/` 是同一份代码在宿主较忙时的重跑，
其中「什么都不做 / 只绑核 / 只隔离」的排序**发生了反转**——这不是代码坏了，这就是边界本身。

**2. 内核 6.8 的公平调度器是 EEVDF，不是 CFS。** 6.6 之后换的。`cpu.weight`、`cpu.max`
是 cgroup 层接口，跨调度器版本语义不变；但「唤醒后能不能立刻抢占」这件事，两代的行为不同，
正文凡引用 CFS 时代经验的地方都标注了口径。

**3. 没开 `CONFIG_RT_GROUP_SCHED`。** 所以 RT 预算只能全局调（`sched_rt_runtime_us=950000`），
不能给某个 cgroup 单独限。H 组量到的「普通进程只剩 5.5%」正是这个全局余量的直接结果。

**4. `wait_pct` 有两个来源。** 单线程的 decode 用 `/proc/self/schedstat` 的 runq wait；
cgroup 配额实验里等待发生在调度器之外（throttle），schedstat 看不到，所以改用
`cpu.stat` 的 `usage_usec` 与墙钟的差。两种口径在正文里都标明了，不能混着比。

## 结果怎么读

`results/*.txt` 是原始行，一行一次测量，形如：

```
DEC policy=SCHED_OTHER tokens=120 wmb=32 p50=15880424 p99=47366529 p999=114214243 \
    max=114214243 mean=17479758.9 total_ms=2097.6 runq_wait_ms=1.6 exec_ms=2099.6 \
    wait_pct=0.08 slices=12 unit=ns sink=128974125.5
```

`p50/p99/p999/max/mean` 单位 ns；`runq_wait_ms` 是可运行但没轮到的时间；`slices` 是被换出又换回的次数；
`sink` 是反优化屏障的下沉值，用来确认计算真的发生了（每次运行必须一致，不一致就说明编译器把活儿优化掉了）。
