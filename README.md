# 大模型推理 · CPU 侧实证验证集

配套公众号系列《CPU 硬件 → 大模型推理》的系列成稿，把每一篇里讲到的机制，
在真实机器上写成可运行的 C 程序跑一遍，把数据留下来。目录按**实验顺序**编号，
篇号见下面各章节的对照表（两者不一致，因为有的篇目拆成了多组实验）。

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

## 追加：`ohos/` —— 同一套代码换到 OpenHarmony 上再跑一遍

上面所有的数字都来自 openEuler 24.03（glibc + gcc 12.3）。`ohos/` 把同一批 `.c` 文件用
**OpenHarmony 5.0.3 的 NDK（clang 15 + musl）** 交叉编译成 `x86_64-linux-ohos` 静态程序，
在 QEMU 里用 **OpenHarmony 自己编的内核**（Linux 6.6.62，`x86_64_virt`）直启一个自带 initramfs 跑完 24 组。

```bash
OH_SDK=/path/to/ohos-sdk/linux/15/native OUT=./bin bash ohos/build_oh.sh
$OH_SDK/llvm/bin/clang --target=x86_64-linux-ohos --sysroot=$OH_SDK/sysroot \
    -static -O2 -o ./init ohos/initramfs/init.c
BIN=./bin INIT=./init bash ohos/mk_initrd.sh ./initrd.img
KERNEL=/path/to/bzImage INITRD=./initrd.img BIN=./bin bash ohos/qemu_boot.sh
```

结果在 [`ohos/results/oh_summary.txt`](ohos/results/oh_summary.txt)（282 条 `RES|`），
环境自证在 [`ohos/results/oh_env.txt`](ohos/results/oh_env.txt)。
口径差异（没有 THP、没有 cpufreq、噪声地板 45.9%）见 [`ohos/README.md`](ohos/README.md)。


## 追加：`22_cache_stride/` —— 步长、Cache 与访存优化（十二个实验）

对应《同一段代码只把步长改大一点，为什么就慢了 8 倍？》那一篇。
原稿的数据是在一台 Windows Server 上量的，这一套把它整体搬到本机 openEuler 虚拟机上重跑，
**文章里的每一个数都出自这里的代码和原始日志**。

| 测什么 | 关键数字 |
|---|---|
| 工作集 16 KB → 256 MB，只改数组大小 | 82.5 → 13.85 GB/s（**5.96×**），台阶在 1 MB 与 8 MB |
| 步长 4 B → 64 B（800 万次访问固定） | 0.990 → 4.899 ns（**4.95×**） |
| 64 MB 里随机指针追逐 vs 顺序读 | 118.4 vs 0.304 ns（**389.3×**） |
| 组冲突：步长 4096 B vs 4160 B | 2.060 vs 1.136 ns（**1.81×**） |
| AoS vs SoA（800 万条记录只读 key） | 9.70 vs 2.69 ms（**3.61×**），等效带宽却几乎一样 |
| 1024³ fp32 矩阵乘分块 | 3,642 → 955 ms（**3.81×**，`-O2`） |
| 手动预取（256 MB 质数步长） | 近距更慢：8 步慢 1.04×，32 步才快 1.06× |
| 4 线程伪共享 / 聚合带宽 | 1.47×；带宽 12.5 → 20.3 GB/s 后见顶 |
| 638.7 MiB 权重每 token 过一遍（decode） | 52.24 ms → 19.1 字/秒；int8 / int4 为 75.6 / 155.0 字/秒 |
| 同样请求 32 MiB，复用块 32 KiB / 256 KiB / 4 MiB / 32 MiB | 30.23 / 29.22 / 20.50 / 13.13 GB/s（2.30×） |
| 软件模型数 miss（perf 的 vPMU 不可用） | 步长 64 B 每次访问搬 64 B；分块让冲突缺失降 15.3× |

```bash
cd 22_cache_stride
bash run_all.sh    # 编译 + 逐项跑，结果写 results/
python3 med.py     # 从原始 RES| 行算中位数，和文章里的数字逐项对得上
```

踩过的坑（加法器吃掉台阶、依赖链吃掉步长、冲突足迹要挑 256 KB、分块必须 i-k-j 且 `-O2`、
预取在乱读上是负收益、探针表墓碑导致 miss 模型退化）写在
[`22_cache_stride/README.md`](22_cache_stride/README.md)，
完整数据与结论见 [`22_cache_stride/REPORT.md`](22_cache_stride/REPORT.md)。

## 追加：`23_membw/` —— 内存带宽与 CPU 瓶颈（五个实验 + 屋顶线）

对应《同样四个核，为什么「一个字一个字往外吐」时只剩 1/93 的算力？》。
上一篇量「一次访问多贵」，这一篇量「一秒能搬多少」和「算力有几斤」，然后把两者画成屋顶线。

| 测什么 | 关键数字 |
|---|---|
| read / copy / scale / triad，1–4 线程 | 单线程 read **15.89 GB/s**（纸面 38.4 的 41%）；copy 13.57；4 线程 23.17 < 2 线程 24.50 |
| 128 MiB 随机环，1–32 条指针链 | 延迟 109.7 → 134.4 ns 几乎不变，带宽 0.58 → 3.81 GB/s（**6.52×**） |
| 峰值算力：1 vs 8 累加器、1 vs 4 线程 | 10.65 / 87.21 / 345.66 GFLOPS，ILP 收益 8.19× |
| 算术强度 0.5 → 64 扫描 | 7.81 → 82.02 GFLOPS，实测弯折在 AI 8–32 |
| batch 1 → 256（64 MB 权重） | 每 token 9.06 → **1.11 ms**（8.16×），AI 0.5 → 113.8 |

由 13 与 15 得到这台机器的两条边：单线程 `min(87.21, 15.89×AI)`，屋脊点 **AI = 5.49**；
四线程 `min(345.66, 23.17×AI)`，屋脊点 **AI = 14.92**。fp32 的 decode 在 AI = 0.5，离屋脊 11 倍。

```bash
cd 23_membw
bash run_bw.sh      # 编译 + 逐项跑，结果写 results/
python3 med_bw.py   # 从原始 RES| 行复算每个数字，含屋顶占比
```

分析报告：[`23_membw/REPORT.md`](23_membw/REPORT.md)。踩过的坑（R 与 reps 搞混算出 222 GFLOPS、
batch 循环顺序两版全错、随机链撞的是 DRAM 行激活而不是 TLB）写在
[`23_membw/README.md`](23_membw/README.md)。

## 追加：`24_numa/` 到 `27_gemm/` —— 第 19~22 篇

这四个目录各自有独立的 README（环境、跑法、数字、边界都在里面），索引如下：

| 篇 | 主题 | 目录 | 这台机器上怎么做的 |
|---|---|---|---|
| 19 | 内存就在隔壁：线程读它为什么要绕路 | `24_numa/` | openEuler 4 vCPU 划成 2 个 vNUMA 节点（0-1 / 2-3），量跨节点访问与亲和性 |
| 20 | DMA 与共享内存：CPU 怎么和 GPU/NPU 换数据 | `25_dma/` | openEuler 6.12 + vmwgfx 虚拟显卡（renderD128），六条通路 + dma-buf |
| 21 | 大模型 CPU 性能分析：怎么定位真正的瓶颈 | `26_perf/` | perf / ftrace / 火焰图，另加 llama.cpp 本体对照（3 线程 25.8 ms/token） |
| 22 | 算子优化：一道矩阵乘法的六次改写 | `27_gemm/` | 11 个 GEMM 版本逐个改一个变量，512³ 从 303 ms 到 2.37 ms |

## 追加：`28_sched_qos/` —— CPU 调度优化：把「等 CPU」从毫秒压到微秒

对应第 23 篇《大模型 CPU 调度优化：如何降低 AI 推理延迟？》。
前面 21、22 篇都在问「算得够不够快」，这一篇反过来问：算得一样快，为什么有的请求就是慢？
答案在「等」上——而「等」只占无竞争场景的千分之零点几，一旦有人抢，它能吃掉 90%。

平台换成了 Ubuntu 24.04.5 / 内核 6.8.0-149（EEVDF），4 vCPU，`gcc 13.3.0`，
九个实验段（A–K）一次跑完约 5 分钟。

| 测什么 | 关键数字 |
|---|---|
| 无竞争的 decode（32 MB 权重） | 每 token p50 **15.74 ms**，排队等待只占 **0.02%**；执行 16.45 ms 是唤醒延迟（12.13 µs）的 **1356 倍** |
| 同核挤 3 个自旋 | `SCHED_OTHER` p99.9 **14.19 ms**；`SCHED_FIFO` **11.12 µs**（约 1276 倍差）；但 p50 两者都还是 11 µs |
| 4 线程固定工作量 | 不绑核 880.7 ms → 绑核 **756.2 ms**（+14%）；4 线程全挤 1 核 **1545.5 ms**（等于单线程） |
| cpuset 隔离 | 「等 CPU」占比 3.96% → **0.09%**；同一 cpuset 里再加一个竞争者回到 5.45% |
| `cpu.max` 配额 50% / 10% | 每 token 34.29 ms（等待 50.81%）/ **242.45 ms**（90.67%）；执行时间几乎不变（1973 → 2714 ms） |
| `cpu.weight` 权重 | 100:300 实测 **1:2.68**，100:10 实测 **1:0.10**，100:100 实测 1:1.00 |
| 饥饿 | FIFO 纯自旋压住一颗核，普通进程只剩 **5.55% / 5.09%** 的 CPU（正好是 950/1000 的余量） |
| 综合（有干扰） | 什么都不做 27.81 ms = 36 token/s → 只做隔离 **20.01 ms = 50 token/s**（+39%）；隔离 + 实时优先级反而 **31.49 ms**（−12%） |

两条最容易踩反的结论：

1. **给长任务加实时优先级会更慢**。它一旦上 CPU 就不肯让，被 RT 节流按周期掐断，
   实测反比不做任何事慢 12%；`wait_pct` 却只有 0.01%——「没在排队」不等于「跑得快」。
2. **给后台降 nice 救不了前台的长尾**。后台仍然在同一颗 LLC 上刷 32 MB 数据，
   缓存和内存带宽的争用跟优先级无关。

```bash
cd 28_sched_qos
gcc -O2 -std=gnu11 -pthread -o sched23 code/sched23.c
sudo bash code/run23.sh ALL > /tmp/run_all.txt 2>&1
python3 code/build_med23.py results > results/summary.txt
```

正文数字的来源在 `results/sec*.txt`（原始行，未经修改）；`results/rerun_busy/` 是同一天宿主较忙时的重跑，
**排序发生了反转**——它本身就是「虚拟机上单次横向对比不可信」的证据。
踩过的坑（握手序号错位死锁、循环被优化掉、`g_sink` 声明在使用之后、cpuset `EACCES`、
饥饿实验口径全错、CRLF 造出 `results\r` 目录）写在
[`28_sched_qos/README.md`](28_sched_qos/README.md)，
完整数据见 [`28_sched_qos/REPORT.md`](28_sched_qos/REPORT.md)。
