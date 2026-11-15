#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 results/ 里的原始 RES| 行算出文章用到的每一个数。

口径（和文章里写的一致）：
  · 01~07 —— 五个整轮（results/s1..s5.txt）里同一档取中位数
  · 08 gemm —— 单独用 -O2 复跑三个整轮（results/gemm_o2.txt）取中位数
  · 09 线程 —— 五个整轮 + results/threads_5rounds.txt，共十轮取中位数

用法：
    python3 med.py          # 打印成表
    python3 med.py --json   # 输出 JSON
"""
import glob
import json
import os
import statistics as st
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "results")

ROUNDS = sorted(glob.glob(os.path.join(RES, "s[1-5].txt")))
GEMM_O2 = os.path.join(RES, "gemm_o2.txt")
THREADS = os.path.join(RES, "threads_5rounds.txt")


NKEY = {"workset": 2, "stride": 1, "locality": 1, "conflict": 1, "chase": 1,
        "prefetch": 1, "layout": 1, "gemm": 2, "false_share": 1, "agg_bw": 1}


def collect(files, prefix):
    """RES|<prefix>|<key...>|<v1>|<v2>...  ->  {key: [(v1,v2,...), ...]}"""
    out = defaultdict(list)
    n = NKEY[prefix]
    for f in files:
        if not os.path.exists(f):
            continue
        for ln in open(f, encoding="utf-8", errors="ignore"):
            if not ln.startswith("RES|" + prefix + "|"):
                continue
            p = ln.strip().split("|")[2:]
            out[tuple(p[:n])].append(tuple(float(x) for x in p[n:]))
    return out


def medcols(d):
    return {k: [round(st.median(c), 4) for c in zip(*v)] for k, v in d.items()}


def collect_fs(files):
    return collect(files, "false_share")


data = {}
for name in ("workset", "stride", "locality", "conflict", "chase", "prefetch", "layout"):
    data[name] = medcols(collect(ROUNDS, name))
data["gemm"] = medcols(collect([GEMM_O2], "gemm"))
data["false_share"] = medcols(collect(ROUNDS + [THREADS], "false_share"))
data["agg_bw"] = medcols(collect(ROUNDS + [THREADS], "agg_bw"))

if "--json" in sys.argv:
    print(json.dumps({k: {"|".join(kk): vv for kk, vv in v.items()} for k, v in data.items()},
                     ensure_ascii=False, indent=1))
    sys.exit()

print(f"轮次：{len(ROUNDS)} 个整轮 + gemm 3 轮(-O2) + 线程 {len(ROUNDS)}+5 轮\n")

print("① 工作集台阶（顺序求和，256 MB 总量）")
print("     工作集     重复    ms      GB/s    ns/元素")
for k, v in sorted(data["workset"].items(), key=lambda kv: (int(kv[0][0]), -int(kv[0][1]))):
    kb = int(k[0]) // 1024
    lab = f"{kb} KB" if int(k[0]) < (1 << 20) else f"{int(k[0]) >> 20} MB"
    print(f"   {lab:>9} {k[1]:>8} {v[0]:>8.2f} {v[1]:>8.2f} {v[2]:>8.4f}")

print("\n② 步长（800 万次访问，64 MB 数组）")
for k, v in sorted(data["stride"].items(), key=lambda kv: int(kv[0][0])):
    print(f"   {int(k[0]):>5} B  {v[0]:>7.3f} ns")

print("\n③ 局部性（ns / 次）")
for k, v in data["locality"].items():
    print(f"   {k[0]:<14} {v[0]:>7.3f}")

print("\n④ 冲突缺失（256 KB 足迹）")
for k, v in sorted(data["conflict"].items(), key=lambda kv: int(kv[0][0])):
    print(f"   {int(k[0]):>5} B  {v[0]:>7.3f} ns")

print("\n⑤ 指针追逐")
for k, v in sorted(data["chase"].items(), key=lambda kv: int(kv[0][0])):
    print(f"   {int(k[0]):>4} MB  随机 {v[0]:>7.2f}  顺序 {v[1]:>6.3f}  {v[0] / v[1]:>6.1f}×")

print("\n⑥ 预取（256 MB，质数步长）")
for k, v in sorted(data["prefetch"].items(), key=lambda kv: int(kv[0][0])):
    print(f"   提前 {int(k[0]):>2} 步  {v[0]:>7.3f} ns")

print("\n⑦ AoS / SoA（800 万条记录，只读 key）")
for k, v in data["layout"].items():
    print(f"   {k[0]:<4} {v[0] / 1e6:>6.1f} MB  {v[1]:>7.2f} ms  {v[2]:>6.2f} GB/s")

print("\n⑧ GEMM（1024³ fp32，-O2）")
for k, v in sorted(data["gemm"].items(), key=lambda kv: (kv[0][0] != "naive", int(kv[0][1]))):
    print(f"   {k[0]:<8} {k[1]:>4}  {v[0]:>9.2f} ms  {v[1]:>5.2f} GFLOPS")

print("\n⑨ 伪共享（ms / 每线程）")
for k, v in sorted(data["false_share"].items(), key=lambda kv: int(kv[0][0])):
    print(f"   {k[0]} 线程  相邻 {v[0]:>6.2f}  隔开 {v[1]:>6.2f}  {v[0] / v[1]:>5.2f}×")

print("\n⑩ 聚合带宽（每线程 128 MB 私有数组）")
for k, v in sorted(data["agg_bw"].items(), key=lambda kv: int(kv[0][0])):
    print(f"   {k[0]} 线程  {v[0]:>6.2f} GB/s")
