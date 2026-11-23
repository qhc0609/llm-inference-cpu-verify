# GPU 篇 02：Thread / Warp / Block / Grid

配套实验：用一块 NVIDIA A10（Ampere，24 GB）实测 CUDA 的线程组织与调度，
输出全部来自真机，没有估算。

## 目录

```
code/
  gpu02.cu     11 组微基准，一个文件搞定
  run.sh       编译 + 依次跑完 11 组，输出落到 raw/
results/
  r_dev.txt    设备属性（72 SM / 1536 线程每 SM / 48 warp / 6 MB L2）
  r_map.txt    gridDim/blockDim 与 gid、warp、lane 的对应关系（16×16 的 block）
  r_lane.txt   一维 256 线程 block → 8 warp，gtid 与 lane 的切分
  r_smid.txt   block → SM 的实际落点（72/144/720/7200 四种规模）
  r_occ.txt    10 种 blockDim 的驻留 block/warp 数与受限资源
  r_tail.txt   grid 越过 wave 边界（432 块）时的耗时台阶
  r_sched.txt  每 SM 1~16 个 warp 的吞吐与反推频率
  r_div.txt    四种分支发散写法，相对 warp 周期数
  r_ballot.txt __activemask() / __ballot_sync() 看到的分支掩码
  r_red.txt    warp shuffle 归约 vs 共享内存归约
  r_launch.txt 一次 kernel 启动的开销（1 ~ 100 万块）
figs/          正文配图 PNG 原图
```

## 复现

```bash
nvcc -O3 -arch=native -o gpu02 code/gpu02.cu
./gpu02 dev      # 也可以用 run.sh 一次跑完
```

单个模式：`dev map lane smid div ballot occ tail red launch sched`。

## 测量上的两个坑

1. **频率没稳住就计时**。A10 空闲时 SM 只有 210~300 MHz，直接计时量到的是爬坡数据。
   程序里每次计时前先空转 400 ms 把频率拉起来（`WARM` 行就是它的输出）。
2. **功耗墙会把频率拽下来**。warp 从 12 个/SM 加到 13 个时，有效频率从 ~1420 MHz
   掉到 ~1231 MHz，所以吞吐增长会突然变缓。`r_sched.txt` 里 `eff_clk_mhz` 这一列
   是 kernel 内 `clock64()` 的周期数除以墙钟时间反推出来的，不是猜的。
