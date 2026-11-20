# CPU 算子优化：一道矩阵乘法的六次改写

第 22 篇配套实验。同一道 `C[MxN] = A[MxK] * B[KxN]`（fp32，行主序），写了 11 个版本，每个版本只改一个变量，在 openEuler 虚拟机上一次跑完。

平台：openEuler 24.03 LTS-SP4 / 4 vCPU（i5-7300HQ）/ L3 6 MiB / GCC 12.3.1。

## 结论速览（512×512×512，单线程）

| 版本 | 毫秒 | GFLOPS | 相对基线 |
|---|---|---|---|
| 教科书 ijk | 303.42 | 0.88 | 1× |
| 换循环顺序 ikj | 14.34 | 18.72 | 21× |
| 4×8 寄存器分块 | 10.37 | 25.88 | 29× |
| 8×8 寄存器分块 | 6.51 | 41.20 | 46× |
| 分块 + 调参 | 5.89 | 45.54 | 52× |
| 再加 3 线程 | 2.37 | 113.07 | 128× |

单核 FMA 峰值实测 102.65 GFLOPS。

## 目录

```
code/22gemm.c        11 个内核 + 6 种驱动模式（bench/block/peak/fusion/int8）
code/22run.sh        全套测量（阶梯、分块扫描、线程、融合、低精度）
code/build_med22.py  CSV -> med_perf22.json（正文所有数字的唯一来源）
code/make_fig22.py   三张图（PIL 直画）
results/*.csv        原始输出，每行一次运行
results/med_perf22.json
figs/                三张图
REPORT.md            完整实验报告：方法、全部表格、可信度边界、踩坑记录
```

## 复现

```bash
gcc -O3 -mavx2 -mfma -o 22gemm 22gemm.c -lm -lpthread
gcc -O3 -mavx2 -mfma -fno-tree-vectorize -o 22gemm_novec 22gemm.c -lm -lpthread
systemctl isolate multi-user.target
bash 22run.sh
python3 build_med22.py && python3 make_fig22.py
```

## 说明

- 这台虚拟机是 x86_64，**没有 NEON**。正文第 4 节给的是 NEON 与 AVX2 的指令映射，以及 x86 上的等价实验（手写汇编 vs intrinsic）。
- 机器无 PMU（详见第 21 篇），本文只有墙钟，没有 IPC 与 cache miss。
- 宿主机同时跑着别的虚拟机，单配置抖动 1.2~1.8 倍，正文报的是多次运行的最快值。
