# 34_gpu_memory — GPU 第 04 篇配套工程

文章：《GPU 内存体系——显存、Cache 和 Shared Memory 如何工作？》

## 环境
- Kaggle Tesla T4（cc 7.5，40 SM，15.6 GB，L2 4 MB，256-bit），CUDA 12.x
- 编译：`nvcc -O3 -arch=sm_75 -Xptxas -v -o /tmp/gpu04 /tmp/gpu04.cu`
- 计时：cudaEvent，预热后多次取平均

## 目录
- `code/gpu04.cu` — 全部微基准源码
- `results/raw_kaggle_t4.txt` — Kaggle T4 实测原始数据
- `results/ptxas.txt` — 寄存器 / stack frame 编译报告
- `figs/` — 7 张配图（含内存层次架构图）

## 核心结果

| 实验 | 结果 |
|---|---|
| 全局内存连续读 | 197.3 GB/s；步长 8 掉到 34.4 GB/s |
| 共享内存无冲突 | 1338.7 GB/s；32 路冲突掉到 53.8 GB/s |
| 寄存器 vs 强制本地内存 | 91.2 vs 1513.9 ns/迭代（16.6×） |
| 常量内存广播 / 分散 | 20.4 / 364.2 ns 每次（18×） |
| 工作集 1 MB / 16 MB | 2184.8 / 217.5 GB/s |
| 指针追逐延迟 共享/32KB/4MB/256MB | 42.1 / 112.1 / 541.1 / 5148.0 ns |

说明：所有数字均为该次 Kaggle T4 会话实测；延迟类数据用指针追逐（相关访存）测得，
不能等同于硬件手册的标称延迟。
