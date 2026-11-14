#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 21_model_knobs/results 下的 perf 与 llama-bench 输出拼成一张表。

单次 CPU = (task-clock(r=4) − task-clock(r=1)) ÷ 3，单位 ms。
除以 token 数得到每 token CPU；与 llama-bench 自报的 t/s 相除即"占几颗核"。
"""
import glob
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "results")

PERF = ("task-clock", "context-switches", "cpu-migrations", "page-faults")


def read_perf(path):
    out = {}
    if not os.path.exists(path):
        return out
    for line in open(path, encoding="utf-8", errors="replace"):
        parts = line.strip().split(",")
        if len(parts) < 3:
            continue
        val, _, name = parts[0], parts[1], parts[2]
        if name in PERF:
            try:
                out[name] = float(val)
            except ValueError:
                pass
    return out


def read_bench(path):
    """返回 (测试名, t/s)。llama-bench -o md 每个用例一行。"""
    if not os.path.exists(path):
        return None, None
    for line in open(path, encoding="utf-8", errors="replace"):
        if not line.startswith("|"):
            continue
        cols = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cols) < 7 or cols[0] in ("model", "---") or set(cols[0]) <= set("- "):
            continue
        m = re.search(r"([\d.]+)", cols[-1])
        if m:
            return cols[-2], float(m.group(1))
    return None, None


def main():
    tags = sorted({os.path.basename(p)[3:-7] for p in glob.glob(os.path.join(RES, "st_*_r4.txt"))})
    rows = []
    for t in tags:
        p4 = read_perf(os.path.join(RES, f"st_{t}_r4.txt"))
        p1 = read_perf(os.path.join(RES, f"st_{t}_r1.txt"))
        if "task-clock" not in p4 or "task-clock" not in p1:
            continue
        one = (p4["task-clock"] - p1["task-clock"]) / 3.0     # ms
        cs = (p4.get("context-switches", 0) - p1.get("context-switches", 0)) / 3.0
        pf = (p4.get("page-faults", 0) - p1.get("page-faults", 0)) / 3.0
        tname, tps = read_bench(os.path.join(RES, f"bench_{t}_r4.txt"))
        n_tok = 128 if (tname or "").startswith("tg") else 512
        cpu_per_tok = one / n_tok
        wall_per_tok = 1000.0 / tps if tps else float("nan")
        cores = cpu_per_tok / wall_per_tok if tps else float("nan")
        rows.append((t, tname, one, cpu_per_tok, tps, cores, cs, pf))

    hdr = f"{'用例':<14}{'测试':<8}{'单次CPU':>9}{'CPU/token':>11}{'t/s':>8}{'占核':>7}{'切换/token':>11}{'缺页/token':>11}"
    print(hdr)
    print("-" * len(hdr))
    for t, tname, one, ct, tps, cores, cs, pf in rows:
        print(f"{t:<14}{str(tname):<8}{one:>9.1f}{ct:>11.2f}{tps:>8.1f}{cores:>7.2f}{cs:>11.2f}{pf:>11.1f}")

    with open(os.path.join(HERE, "knobs_table.txt"), "w", encoding="utf-8") as f:
        f.write(hdr + "\n")
        for t, tname, one, ct, tps, cores, cs, pf in rows:
            f.write(f"{t:<14}{str(tname):<8}{one:>9.1f}{ct:>11.2f}{tps:>8.1f}{cores:>7.2f}{cs:>11.2f}{pf:>11.1f}\n")


if __name__ == "__main__":
    main()
