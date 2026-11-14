# 20_runtime_probe —— 把测量工具挂到跑着的模型上

前面的 `01`~`19` 都是自己写一段 C，把某个机制单独拆出来测。这一组反过来：
不写任何计算代码，直接把 Qwen2.5-0.5B 跑起来，在它运行期间读内核给它记的账。

回答的问题只有一个：**模型在算的时候，这颗 CPU 到底在忙什么。**

## 怎么测的

三个约束决定了方法：

1. **虚拟机的硬件性能计数器不可用。** `perf stat -e cycles,instructions` 在本机返回
   `<not supported>` —— VMware 没有把 vPMU 暴露给 guest。IPC、cache-miss 这类需要 PMU
   的指标一个都拿不到，能用的只有 software event 和内核自己维护的计数器。这是硬边界。
2. **prefill 和 decode 必须分开算。** 一次推理里两段行为完全不同，平均没有意义。
3. **加载会污染数据。** 0.5B 模型加载要建页表、读 463MB 权重，这部分会把每 token 成本抬高。

针对第 2、3 条，用**差分法**：同一个用例跑 `r` 次和跑 1 次，两次的进程总 CPU 时间相减，
剩下的就是 `r-1` 次真实计算。加载、context 初始化、页表建立这些一次性开销自动抵消。

```
单次 CPU = (task-clock(r 次) - task-clock(1 次)) / (r - 1)
```

CPU 时间取自 `perf stat -e task-clock`：内核在每次上下文切换时精确累加，
比每 20ms 读一次 `/proc/<pid>/task/*/stat` 可靠 —— 后者的漏计误差会被差分放大 `r-1` 倍。

采集项与出处：

| 指标 | 来源 |
|---|---|
| CPU 时间（含所有线程） | `perf stat -e task-clock` |
| 上下文切换、迁移、页错误 | `perf stat -e context-switches,cpu-migrations,page-faults` |
| 在 CPU 上跑 vs 在队列里等 | `/proc/<pid>/task/*/schedstat` |
| 峰值 RSS、切换分类 | `/proc/<pid>/status` |
| 时间花在哪个函数 | `perf record -e cpu-clock -F 499 -g` |

## 怎么跑

```bash
bash stat_all.sh         # 主实验：prefill / decode × 1 线程 / 4 线程 + 三个量化档
python3 analyze.py       # 差分，出每 token 成本与占用核数
bash run_all.sh          # 附加：/proc 轮询曲线 + perf 采样
python3 parse_report.py  # 把 perf 采样压成热点表
```

## 结果

模型 `qwen2.5-0.5b-instruct-q4_k_m.gguf`（462.96 MiB），上下文 2048。
"占用"= 每 token 的 CPU 毫秒 ÷ 每 token 的墙钟毫秒，也就是这段时间里平均有几颗核在转。

| 用例 | 每 token CPU | 每 token wall | 占用 | 吞吐 |
|---|---:|---:|---:|---:|
| prefill 512 / 4 线程 | 26.74 ms | 9.26 ms | **2.89 核** | 108.0 t/s |
| prefill 512 / 1 线程 | 19.40 ms | 19.63 ms | 0.99 核 | 50.9 t/s |
| decode 128 / 4 线程 | 115.75 ms | 28.84 ms | **4.01 核** | 34.7 t/s |
| decode 128 / 1 线程 | 55.09 ms | 54.79 ms | 1.01 核 | 18.2 t/s |

线程扩展：

| 阶段 | wall 加速 | CPU 成本 | 结论 |
|---|---:|---:|---|
| prefill | **2.12×** | 1.38× | 用 38% 的额外 CPU 换 2.1 倍速度，划算 |
| decode | **1.90×** | **2.10×** | 用 2.1 倍的 CPU 换 1.9 倍速度，净亏 |

decode 那一行是这一组最硬的发现：**4 个线程把 4 颗核占满了（4.01），吞吐只涨 1.9 倍。**
换句话说，一半以上的 CPU 时间没有变成 token。`perf record` 直接给出了这半部分去哪了。

## CPU 时间花在哪

Self 占比。libgomp 在 openEuler 上是 strip 过的，只能看到地址，统一归成"同步"。

**prefill 512 token / 4 线程**

| Self | 符号 | 是什么 |
|---:|---|---|
| 57.17% | `tinyBLAS_Q0_AVX<block_q5_0, block_q8_0, float>::gemm4xN` | 量化矩阵乘，一次算 4 行 |
| 19.97% | libgomp barrier / spin | 线程同步 |
| 5.40% | `ggml_gemm_q4_K_8x8_q8_K` | q4_K 分块 GEMM |
| 4.72% | `tiled_run_microtile_avx2` | 分块调度，AVX2 微内核 |
| 3.81% | `try_to_wake_up`（内核） | 唤醒被 barrier 挂住的线程 |
| 3.18% | `ggml_compute_forward_flash_attn_ext_tiled` | 注意力 |

**decode 128 token / 4 线程**

| Self | 符号 | 是什么 |
|---:|---|---|
| 40.42% | libgomp barrier / spin | 线程同步 |
| 21.97% | `ggml_vec_dot_q5_0_q8_0` | 量化点积 |
| 16.38% | `ggml_vec_dot_q8_0_q8_0` | 量化点积 |
| 6.86% | `try_to_wake_up`（内核） | 唤醒被 barrier 挂住的线程 |
| 4.45% | `ggml_vec_dot_q6_K_q8_K` | 量化点积 |
| 2.96% | `ggml_gemv_q4_K_8x8_q8_K` | 矩阵向量乘 |

两段完全不同：

- prefill 的时间压在 GEMM 上（57%），同步只占 20%，2.89 颗核有 2.3 颗在算矩阵。
- decode 反过来，**同步是第一大项，占 40.4%**，三个 vec_dot 加起才 42.8%，
  而且被切成很多小段，每段之间都要过一次 barrier。

上下文切换数也印证这一点：同一次 decode，4 线程 1800 次，1 线程 67 次，差 27 倍；
prefill 是 3440 对 180，差 19 倍。线程不是被数据挡住，是被彼此挡住。

## 换量化档，看带宽这条线

同一条 decode，只换权重文件：

| 档位 | 体积 | 每 token wall | 占用 | 吞吐 | 等效带宽 |
|---|---:|---:|---:|---:|---:|
| q2_k | 395.9 MiB | 25.67 ms | 4.00 核 | 39.0 t/s | 16.2 GB/s |
| q4_k_m | 463.0 MiB | 28.84 ms | 4.01 核 | 34.7 t/s | 16.8 GB/s |
| q8_0 | 644.4 MiB | 31.65 ms | 3.86 核 | 31.6 t/s | **21.4 GB/s** |

体积涨 63%（q2 → q8），每 token 时间只涨 23% —— **时间不跟体积成正比**。
如果 decode 纯粹是"把权重搬一遍"，两者应该同步。差的那部分是固定开销：
每层都要过一次同步，每层都要走一遍调度，这部分跟权重多大没关系。

反过来看等效带宽：q8_0 已经跑到 21.4 GB/s，逼近这台机器 memcpy 的平台值 24.75 GB/s
（见 `19_audit`）；q2_k 只有 16.2 GB/s，说明它离带宽上限还远，
瓶颈不在内存，在"每层太小、同步太贵"。

## 边界

- **没有 IPC、没有 cache-miss、没有分支失败率。** 虚拟机没暴露 vPMU，
  这三类指标必须换物理机或开了 `vpmu.enable` 的虚拟机重跑，脚本不用改。
- **CPU 时间里含 spin 等待。** OpenMP 的 barrier 是自旋的，等的时候也算 CPU 时间。
  所以"每 token 115.75 ms CPU"不等于"算了 115.75 ms"，其中 40% 是在等。
  这也正是为什么 decode 的 CPU 成本会超过 1 线程的 2 倍。
- 0.5B 太小，一层的工作量喂不饱 4 颗核的同步开销。模型越大，同步占比越低，
  但不会消失 —— 这是张量并行绕不过去的固定成本。
- 所有 wall 时间带宿主抖动，同配置独立 5 次极差在这台机器上是 11%~15%。
  只报"差一倍""差 2.4 倍"这种量级，不报 5% 以内的差异。

## 线程数的后续修正

本节用 r=5 / r=1 单轮差分，4 线程 115.75 ms/token、34.7 t/s。后来在 `21_model_knobs`
里把线程扩展跑了三轮取最小值，同一配置是 93.83 ms/token、40.0 t/s —— 这台虚拟机单轮之间
的极差就有 15%，单轮值不能当结论用。**以 `21_model_knobs/results` 的三轮最小值为准**，
本节其余结论（预填充与解码的核数差异、同步占比、量化等效带宽）方向不变。
