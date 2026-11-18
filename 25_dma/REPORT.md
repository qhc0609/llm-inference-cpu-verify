# CPU 与异构计算设备的数据交换架构

> 交付物。配套图 `figs/fig20_arch.png`，数据 `med_dma.json`。
> 平台：openEuler 24.03 LTS-SP4 / 内核 6.12 / 4 vCPU / 15 GiB。

## 1. 三条通路，一张账单

CPU 想把一块数据交给 GPU 或 NPU，只有三种做法。它们的差别不在"快慢"，
而在于**谁的时间被占掉**。

| 通路 | 谁搬 | 实测吞吐 | 每字节 CPU 成本 | CPU 是否被占住 |
|---|---|---|---|---|
| ① memcpy | CPU 核 | 12.80 GB/s | 0.078 ns | 是，全程 |
| ② 设备 DMA | 设备（CPU 只提交描述符） | 0.65 GB/s（1 MiB/次） | 1.17 ns | 否，只付提交成本 |
| ③ dma-buf 共享 | 没人搬 | 12.43 GB/s | 0.08 ns | 否 |

②的绝对带宽比①低，因为这台虚拟机的 virtio 请求本身很贵；但**CPU 占用**
从"整核被吃住"降到"每 MiB 提交一次"。这就是 DMA 存在的理由：它不比 CPU 快，
它把 CPU 还给你。

## 2. 为什么"一次搬多大"比"搬多快"更重要

`O_DIRECT` 直读（设备直接写进用户页，中途不经过 page cache）的请求大小扫描：

| 请求 | 吞吐 | CPU |
|---|---|---|
| 4 KiB | 0.02 GB/s | 30.61 ns/B |
| 16 KiB | 0.06 GB/s | 8.05 ns/B |
| 64 KiB | 0.27 GB/s | 1.78 ns/B |
| 256 KiB | 0.47 GB/s | 1.22 ns/B |
| 1 MiB | 0.65 GB/s | 1.17 ns/B |

从 4 KiB 到 1 MiB，吞吐 32 倍，CPU 成本 26 倍。差的不是带宽，是**每个描述符的固定开销**：
提交、地址翻译、完成中断、唤醒。DMA 控制器的描述符环、批量提交、中断合并，
全部是为了把这个固定开销摊薄。

同一请求大小下，O_DIRECT 比"读进 page cache 再拷出来"省 1.43 倍 CPU——
省掉的就是那次内核到用户的拷贝。

## 3. 一致性不是免费的

x86 + PCIe 是缓存一致的，设备写进内存的数据，CPU 立刻能看见。实测：

- 通过 GEM 映射写 64 MiB，再从 dma-buf 映射读：16384 页，**0 页不一致**；
- 反方向同样 0 页不一致；
- `DMA_BUF_IOCTL_SYNC` 一对（START+END）耗时 **1.97 µs**，与数据量无关。

也就是说在这台机器上，"同步"是个空操作。

ARM 侧不是这样。那里 CPU 的 cache 与设备看到的 DRAM 之间没有硬件一致性，
必须由软件补：

| 操作 | 每 64 B 行 | 64 MiB 合计 |
|---|---|---|
| `clflush`（串行化） | 21.4 ns | 22.4 ms |
| `clflushopt`（可流水） | 2.0 ns | 2.1 ms |
| 对照：memcpy 搬一遍 | 5.2 ns | 5.2 ms |

**刷一遍缓存比搬一遍数据还贵 4.3 倍**（用 clflush 时）。换成可流水的指令才降到 0.4 倍。
这就是为什么 dma-buf 的 exporter 要在 `dma_buf_ops` 里实现 `begin_cpu_access` /
`end_cpu_access`，而不是让每个驱动自己写 flush 循环——分散的 flush 无法流水。

## 4. 映射一次，等于搬一次的价钱

DMA 不是"把地址给设备"这么简单，内核还要把用户页钉住（GUP + get_page）：

| 操作 | 成本 |
|---|---|
| mlock 256 MiB（65537 个 4 KiB 页） | 0.17 ms，2.5 ns/页 |
| 同样 256 MiB，2 MiB 大页 | 0.04 ms，323 ns/页（但只有 128 页） |
| 按 64 KiB 块反复 map/unmap | 5.24 µs/次 |
| 对照：拷贝 64 KiB | 5.12 µs |

按块映射的开销 ≈ 拷贝同样大小数据的开销。**每搬一次就映射一次，等于把成本翻倍。**
正确做法是映射一次、搬很多次（环形缓冲区、持久映射、IOMMU 直通）。

## 5. 零拷贝到底省了什么

| 通路 | 拷贝次数 | 吞吐 | CPU |
|---|---|---|---|
| pipe write/read | 2 | 3.71 GB/s | 0.25 ns/B |
| unix socket | 2 | 6.23 GB/s | 0.14 ns/B |
| sendfile | 1（内核内） | 4.66 GB/s | 0.21 ns/B |
| splice | 0 | 5.61 GB/s | 0.18 ns/B |
| vmsplice | 0（钉页） | 5.89 GB/s | 0.13 ns/B |
| memfd 共享映射 | 0 | 12.43 GB/s | 0.08 ns/B |

共享映射比管道快 3.35 倍、CPU 便宜 3.13 倍。但要注意：环回场景下接收端仍要读一遍，
所以 sendfile/splice 只比 pipe 快 1.3~1.5 倍——**零拷贝省的是发送侧 CPU，不是时间**。
真正把时间也省掉的，是"两边访问同一块物理页"。

## 6. 落到 RK3588

RK3588 的 CPU（4×A76 + 4×A55）、Mali-G610、RKNPU2 共享同一颗 LPDDR4x。
按本机效率折算（单线程为纸面的 30%，四线程 60%）：

- 可用带宽：单线程 10.3 GB/s，四线程 20.5 GB/s（纸面 34.1）
- 一块 64 MiB 特征图：拷贝 3.3 ms
- 同一块如果走"非一致 + 每行 flush"：**22.4 ms**，是拷贝的 6.8 倍

所以 RK3588 的 BSP 里三件事是必须的，不是可选优化：

1. **dma-heap + dma-buf**：分配一次，CPU/GPU/NPU 各自 mmap，谁都不复制；
2. **IOMMU 或 CMA 连续内存**：让设备拿到的是物理地址，不用 scatter-gather 拼；
3. **cache 维护收敛到 exporter 的 begin/end_cpu_access**：只对真正脏的区间做，
   并且用可流水化的指令。

这就是为什么 `dma_buf` 的 `begin_cpu_access(DMA_BUF_SYNC_READ)` 在 ARM 上
对应 `dma_sync_sg_for_cpu`（invalidate），`SYNC_WRITE` 对应 `dma_sync_sg_for_device`（clean），
而在 x86 上两者都是空函数——同一份驱动代码，两个平台的成本差 10 倍。

## 7. 边界

- 本机没有真实 GPU/NPU，`vmwgfx` 的 dumb buffer 落在系统内存里，读它**没有 PCIe 代价**。
  所以本文不给"设备内存比系统内存慢多少"的数字，那个数在真板上才有意义。
- 没有 dma-heap、没有 udmabuf，`dma-buf` 的实验是通过 DRM 的 PRIME 导出完成的，
  只覆盖了"导出 + mmap + 同步"这一段，没覆盖"import 到另一个驱动"。
- 延迟类数字（SYNC 1.97 µs）含 ioctl 固定开销，不代表缓存维护本身。
