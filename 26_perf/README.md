# 26_perf — 大模型 CPU 性能分析

第 21 篇《大模型 CPU 性能分析：如何定位真正的性能瓶颈》的实验工程。
平台：openEuler 24.03 LTS-SP4，内核 6.6.0，4 vCPU（i5-7300HQ 2.5 GHz）/ 7.4 GiB，L3 6 MiB。

## 跑

```bash
bash run21.sh results21      # 首轮：六组实验
bash 21h_retest.sh 7         # 复核：每档 7 轮交叉轮转（关键）
python3 build_med2.py        # -> med_perf2.json
python3 flame.py results21/perf.script flame.svg
python3 make_fig21.py        # -> fig21_flame.png / fig21_cmp.png
```

`21h_retest.sh` 是第二版加的。首轮把同一档连跑多次取最小值，如果这一档恰好撞上宿主忙，
整档数据全废——第一版就是这样算出「4 核 6.7× 加速比」的。交叉轮转把每档拆散到每一轮里去，
分母和分子承受同样的干扰。

## 这台机器能测什么

| 能力 | 本机状态 |
|---|---|
| perf（软件事件） | 有，`task-clock` / `cpu-clock` / `context-switches` 可用 |
| PMU（硬件计数器） | **不可用**：`cycles`/`instructions`/`cache-misses` 全部 `<not supported>`，尽管 `arch_perfmon` 与 `/sys/bus/event_source/devices/cpu` 都在；vmx 加 `vpmu.enable="TRUE"` 重启也无效 |
| ftrace | 可用，`sched_switch` / `sched_wakeup` 正常 |
| 火焰图 | 可用（`-e cpu-clock -g --call-graph fp`），8 268 个样本 |
| 实时优先级 | 不可用，`chrt -f 50` 报 `Operation not permitted` |
| 核心频率 | **读不到**：无 cpufreq，`cpu MHz` 恒显示 2496，但实际频率在变 |
| 有效并行度 | 峰值 3.4–3.6（4 vCPU 里有半个被宿主拿走） |

## 六组实验

| 文件 | 实验 | 关键结果 |
|---|---|---|
| `21b_loads.c` | 依赖链 / 四路并行 / 随机置换环 | 每次 FMA 10.40 → 1.97 周期（中位 5.3×，各轮 2.8–8.0）；随机访存 505 周期 |
| `21c_branch.c` | 排序 vs 随机分支 | 2.06 → 16.06 周期/元素（7.8×） |
| `21d_wss.c` | 16 KiB→256 MiB 工作集 | 顺序读 24.0→15.6 GB/s；随机 4.2→268.2 ns |
| `21e_threads.c` | 1→8 线程 | 有效并行度 1.0→3.4（4 线程），6/8 线程停在 3.5 |
| `run21.sh` | ftrace sched_switch | 0.25 s 内 145 次切换，79% 是被抢占 |
| `21f_gemv.c` | Qwen2.5-0.5B Decode 复刻 | 149.7 ms/token（5 轮 143.6–159.4），3.15 GB/s，`gemv_q8_0` 占 96.1% |

详细分析见 [REPORT.md](REPORT.md)。

## 结论

算力不是瓶颈（4 核只用到 24%），DRAM 带宽也不是（15.6 GB/s 里只吃到 20%）。
瓶颈在 `gemv_q8_0` 内核内的 int8 解包：每周期只处理 1.3 字节，而理论上限是 5–8 字节。
4 个线程就能用满物理核，再往上加只是排队；真正的收益来自重写内核
（`maddubs` 整数点积 + 多行并行）。

## 第二版修正了什么

| 第一版 | 真因 | 第二版 |
|---|---|---|
| 顺序读 72 GB/s（比 DDR4 理论值高 3 倍） | 把「一遍耗时」当「三遍总量」算带宽 | 24.0 / 15.6 GB/s |
| 4 核 6.7× 加速比 | 分母撞上低频被抬高 | 改用有效并行度 3.4 |
| 「3 线程见顶、第 4 线程掉下来」 | 同一批脏数据推出的假象 | 4 线程用满物理核，之后不再增长 |
| 5.8×（单点值） | TSC 按固定 2.5 GHz 换算，频率在变 | 5.3×（范围 2.8–8.0） |
| 3.31 GB/s / 15% | 用旧耗时算的带宽 | 3.15 GB/s / 20% |
