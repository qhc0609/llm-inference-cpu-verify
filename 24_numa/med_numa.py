#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""med_numa.py - turn the raw RES| lines of run_numa.sh into med_numa.json.

    python3 med_numa.py [results_dir] [out.json]

Only medians of what the logs actually printed; no number is invented here.
"""
import glob
import json
import os
import statistics
import sys


def lines(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return [l.strip() for l in f if l.startswith("RES|")]
    except OSError:
        return []


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else "results_numa"
    dst = sys.argv[2] if len(sys.argv) > 2 else "med_numa.json"

    data = {"topo": {"nodes": 0, "node": {}}, "lat": {}, "bw": {},
            "ft": {}, "ft_inter": {}, "mig": {}, "share": {}, "dec": {}, "vmstat": {}}

    # ---- topology
    for l in lines(os.path.join(src, "topo.txt")):
        p = l.split("|")
        if p[1] == "nodes":
            data["topo"]["nodes"] = int(p[2])
        elif p[1] == "node":
            n = p[2]
            d = data["topo"]["node"].setdefault(n, {})
            d[p[3]] = p[4]
        elif p[1] == "page_kb":
            data["topo"]["page_kb"] = int(p[2])
        elif p[1] == "thp":
            data["topo"]["thp"] = p[2]

    # ---- latency + bandwidth (comb lines carry the label for the block that follows)
    comb = None
    for f in sorted(glob.glob(os.path.join(src, "vm_*.txt"))):
        for l in lines(f):
            p = l.split("|")
            if p[1] == "lat":
                data["lat"][p[2]] = {"mb": int(p[3]), "ns": float(p[4]),
                                     "gbps": float(p[5]), "pages": [int(p[6]), int(p[7])],
                                     "huge_kb": int(p[8]) if len(p) > 8 else -1}
            elif p[1] == "vmstat":
                data["vmstat"][p[2]] = {p[3]: int(p[4]), p[5]: int(p[6]),
                                        p[7]: int(p[8]), p[9]: int(p[10]),
                                        p[11]: int(p[12]), p[13]: int(p[14])}
            elif p[1] == "dec":
                data["dec"][os.path.basename(f)[3:-4]] = {
                    "nt": int(p[2]), "ms": float(p[3]), "tok_s": float(p[4]),
                    "gflops": float(p[5]), "gbps": float(p[6]),
                    "ms_med": float(p[7]), "ms_max": float(p[8]),
                    "samples": int(p[9]) if len(p) > 9 else 0}
            elif p[1] == "mig":
                data["mig"][os.path.basename(f)[3:-4]] = {
                    "mb": int(p[2]), "to": int(p[4]), "ms": float(p[6]),
                                  "gbps": float(p[8]), "before": [int(p[10]), int(p[11])],
                                  "after": [int(p[13]), int(p[14])],
                                  "huge_kb": int(p[16]) if len(p) > 16 else -1}

    # bandwidth blocks: name them by the comb line that precedes them
    for f in sorted(glob.glob(os.path.join(src, "vm_bw_*.txt"))):
        tag = os.path.basename(f)[3:-4]           # vm_bw_m0c0.txt -> bw_m0c0
        rows = {}
        for l in lines(f):
            p = l.split("|")
            if p[1] == "bw":
                rows["read" + p[3] if len(p) == 5 else p[2]] = float(p[-1])
        if rows:
            data["bw"][tag] = rows

    # ---- first touch
    for name, path in (("ft", "firsttouch.txt"), ("ft_inter", "firsttouch_inter.txt")):
        g, pg = {}, {}
        for l in lines(os.path.join(src, path)):
            p = l.split("|")
            if p[2] == "gbps":
                g.setdefault(p[3], []).append(float(p[4]))
            elif p[2] == "pages":
                pg[p[3]] = [int(p[4]), int(p[5])]
        data[name] = {k: {"gbps": statistics.median(v), "pages": pg.get(k)}
                      for k, v in g.items() if v}

    # ---- false sharing
    for name, path in (("share", ["share_same.txt", "share_cross.txt"])):
        for f in path:
            for l in lines(os.path.join(src, f)):
                p = l.split("|")
                data["share"][p[2]] = float(p[3])

    # ---- derive the numbers the article argues with
    try:
        a = data["lat"]["lat_m0c0"]["ns"]
        b = data["lat"]["lat_m1c0"]["ns"]
        data["derived"] = {
            "lat_local": a, "lat_remote": b,
            "lat_ratio": b / a,
            "lat_delta_pct": (b / a - 1) * 100,
        }
        s = data["share"]
        data["derived"]["share_same"] = s.get("packed_same_node", 0)
        data["derived"]["share_cross"] = s.get("packed_cross_node", 0)
        data["derived"]["share_pad_same"] = s.get("padded_same_node", 0)
        data["derived"]["pack_penalty"] = (s["packed_same_node"] /
                                           s["padded_same_node"]) if s.get("padded_same_node") else 0
        d = data["dec"]
        if "dec_2local" in d and "dec_2split" in d:
            data["derived"]["dec_split_ratio"] = d["dec_2split"]["tok_s"] / d["dec_2local"]["tok_s"]
        if "dec_4local" in d and "dec_2local" in d:
            data["derived"]["dec_thr"] = d["dec_4local"]["tok_s"] / d["dec_2local"]["tok_s"]
    except (KeyError, ZeroDivisionError):
        pass

    with open(dst, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=1)
    print("wrote", dst, "keys:", list(data))


if __name__ == "__main__":
    main()
