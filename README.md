# 大模型推理 · CPU 侧实证验证集

配套公众号系列《CPU 硬件 → 大模型推理》的 16 篇成稿，把每一篇里讲到的机制，
在真实机器上写成可运行的 C 程序跑一遍，把数据留下来。

不是教程，是**证据集**：文章里每一个数字，都能在这里找到产生它的代码。

## 环境

| 项 | 值 |
|---|---|
| 系统 | openEuler 24.03 LTS-SP4（x86_64） |
| 宿主 | VMware Workstation 虚拟机，4 vCPU / 8 GB |
| CPU | Intel Core i5-7300HQ @ 2.50 GHz |
| 缓存 | L1d 32KB / L2 256KB / L3 6MB |
| 编译器 | gcc 12.3.1 (openEuler 12.3.1-110.oe2403sp4) |
| 交叉工具链 | gcc-cross (aarch64-linux-gnueabi) + qemu-aarch64-static |

## 跑起来

```bash
bash build.sh          # 编译全部 15 个 C 实验
bash run_all.sh        # 依次运行，结果写入 results/
```

单篇也可以单独跑：

```bash
gcc -O2 -std=gnu11 -I. -o 03_cache/cache_lat 03_cache/cache_lat.c -lm
./03_cache/cache_lat 4
```

从 Windows 侧远程部署（把工程推到虚拟机、编译、运行、取回结果）：

```bash
OE_PASS='<root 密码>' python deploy.py
```

## 16 篇与对应实验

| 篇 | 主题 | 目录 | 主要测什么 |
|---|---|---|---|
| 01 | CPU 硬件整体认知 | `01_topology/` | 核心拓扑、各级 cache 几何、不同工作集下的 memcpy 带宽 |
| 02 | 从 C 代码到 ARM64 机器指令 | `02_c2arm64/` | 同一份 C 交叉编译到 aarch64，比对指令序列与数值结果 |
| 03 | CPU Cache 体系 | `03_cache/` | 指针链延迟曲线，定位 L1/L2/L3/内存四个台阶 |
| 04 | CPU 流水线与分支预测 | `04_pipeline/` | 有序 vs 乱序数据的分支预测差异、依赖链延迟、ILP |
| 06 | 大模型推理执行流程 | `06_infer_flow/` | 一个最小 Transformer 前向，分阶段计时 |
| 07 | Tensor 与矩阵计算 | `07_gemm/` | 循环顺序、分块大小对 GEMM 吞吐的影响 |
| 08 | Transformer 与 Attention | `08_attention/` | 序列长度翻倍时耗时的平方增长；完整矩阵 vs 流式计算 |
| 09 | 大模型量化计算 | `09_quant/` | FP32 / INT8 / INT4 的带宽、速度与误差 |
| 10 | Prefill 与 Decode | `10_prefill_decode/` | 同一份权重，batch 1→512 时算力与带宽的翻转 |
| 11 | Linux 进程与线程 | `11_thread/` | fork 与 pthread 成本、地址空间变大后 fork 的代价 |
| 12 | Linux 调度器 | `12_sched/` | 线程唤醒延迟的分布（p50 / min / p99）、竞争下的长尾 |
| 13 | CPU Affinity 与绑核 | `13_affinity/` | 绑核前后的墙钟时间与被动上下文切换次数 |
| 14 | ARM 大小核调度 | `14_biglittle/` | **能力受限，只做机制示意**，见下文 |
| 15 | CPU 并行计算 | `15_openmp/` | OpenMP 扩展性、假共享 vs 缓存行填充 |
| 16 | 大模型权重加载 | `16_mmap_load/` | mmap 虚拟映射、冷读/热读、缺页、随机 vs 顺序 |

> 编号跳过了 05，因为原系列就没有第 05 篇，这里保持与公众号一致。

## 三条必须说明的边界

写文章和看数据时都不能绕过这三条，否则就是造假。

### 1. 第 02 篇：aarch64 是交叉编译 + QEMU，不是 ARM 硬件

这台机器是 x86_64 虚拟机，没有 ARM 核心。aarch64 一侧的指令序列是
`gcc-cross` 真实产出的（`objdump` 可查），执行则跑在 `qemu-aarch64-static` 上。

另外，openEuler 的 aarch64 glibc 2.38 在这个 qemu 版本下启动会崩（rseq 未实现），
所以 ARM 侧用 `-nostdlib` 走纯系统调用（`svc`）输出结果。
这反而说明了一件事：`printf` 不在语言里，它只是 libc 的一层包装。

### 2. 第 14 篇：没有 big.LITTLE，这个实验无法真正验证

4 个核完全对称，`cpu_capacity` 内核根本不上报，也没有 EAS。

所以 `14_biglittle/` 做的是：证明这台机器是均质的，然后用"把一半任务压到一个核上"
人为构造出容量不均的处境，量出这种不均造成的任务完成时间离散度。
它只能说明"容量不均会怎样"，**不能**说明"ARM 上 EAS 调度得好不好"。

### 3. 虚拟机的时间测量有抖动，微秒级数字只看分布

宿主是共享的 4 核，vCPU 靠时间片切。同一个 fork 测试连续跑三次，
结果能差 3 倍；唤醒延迟的空闲平均值能跑出 6 μs 到 285 μs。

因此凡是微秒级的结论，本仓库一律给**分布**（min / p50 / p99）而不是平均值，
并且只对"多次重复方向一致"的结论下判断。平均值在这类环境里没有意义。

## 结果

全部原始输出在 `results/`，每个实验一个 `.txt`，
其中 `results/summary.txt` 是所有 `RES|` 行的汇总，`results/all.log` 是完整运行日志。

`RES|` 行的格式是 `RES|键=值|单位|补充`，方便直接 grep 成表。

## 追加：`17_llm_real/` —— 真的跑一次大模型

上面 15 个实验都是手写 C 程序模拟大模型的某一段计算。`17_llm_real/` 是它的反面：
用 llama.cpp 真的加载 Qwen2.5-0.5B-Instruct 的 GGUF 权重，真的算 token，真的让模型说话。

```bash
cd 17_llm_real
bash 00_setup.sh          # 编译 llama.cpp + 下载 q2_k / q4_k_m / q8_0 三个 GGUF
bash run_all.sh           # 全跑，结果写 /root/llm/results
```

六组结果（细节与边界见 [`17_llm_real/README.md`](17_llm_real/README.md)）：

| 测什么 | 关键数字 |
|---|---|
| 线程 1→4，预填充 | 50.60 → 199.79 t/s（**3.95×**，接近线性） |
| 线程 1→4，解码 | 18.14 → 50.32 t/s（**2.77×**，带宽受限） |
| 量化三档（q2/q4/q8） | 解码 61.81 / 52.21 / 41.47 t/s；困惑度 16.10 / 15.37 / 14.97 |
| mmap vs 读进堆 | 总内存都是 758 MB，但文件页 664 MB ↔ 匿名页 733 MB |
| KV cache | 每 token 12 KiB；默认 32768 上下文 = **384 MiB** |
| KV 深度 0→4096 | 解码 52.04 → 25.61 t/s（**−50.8%**） |

还有一条必须先看的：同一配置独立跑 5 次，极差 **11%~15%**。
本目录所有数字只在数量级上可信，不能用来比较 5% 的提升。

