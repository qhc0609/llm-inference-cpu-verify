# CUDA 线程组织与调度：A10 实测报告

- 设备：NVIDIA A10（GA102，Ampere，cc 8.6），23028 MiB，驱动 570.211.01
- 预算：72 SM / 1536 线程每 SM / 48 warp 每 SM / 16 block 每 SM / 65536 寄存器每 SM / 100 KB L1+共享
- CUDA：13.0（V13.0.88），`nvcc -O3 -arch=native`
- 主机：16 vCPU（Intel Xeon Platinum 8369B），Ubuntu 22.04

## 1. 一维 256 线程的 block 怎么切成 warp

```
LANE|blockDim=256|gridDim=3|warpsPerBlock=8|totalWarps=24
BLK_WARP|bid=0|warps=8|tid_range=[0,255]|gtid_range=[0,255]
WARP|gtid=0..31|bid=0|internal_warp=0|lane0..31=0,...,31
```

结论：`warp = gtid / 32`、`lane = gtid % 32` 在 block 内成立；
`blockIdx.x` 只影响 gtid 的起点（每块 256），不影响 warp/lane 的切法。

## 2. 二维 block（16×16）的真实映射

```
MAP|blockDim=(16,16,1)|gridDim=(2,2,1)|threadsPerBlock=256|totalWarps=32
BLK|bid=(0,0,0)|first_gid=0|last_gid=255
BLK|bid=(1,0,0)|first_gid=256|last_gid=511
BLK|bid=(0,1,0)|first_gid=512|last_gid=767
BLK|bid=(1,1,0)|first_gid=768|last_gid=1023
THR|gid=0|thread=(0,0,0)|warp=0|lane=0
THR|gid=15|thread=(15,0,0)|warp=0|lane=15
THR|gid=16|thread=(0,1,0)|warp=0|lane=16
THR|gid=32|thread=(0,2,0)|warp=1|lane=0
```

两条容易踩的：
1. `threadIdx.x` 只有 0~15，**一个 warp 横跨两行**（gid 0~31 = 第 0 行 + 第 1 行）。
2. 全局编号沿 x 最快、y 次之、z 最后：`gid = ((bz*gy+by)*gx+bx)*blockDim + tid`。

## 3. block 落到哪颗 SM

```
SMID|grid=72|nsm=72|sm_used=72|blocks_per_sm_min=1|blocks_per_sm_max=1
BLK2SM|0->0,1->2,2->4,...,35->70,36->1,37->3,...,71->71
SMID|grid=720|blocks_per_sm_min=9|blocks_per_sm_max=12
```

- 72 块时：先偶数号 SM，再奇数号 —— 不是 0,1,2,3。
- 720 块时每 SM 9~12 块（平均 10），**不保证均分**。
- 7200 块时每 SM 81~108 块，差距 33%，说明分配是动态的，谁先空谁接。

## 4. 每 SM 能住多少 block

| blockDim | 块/SM | 驻留 warp/SM | 驻留线程/SM | 受限资源 |
|---|---|---|---|---|
| 32 | 16 | 16 | 512 | block 名额（16） |
| 64 | 16 | 32 | 1024 | block 名额（16） |
| 96 | 16 | 48 | 1536 | block 名额（16） |
| 128 | 12 | 48 | 1536 | warp 名额（48） |
| 192 | 8 | 48 | 1536 | warp 名额（48） |
| 256 | 6 | 48 | 1536 | warp 名额（48） |
| 384 | 4 | 48 | 1536 | warp 名额（48） |
| 512 | 3 | 48 | 1536 | warp 名额（48） |
| 768 | 2 | 48 | 1536 | warp 名额（48） |
| 1024 | 1 | **32** | **1024** | warp 名额（48） |

三种上限取最小值：`min(16, 48/warpsPerBlock, 1536/blockDim)`，十行全部吻合。
1024 线程的 block 只能住 1 块，**512 个线程位永远空着**。

## 5. 尾效应

一个 wave = 72 SM × 6 块/SM = **432 块**。

| grid | wave | ms | 相对 432 |
|---|---|---|---|
| 431 | 1 | 0.2044 | 1.00 |
| 432 | 1 | 0.2043 | 1.00 |
| 433 | 2 | 0.2370 | **1.16** |
| 864 | 2 | 0.3950 | 1.93 |
| 865 | 3 | 0.4269 | 2.09 |

多派 **1 个** block，整个 kernel 慢 **16%**。

## 6. warp 数量与吞吐

| warp/SM | GFLOPS | 反推频率 MHz | 占当频产能 | 占纸面峰值 | 每 warp 每周期 FMA |
|---|---|---|---|---|---|
| 1 | 1533 | 1483 | 5.6% | 4.9% | 0.224 |
| 4 | 6122 | 1481 | 22.4% | 19.6% | 0.224 |
| 8 | 12010 | 1473 | 44.2% | 38.4% | 0.221 |
| 12 | 16902 | 1420 | 64.6% | 54.1% | 0.215 |
| 16 | 19917 | 1290 | 83.8% | 63.7% | 0.209 |

「占当频产能」= GFLOPS ÷（72 SM × 128 lane × 2 ÷ 1e9 × 当时频率）；
「占纸面峰值」= GFLOPS ÷ 31260（31260 是 1.695 GHz 下的纸面值）。
两者差出来的一截，就是功耗墙吃掉的频率。

- 1→8 个 warp，墙钟只多 2%（0.0601 → 0.0614 ms），吞吐涨 7.8 倍：
  **单 warp 时 SM 有 78% 的时间在等 FMA 的 4 周期延迟**。
- 13 个 warp 起频率掉到 1231 MHz（-17%），吞吐增长被功耗墙截断。

（"峰值占比"按 `GFLOPS / (72×128×2×1.695 GHz)`，即 31260 GFLOPS 计。）

## 7. 分支发散

| 写法 | warp 周期 | 相对 |
|---|---|---|
| 全 warp 同一循环次数 | 9159 | 1.00 |
| 半数 2 倍循环 | 18047 | **1.97** |
| 按 lane 微发散（差 1 次） | 9451 | 1.03 |
| 四组不同循环次数（最大 4 倍） | 35823 | **3.91** |

掩码证据：

```
BLK|full_mask=0xffffffff|branch_low_mask=0x0000ffff|branch_high_mask=0xffff0000
ballot_lane_lt16=0x0000ffff
```

`if (lane<16)` 时两个分支各自看到的 `__activemask()` 是互补的一半 ——
warp 被拆成两遍串行执行，代价接近 200%，不是 50%。

## 8. 归约与启动开销

| 项 | 值 |
|---|---|
| warp shuffle 归约 | 8.5 ns/block（256 线程） |
| 共享内存归约 | 11.0 ns/block |
| 1 块启动 | 2.16 µs |
| 100 万块启动 | 726 µs（0.73 ns/块） |

空 kernel 的启动底噪在 2 µs 量级；块数上万之后摊薄到亚纳秒。
