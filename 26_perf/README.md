# 26_perf — 大模型 CPU 性能分析

第 21 篇《大模型 CPU 性能分析：如何定位真正的性能瓶颈》的实验工程。
平台：openEuler 24.03 LTS-SP4，内核 6.6.0，4 vCPU（i5-7300HQ 2.5 GHz）/ 7.4 GiB，L3 6 MiB。

## 跑

```bash
bash run21.sh results21        # 六组实验，关键项 5–9 轮
python3 build_med_perf.py      # -> med_perf.json
python3 flame.py results21/perf.script flame.svg
python3 make_fig21.py          # -> fig21_flame.png / fig21_cmp.png
```

## 这台机器能测什么

| 能力 | 本机状态 |
|---|---|
| perf（软件事件） | 有，`task-clock` / `cpu-clock` / `context-switches` 可用 |
| PMU（硬件计数器） | **不可用**：`cycles`/`instructions`/`cache-misses` 全部 `<not supported>`，尽管 `arch_perfmon` 与 `/sys/bus/event_source/devices/cpu` 都在 |
| ftrace | 可用，`sched_switch` / `sched_wakeup` 正常 |
| 火焰图 | 可用（`-e cpu-clock -g --call-graph fp`），8 268 个样本 |
| 实时优先级 | 不可用，`chrt -f 50` 报 `Operation not permitted` |
| 频率调节 | 无 cpufreq，固定 2.496 GHz |

## 六组实验

| 文件 | 实验 | 关键结果 |
|---|---|---|
| `21b_loads.c` | 依赖链 / 四路并行 / 随机置换环 | 每次 FMA 4.89 → 0.84 周期（5.8×）；随机访存 505 周期 |
| `21c_branch.c` | 排序 vs 随机分支 | 2.06 → 16.06 周期/元素（7.8×） |
| `21d_wss.c` | 16 KiB→256 MiB 工作集 | 顺序读 72→22.4 GB/s；随机 2.6→275.6 ns |
| `21e_threads.c` | 1→12 线程 | 3 线程 4.1× 见顶，4 线程掉到 2.6× |
| `run21.sh` | ftrace sched_switch | 0.25 s 内 145 次切换，79% 是主动让出 |
| `21f_gemv.c` | Qwen2.5-0.5B Decode 复刻 | 149 ms/token，3.31 GB/s，`gemv_q8_0` 占 96.1% |

详细分析见 [REPORT.md](REPORT.md)。

## 结论

Decode 的瓶颈不在算力，也不在 DRAM 带宽，而在 `gemv_q8_0` 内核的解包效率：
单线程只跑到机器顺序读带宽的 15%。加线程到 4 个会因 vCPU 超卖而倒退，
真正的收益来自重写内核（`maddubs` 整数点积 + 多行并行）。
