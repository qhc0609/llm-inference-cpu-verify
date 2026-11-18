# 24_numa — NUMA 与内存亲和性

第 19 篇《内存就在隔壁：为什么你的线程读它要绕路？》的实验工程。
平台：openEuler 24.03 LTS-SP4，4 vCPU / 16 GiB，2 个 vNUMA 节点（0-1 / 2-3）。

## 跑

```bash
bash run_numa.sh results_numa      # 七组实验，一轮扫描
REPS=5 bash rep.sh results_ab      # 本地/远端交替五轮（对照用）
bash bw_only.sh results_bw         # 整机 STREAM 五轮
python3 build_med_numa.py          # -> med_numa.json
```

## 重要边界

宿主机是**单路** i5-7300HQ。vNUMA 由 `numa.vcpu.maxPerVirtualNode=2` 生成，
内核拓扑是真的，但两个节点背后是同一套内存控制器。因此：

- 能测：拓扑、首次触碰落点、页迁移、伪共享、带宽饱和点
- 测不出：本地/远端延迟差、跨节点带宽折损、互连拥塞

详见 `REPORT.md` 第 6 节。

## 文件

| 文件 | 作用 |
|---|---|
| `19a_topo.sh` | 拓扑（sysfs + numactl + numastat） |
| `19b_latency.c` | 指针链延迟，显式大页 + 回读 AnonHugePages |
| `19c_firsttouch.c` | 首次触碰四阶段，逐轮墙钟聚合 |
| `19d_migrate.c` | `move_pages` 双向迁移，每轮先无计时迁回 |
| `19e_share.c` | 伪共享：同行/独占行 × 同节点/跨节点 |
| `19f_decode.c` | 256 MiB 权重 GEMV，逐 token 计时取最小 |
| `13_streambw.c` | STREAM 四内核（`BW_MAXT` 限制线程数上限） |
| `run_numa.sh` | 七组实验总入口 |
| `rep.sh` | A/B 交替五轮 |
| `bw_only.sh` | 整机带宽五轮 |
| `build_med_numa.py` | 原始日志 → `med_numa.json` |
