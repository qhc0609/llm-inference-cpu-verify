#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""build_med_numa.py - results_numa + results_ab  ->  med_numa.json

两条规矩，写进 JSON 里一起交代：

1. 单点事实（拓扑、页面落点、迁移、伪共享、延迟矩阵）取最后一轮扫描
   results_numa/ 的原始行，不做二次挑选。
2. 「本地 vs 远端」这类对照结论取 results_ab/ 的交替五轮，报中位数和区间。
   绝不在两次跑之间比大小——宿主机负载一轮一个样，跨轮比较就是自欺。

所以 JSON 里每个条目都带 src 和 stat 两个字段，谁都能顺着查回去。
"""
import glob
import json
import os
import statistics as st


def res(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return [l.strip() for l in f if l.startswith("RES|")]
    except OSError:
        return []


def tag_of(path):
    return os.path.basename(path)[3:-4]          # vm_dec_2local.txt -> dec_2local


def main():
    SRC = "results_numa"
    AB = "results_ab"
    d = {"src": "results_numa (一轮扫描) + results_ab (交替五轮)",
         "topo": {}, "lat": {}, "bw": {}, "ft": {}, "mig": {}, "share": {},
         "dec": {}, "ab": {}}

    # ---------------- 拓扑：直接抄内核的话
    for l in res(os.path.join(SRC, "topo.txt")):
        p = l.split("|")
        if p[1] == "nodes":
            d["topo"]["nodes"] = int(p[2])
        elif p[1] == "node":
            n = d["topo"].setdefault("node" + p[2], {})
            n[p[3]] = p[4]
        elif p[1] in ("page_kb", "thp"):
            d["topo"][p[1]] = p[2]

    # ---------------- 延迟矩阵
    for f in sorted(glob.glob(os.path.join(SRC, "vm_lat_*.txt"))):
        for l in res(f):
            p = l.split("|")
            if p[1] == "lat":
                d["lat"][p[2]] = {"mb": int(p[3]), "ns": float(p[4]),
                                  "gbps": float(p[5]),
                                  "pages": [int(p[6]), int(p[7])],
                                  "huge_kb": int(p[8])}

    # ---------------- 带宽矩阵（1/2 线程，各节点组合）
    for f in sorted(glob.glob(os.path.join(SRC, "vm_bw_*.txt"))):
        t = tag_of(f)
        row = {}
        for l in res(f):
            p = l.split("|")
            if p[1] == "bw" and len(p) == 5:
                row["read" + p[3]] = float(p[4])
            elif p[1] == "bw" and len(p) == 6:
                row[p[2]] = float(p[5])
        if row:
            d["bw"][t] = row

    # ---------------- 首次触碰：只认落点，带宽那一列受宿主抢占影响太大
    for name, fn in (("default", "firsttouch.txt"), ("interleave", "firsttouch_inter.txt")):
        g, pg = {}, {}
        for l in res(os.path.join(SRC, fn)):
            p = l.split("|")
            if p[2] == "gbps":
                g.setdefault(p[3], []).append(float(p[4]))
            elif p[2] == "pages":
                pg[p[3]] = [int(p[4]), int(p[5])]
        d["ft"][name] = {k: {"gbps": st.median(v) if v else 0,
                            "pages": pg.get(k)} for k, v in g.items()}

    # ---------------- 迁移与伪共享
    for f in sorted(glob.glob(os.path.join(SRC, "vm_mig*.txt"))):
        for l in res(f):
            p = l.split("|")
            if p[1] == "mig":
                d["mig"][tag_of(f)] = {"mb": int(p[2]), "to": int(p[4]),
                                       "ms": float(p[6]), "gbps": float(p[8]),
                                       "before": [int(p[10]), int(p[11])],
                                       "after": [int(p[13]), int(p[14])],
                                       "huge_kb": int(p[16])}
    for f in sorted(glob.glob(os.path.join(SRC, "share_*.txt"))):
        for l in res(f):
            p = l.split("|")
            if p[1] == "share":
                d["share"][p[2]] = float(p[3])

    # ---------------- decode 六种摆法
    for f in sorted(glob.glob(os.path.join(SRC, "vm_dec_*.txt"))):
        for l in res(f):
            p = l.split("|")
            if p[1] == "dec":
                d["dec"][tag_of(f)] = {"nt": int(p[2]), "ms": float(p[3]),
                                       "tok_s": float(p[4]), "gflops": float(p[5]),
                                       "gbps": float(p[6]), "ms_med": float(p[7]),
                                       "ms_max": float(p[8]), "n": int(p[9])}

    # ---------------- 交替五轮：本地 vs 远端
    vals = {}
    for l in res(os.path.join(AB, "ab.txt")):
        p = l.split("|")
        if p[1] != "ab":
            continue
        test, tag = p[2], p[3]
        if test == "lat" and len(p) >= 8:
            vals.setdefault(("lat", tag.split("-")[-1]), []).append(float(p[6]))
        elif test == "dec" and len(p) >= 7:
            vals.setdefault(("dec", tag.split("-")[-1]), []).append(float(p[5]))  # nt,ms -> ms
        elif test == "bw2" and len(p) >= 6 and p[4] == "read" and p[5] == "2":
            vals.setdefault(("bw2", tag.split("-")[-1]), []).append(float(p[6]))

    def pack(v):
        return {"median": st.median(v), "min": min(v), "max": max(v),
                "n": len(v), "vals": [round(x, 2) for x in v]}

    for (test, cfg), v in vals.items():
        d["ab"].setdefault(test, {})[cfg] = pack(v)

    # ---------------- 派生：文章真正要用的那几个比值
    dv = {}
    try:
        dv["lat_local"] = d["ab"]["lat"]["m0"]["median"]
        dv["lat_remote"] = d["ab"]["lat"]["m1"]["median"]
        dv["lat_gap"] = dv["lat_remote"] / dv["lat_local"] - 1
        dv["dec_local"] = d["ab"]["dec"]["m0"]["median"]
        dv["dec_remote"] = d["ab"]["dec"]["m1"]["median"]
        dv["dec_gap"] = dv["dec_remote"] / dv["dec_local"] - 1
        dv["dec_split"] = d["ab"]["dec"]["split"]["median"]
        dv["dec_split_gap"] = dv["dec_split"] / d["ab"]["dec"]["local"]["median"] - 1
    except KeyError:
        pass

    # ---------------- 整机 STREAM 五轮（1~4 线程各一次），取中位数
    five = {}
    for l in res(os.path.join("results_bw", "bw_4cpu.txt")):
        p = l.split("|")
        if p[1] == "bw":
            key = "read" + p[3] if len(p) == 5 else p[2]
            five.setdefault(key, []).append(float(p[-1]))
    if five:
        d["bw5"] = {k: {"med": st.median(v), "max": max(v), "min": min(v), "n": len(v)}
                    for k, v in five.items()}

    b5 = d.get("bw5", {})
    b4 = d["bw"].get("bw_4cpu_m0", {})
    dv["bw1"] = b5["read1"]["med"] if "read1" in b5 else max(v.get("read1", 0) for v in d["bw"].values())
    dv["bw2"] = b5["read2"]["med"] if "read2" in b5 else max(v.get("read2", 0) for v in d["bw"].values())
    dv["bw3"] = b5["read3"]["med"] if "read3" in b5 else b4.get("read3", 0)
    dv["bw4"] = b5["read4"]["med"] if "read4" in b5 else b4.get("read4", 0)
    dv["bw_plateau"] = max(dv["bw2"], dv["bw3"], dv["bw4"])

    # 权重流 = 256 MiB / 每 token 毫秒
    WEIGHT_MIB = 16384 * 4096 * 4 / 1048576.0
    dv["weight_mib"] = WEIGHT_MIB
    dv["dec_gbps"] = d["ab"]["dec"]["m0"]["median"] and \
        WEIGHT_MIB * 1048576 / (d["ab"]["dec"]["m0"]["median"] / 1000.0) / 1e9
    dv["dec_vs_stream"] = dv["dec_gbps"] / dv["bw2"] if dv["bw2"] else 0

    dv["share_pack_same"] = d["share"]["packed_same_node"] / d["share"]["padded_same_node"]
    dv["share_pack_cross"] = d["share"]["packed_cross_node"] / d["share"]["padded_cross_node"]
    d["derived"] = dv

    with open("med_numa.json", "w", encoding="utf-8") as f:
        json.dump(d, f, ensure_ascii=False, indent=1)
    print("wrote med_numa.json")
    for k, v in dv.items():
        print("  %-16s %s" % (k, ("%.3f" % v) if isinstance(v, float) else v))


if __name__ == "__main__":
    main()
