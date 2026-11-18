#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 results/ 的原始 RES| 行，复算文章《同样四个核…》里出现的每一个数字。

用法：python3 med_bw.py
"""
import os
import statistics as st

BASE = os.path.dirname(os.path.abspath(__file__))
RES = next((os.path.join(BASE, d) for d in ("results", "results_bw")
            if os.path.isdir(os.path.join(BASE, d))), os.path.join(BASE, "results"))


def rows(name):
    """RES|<tag>|<f1>|<f2>|...  ->  [(tag, [f...])]，字段原样保留为字符串。"""
    path = os.path.join(RES, name)
    out = []
    if not os.path.exists(path):
        return out
    for ln in open(path, encoding="utf-8", errors="ignore"):
        ln = ln.strip()
        if not ln.startswith("RES|"):
            continue
        p = ln.split("|")[1:]
        out.append((p[0], p[1:]))
    return out


bw, mlp, peak, ais, batch = {}, {}, {}, [], {}
# 13: RES|bw|<kernel>|<threads>|<GB/s>
for tag, f in rows("13_streambw.txt"):
    if tag == "bw":
        bw["%s_%d" % (f[0], int(float(f[1])))] = float(f[2])
# 14: RES|mlp|<chains>|<lat ns>|<GB/s>
for tag, f in rows("14_latency_mlp.txt"):
    if tag == "mlp":
        mlp[str(int(float(f[0])))] = (float(f[1]), float(f[2]))
# 15: RES|peak|<variant>|<threads>|<GFLOPS>
for tag, f in rows("15_peakflops.txt"):
    if tag == "peak":
        peak["%s_%d" % (f[0], int(float(f[1])))] = float(f[2])
# 16: RES|ai|<AI>|<GFLOPS>|<GB/s>|<ms>
for tag, f in rows("16_ai_sweep.txt"):
    if tag == "ai":
        ais.append([float(x) for x in f[:4]])
# 17: RES|batch|<B>|<ms>|<GFLOPS>|<GB/s>|<AI>
for tag, f in rows("17_batch.txt"):
    if tag == "batch":
        batch[str(int(float(f[0])))] = [float(x) for x in f[1:5]]

ais.sort()
bw1, bw4 = bw["read_1"], bw["read_4"]
p1, p8, p4 = peak["fma1_1"], peak["fma8_1"], peak["fma8_4"]
knee1, knee4 = p8 / bw1, p4 / bw4

print("=" * 76)
print("机器：openEuler 24.03 LTS-SP4 / Intel i5-7300HQ 4C4T / 16 GiB（虚拟机）")
print("=" * 76)

print("\n① 顺序带宽（256 MiB 数组，远大于 6 MB 的 L3）")
for k in ["read_1", "copy_1", "scale_1", "triad_1", "read_2", "read_4"]:
    print("   %-9s %8.2f GB/s" % (k, bw[k]))

print("\n② 延迟与并发（128 MiB 随机环，每次跳 64 B）")
print("   链数    单链延迟 ns    聚合带宽 GB/s")
for k in ["1", "2", "4", "8", "16", "32"]:
    print("   %4s %13.1f %15.2f" % (k, mlp[k][0], mlp[k][1]))

print("\n③ 峰值算力（寄存器内 FMA，不读内存）")
print("   1 个累加器 1 线程  %8.2f GFLOPS" % p1)
print("   8 个累加器 1 线程  %8.2f GFLOPS" % p8)
print("   8 个累加器 4 线程  %8.0f GFLOPS" % p4)
print("   ILP 收益           %8.2f×" % (p8 / p1))

print("\n④ 算术强度扫描（单线程，128 MiB）")
print("   AI      GFLOPS     GB/s      ms     屋顶占比")
for a in ais:
    roof = min(p8, bw1 * a[0])
    print("   %5.2f %9.2f %8.2f %8.2f %8.0f%%" % (a[0], a[1], a[2], a[3], a[1] / roof * 100))

print("\n⑤ 批处理（64 MB 权重，K = N = 4096）")
print("   B      总 ms   每 token ms    GFLOPS        AI    屋顶占比")
for k in ["1", "2", "4", "8", "16", "32", "64", "256"]:
    v = batch[k]
    roof = min(p8, bw1 * v[3])
    print("   %4s %9.2f %12.2f %9.2f %9.1f %8.0f%%" % (k, v[0], v[0] / int(k), v[1], v[3],
                                                      v[1] / roof * 100))

print("\n⑥ 派生数字（文章里引用的倍数）")
print("   屋脊点            单线程 %.2f / 四线程 %.2f" % (knee1, knee4))
print("   四核峰值 ÷ GEMV   %.1f×" % (p4 / batch["1"][1]))
print("   随机 ÷ 顺序带宽   %.1f%%" % (mlp["32"][1] / bw1 * 100))
print("   1 → 8 链带宽      %.2f×" % (mlp["8"][1] / mlp["1"][1]))
print("   批处理 1 → 256    %.2f×（每 token）" % (batch["1"][0] / (batch["256"][0] / 256)))
print("   实测 ÷ 纸面 38.4  %.1f%%（DDR4-2400 双通道）" % (bw1 / 38.4 * 100))
