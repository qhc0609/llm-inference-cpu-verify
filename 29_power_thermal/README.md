# 大模型持续推理的功耗与温控：在一台「什么都读不到」的机器上怎么判降频

第 24 篇配套实验。一台 4 vCPU 虚拟机上跑一个最小的 decode 循环（32 MB 权重，每 token 过一遍），
连续跑 240 秒 / 四线程 180 秒 / 间歇四轮 / cgroup 配额三档，
用 `getrusage(RUSAGE_THREAD)` 逐 token 采样、取窗口内 **p10** 作为「频率代理指标」，
把「CPU 降频」和「CPU 份额被抢」这两种都会让 token/s 下降的情况分开。

平台：Ubuntu 24.04.5 / 4 vCPU（i5-7300HQ）/ 内核 6.8.0-149 / gcc 13.3.0。

## 先说清楚：这台机器读不到什么

| 想读 | 读法 | 结果 |
|---|---|---|
| 当前频率 | `/sys/devices/system/cpu/cpu0/cpufreq` | **目录不存在** |
| 温度 | `/sys/class/thermal/thermal_zone*/temp` | **0 个 zone** |
| 降温设备 | `cooling_device*/type` | 4 个 Processor，没绑任何 zone |
| 功耗 | `/sys/class/powercap` | **空** |
| 实际频率 | `rdmsr 0x198` | 四核全读出 0 |
| 硬件计数 | `perf stat -e cycles,instructions` | `<not supported>`（paranoid=4） |
| 标称频率 | `/proc/cpuinfo` | 2496.005，连读 5 次不变（静态值） |

所以本篇**没有任何频率、温度、功耗的直接测量**。所有结论都是间接推断，推断方法见下面「方法」一节。

## 方法：两个数一起看

- `tokens_per_s`：墙钟吞吐，会被「分到多少 CPU 时间」影响；
- `cpu_us_per_token`（取窗口内 **p10**）：每 token 真正占用的 CPU 时间，与频率严格成反比。

判别表：

| 现象 | token/s | 每 token 的 CPU 时间 | 结论 |
|---|---|---|---|
| 真降频 | 下降 | **上升** | 频率掉了 |
| 被抢 CPU | 下降 | **基本不变** | 活还是那么快，只是没轮到它 |
| 换更重模型 | 下降 | 上升 | 每 token 要搬的数据变多 |
| 缓存命中变差 | 下降 | 上升（幅度小） | 同样字节数，等内存更久 |

## 结论速览

| 测什么 | 关键数字 |
|---|---|
| 单线程 240 s | token/s 34.8~50.5（中位 **46.3**），p10 15957~16964 μs（中位 **16100**），无单调上升 |
| 四线程 180 s | token/s 45.6~136.8（中位 **103.7**），p10 中位 **15953 μs**，与单线程几乎相同 |
| 并行效率 | 1→4 线程只有 **2.24 倍**，判定为内存带宽受限，非降频 |
| 间歇 40 on / 20 off ×4 | 92.4 → 99.3 → 86.5 → **74.8** token/s，每 token 的 CPU 时间 34.2→40.7 ms（一起坏，但无法归因温度） |
| cgroup 配额 100/50/25% | 吞吐 47.2 / 26.1 / **14.0** token/s（近似线性），每 token CPU 时间 20.1→18.2→**17.3** ms（**不升反降**） |
| 恢复探针 120 s | 前 9 窗口均值 169.0、后 6 窗口均值 **203.2** Mops/s，无持续衰减 |

三条结论：① 本机没有降频通道，测不到不等于不存在；② 吞吐的大幅抖动确实全部来自抢占（由 PSI 与配额对照共同支撑）；
③ 判断「变慢」必须两个数一起看。

## 目录

```
code/pow24.c       实验主程序：sustain / sustain2 / burst / probe 四种模式
code/run24.sh      A 接口自证 / B 短测 / C 单核 240s / D 四核 180s / E 恢复 / F 间歇 / G 配额
code/run24b.sh     补跑 C2 / D2（带 p10 采样口径）
code/run24c.sh     只跑 G 配额组（修掉 cpu.max 写入写法之后的版本）

results/env.txt            环境与「读不到」清单的原始回显
results/secA_iface.txt     A 组：逐个接口去读
results/secB_short.txt     B 组：五次 10 秒短测（噪声基线）
results/secC_single.txt    C 组：单线程 240 s（平均值口径）
results/secC2_p10.txt      C2 组：单线程 240 s（p10/p50 口径，正文用这组）
results/secD_quad.txt      D 组：四线程 180 s（平均值口径）
results/secD2_p10.txt      D2 组：四线程 180 s（p10/p50 口径，正文用这组）
results/secE_recover.txt   E 组：满载后恢复曲线
results/secF_burst.txt     F 组：40 s 开 20 s 停 ×4 轮
results/secG_quota.txt     G 组：cgroup 配额 100/50/25%

figs/f1_single.png   单线程：吞吐在抖、p10 不动（归一化后同轴）
figs/f2_quad.png     四线程：同上，附 PSI 柱
figs/f3_compare.png  单核 vs 四核（双面板：token/s 与 p10 各用各的轴）
figs/f4_quota.png    配额三档（双面板）
```

## 怎么跑

```bash
cd /tmp && mkdir -p sched24/results && cd sched24
gcc -O2 -o pow24 pow24.c -lpthread
bash run24.sh          # 需要 root：会写 /sys/fs/cgroup
```

配额组要用 sudo，脚本里已经包了 `S() { echo 1 | sudo -S -p '' "$@"; }`——
注意 `echo ... | tee` 的坑：`sudo` 排在 `tee` 前面时，`echo 1` 会被 `tee` 当成输入，把 `cpu.max` 写成 1。

## 边界

本篇的「没有观察到降频」是**被环境决定的**：这台机器没有 cpufreq、没有 thermal zone、没有 RAPL，
它连降频这个动作都做不了。换到有这些接口的真机上，方法（吞吐 + p10 双指标）仍然成立，
但要补上「频率」这第三条证据链。详见 `REPORT.md` 的第六节。
