# GPU 第 01 篇 · A10 微架构实测

对应文章《GPU 微架构基础：GPU 究竟是如何执行 AI 计算的？》

这一篇只做一件事：把一块 **NVIDIA A10（Ampere, GA102, 24 GB）** 的硬件参数
从 `nvidia-smi` 和 CUDA 运行时里读出来，再用四个微基准把它「跑一遍」——
让纸面参数和实测数字对上，或者对不上（对不上的地方更有意思）。

## 环境

| | |
|---|---|
| 实例 | 魔搭（ModelScope）Notebook 免费 GPU 实例，8 vCPU / 58 GB / A10 24 GB |
| 系统 | Ubuntu 22.04.5 LTS，内核 6.8.0 |
| 驱动 | 570.211.01，CUDA 13.0（nvcc V13.0.88） |
| 计算框架 | torch 2.13.0+cu130 |
| CPU | Intel Xeon Platinum 8369B @ 2.90 GHz（AVX-512，16 vCPU 可见） |
| PCIe | Gen4 x16，实测 H2D 12.1 GB/s |

A10 是**虚拟化出来的 vGPU**（`pciBusID 0000:00:03.0`），所以 PCIe 带宽比裸金属低一截，
这一点在报告里单独标注了，不影响其他结论。

## 怎么跑

```bash
cd 31_gpu_arch/code
bash run.sh          # 依次跑完六组，输出落在 /tmp/gpu01/r_*.txt
```

单独跑某一组：

```bash
nvcc -O3 -arch=sm_86 -o gpu01 gpu01.cu
./gpu01 dev      # deviceQuery：72 SM / 6 MB L2 / 600 GB/s 这些数从哪来
./gpu01 chase    # 延迟阶梯：4 KB → 256 MB 的随机指针追逐
./gpu01 bw       # 带宽：copy 内核与 cudaMemcpy 对打
./gpu01 fma      # 算力：block 数从 1 加到 288
./gpu01 div      # warp 分歧：同样总工作量，两种切法
./gpu01 occ      # 占用率：块大小 32 → 1024
./gpu01 pcie     # PCIe H2D / D2H
```

## 踩过的坑（都留在代码里）

1. **CUDA 13 删掉了 `cudaDeviceProp::clockRate` 和 `memoryClockRate`。**
   网上大量示例代码还是 `p.clockRate`，在 CUDA 13 下直接编译失败。
   要改成 `cudaDeviceGetAttribute(&v, cudaDevAttrClockRate, i)`。
   报错长得像「class has no member」，很容易被误判成头文件版本问题。

2. **`cudaMemcpy` 循环测带宽，不补 `cudaDeviceSynchronize()` 会跑出 183 TB/s。**
   第一次测出「64 MB 拷贝 46358 GB/s」——原因是五次 memcpy 只是进了队列，
   计时器量到的是入队时间。加同步后回到 403 GB/s，和 copy 内核一个量级。

3. **`if (active) a = fmaf(...)` 测不出 warp 分歧。**
   编译器会把它变成**谓词执行**（predication），一个 FMA 而已，不值得分支。
   两种写法都 0.59 ms，差 0.00。改成「warp 内 trip count 不同」——
   一半线程循环 2N 次、另一半 0 次，总工作量不变——才测出 2.00 倍。

4. **`cudaMemcpy` 的 H2D / D2H 方向写反不会报编译错，跑起来才报 invalid argument。**
   H2D 是 `cudaMemcpy(dst_dev, src_host)`，我一开始写成了 `(h, d)`。

5. **block 数超过 SM 数之后性能不再涨，但也不会掉。**
   288 个 block 和 72 个 block 只差 1.1%，因为多出来的 block 是排队等空 SM，
   不是抢资源。

## 目录

```
code/     gpu01.cu（六个微基准）/ bench.py（精度与 decode）/ coop.py（协同）/ cpugpu.py（CPU 对照）/ run.sh
results/  15 份原始输出，终端重定向出来没二次加工
figs/     六张配图 PNG 原图
REPORT.md 数据汇总与结论
```
