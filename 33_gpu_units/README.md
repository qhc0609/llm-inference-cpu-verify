# 33_gpu_units — GPU 第 03 篇配套工程

文章：《GPU 执行单元——SM、CUDA Core 和 Tensor Core 如何协同计算？》

## 环境
- Kaggle Tesla T4（cc 7.5，40 SM，15.6 GB），CUDA 12.x
- 编译：`nvcc -O3 -arch=sm_75 -o /tmp/gpu03 /tmp/gpu03.cu`

## 目录
- `code/gpu03.cu` — 微基准源码（dev/ccount/reg/peak/gemm_f32/gemm_f16/gemm_int8）
- `results/raw_kaggle_t4.txt` — Kaggle T4 实测原始数据与口径说明
- `figs/` — 6 张配图

## 核心结果（N=1024 GEMM，统一口径 2·N³·reps/ms）
| 模式 | ms(总) | GFLOPS |
|---|---|---|
| FP32 CUDA Core（朴素 FMA） | 84.7213 | 507.0 |
| FP16 Tensor Core（WMMA） | 27.8200 | 3859.6 |
| INT8 Tensor Core（WMMA） | 14.4201 | 7446.1 |

- F16 / F32 = 7.61×；INT8 / F32 = 14.69×
- 纸面峰值（规格×频率）：FP32 16282 GFLOPS，FP16 65126 GFLOPS
- BF16 / TF32 需 cc≥8.0，T4（cc7.5）不支持，实验中 SKIP，未冒充实测。
