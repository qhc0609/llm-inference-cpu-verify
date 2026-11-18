# 25_dma — DMA 与共享内存

第 20 篇《DMA 与共享内存：CPU 怎么和 GPU/NPU 换数据》的实验工程。
平台：openEuler 24.03 LTS-SP4，内核 6.12.0，4 vCPU / 15 GiB，vmwgfx 虚拟显卡（renderD128）。

## 跑

```bash
bash run_dma.sh results_dma      # 六组实验，各五轮
python3 build_med_dma.py         # -> med_dma.json
```

## 这台机器能测什么

| 能力 | 本机状态 |
|---|---|
| dma-buf 核心 | `CONFIG_DMA_SHARED_BUFFER=y` |
| 真实 dma-buf 导出 | 可以：`/dev/dri/card0` + `SET_MASTER` → `CREATE_DUMB` → `PRIME_HANDLE_TO_FD` |
| dma-heap / udmabuf | 都没有（`CONFIG_UDMABUF` 未开，无 `/dev/dma_heap`） |
| IOMMU | 有但 `Passthrough`，0 个 iommu group |
| 真实 DMA 引擎 | virtio-scsi（`O_DIRECT` 直入用户页） |
| clflush / clflushopt | 都有，`clwb` 没有 |

## 文件

| 文件 | 作用 |
|---|---|
| `20a_topo.sh` | 拓扑与能力探测 |
| `20b_copy.c` | memcpy / 只读 / 只写，1/2/4 线程 |
| `20c_cache.c` | clflush / clflushopt 每行成本、flush 后重读 |
| `20d_pin.c` | mlock（= DMA map 的钉页步骤）与按块映射开销 |
| `20e_path.c` | 六条数据通路：pipe / socket / sendfile / splice / vmsplice / memfd |
| `20f_dmabuf.c` | 真 dma-buf：创建、导出、双向一致性、SYNC 成本 |
| `20g_direct.c` | O_DIRECT：请求大小 vs 吞吐/CPU（描述符粒度） |
| `figs/fig20_arch.png` | 交付物：数据交换架构图 |
| `figs/fig20_flow.png` | 文章中用的三面板数据图 |

## 方法学

宿主机是单路 i5-7300HQ，虚拟机 4 vCPU 与宿主争核，单次测量抖动可达 4 倍。
因此每个指标取**五轮最优值**代表硬件上限，并在文中注明。绝对带宽仅代表这台虚拟机。
