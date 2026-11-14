#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""analyze.py —— 差分算出"单次 prefill / 单次 decode"的 CPU 成本。

CPU 时间取自 perf 的 task-clock（内核在切换时精确累加），
wall 取自 llama-bench 自报的 t/s（它已经把加载和预热排除在外）。
两者相除就是这一阶段真实占用了几颗核。

差分：同一用例跑 r 次与跑 1 次相减，加载、初始化、建页表的一次性开销抵消。
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "results")


def stat(tag):
    """读 perf stat 的 -x, 输出：值,单位,事件名,..."""
    p = os.path.join(RES, f"stat_{tag}.txt")
    if not os.path.exists(p):
        return None
    d = {}
    for line in open(p, encoding="utf-8", errors="replace"):
        parts = line.strip().split(",")
        if len(parts) < 3:
            continue
        try:
            v = float(parts[0])
        except ValueError:
            continue
        d[parts[2]] = v
    return d or None


def tps(tag, kind):
    """从 llama-bench 的 markdown 表里取 pp512 / tg128 的 t/s。"""
    p = os.path.join(RES, f"bench_{tag}.txt")
    if not os.path.exists(p):
        return None
    for line in open(p, encoding="utf-8", errors="replace"):
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.split("|")]
        for i, c in enumerate(cells):
            if re.fullmatch(rf"{kind}\d+", c) and i + 1 < len(cells):
                m = re.match(r"([\d.]+)", cells[i + 1])
                if m:
                    return float(m.group(1))
    return None


def diff(hi, lo, reps, tokens, kind):
    a, b = stat(hi), stat(lo)
    if not a or not b:
        return None
    cpu = (a["task-clock"] - b["task-clock"]) / reps
    v = tps(hi, kind)
    wall = 1000.0 / v if v else None
    return {
        "cpu_ms_per_token": cpu / tokens,
        "wall_ms_per_token": wall,
        "cpu_per_run": cpu,
        "util": (cpu / tokens) / wall if wall else None,
        "tps": v,
        "cs_per_run": (a.get("context-switches", 0) - b.get("context-switches", 0)) / reps,
        "pf_per_run": (a.get("page-faults", 0) - b.get("page-faults", 0)) / reps,
    }


def main():
    out = []
    plan = [
        ("prefill 512 / 4 线程", "pp4_r5", "pp4_r1", 4, 512, "pp"),
        ("prefill 512 / 1 线程", "pp1_r3", "pp1_r1", 2, 512, "pp"),
        ("decode 128 / 4 线程", "tg4_r5", "tg4_r1", 4, 128, "tg"),
        ("decode 128 / 1 线程", "tg1_r3", "tg1_r1", 2, 128, "tg"),
    ]
    rows = {}
    for name, hi, lo, reps, tok, kind in plan:
        r = diff(hi, lo, reps, tok, kind)
        if not r:
            out.append(f"{name}: 缺数据")
            continue
        rows[name] = r
        out.append(
            f"{name}\n"
            f"    单次 CPU {r['cpu_per_run']:8.1f} ms   "
            f"每 token CPU {r['cpu_ms_per_token']:6.2f} ms   "
            f"每 token wall {r['wall_ms_per_token']:6.2f} ms   "
            f"占用 {r['util']:.2f} 核   "
            f"{r['tps']:.1f} t/s"
        )

    pp4, pp1 = rows.get("prefill 512 / 4 线程"), rows.get("prefill 512 / 1 线程")
    tg4, tg1 = rows.get("decode 128 / 4 线程"), rows.get("decode 128 / 1 线程")
    out.append("")
    if pp4 and pp1:
        out.append(f"prefill 4 线程 vs 1 线程：wall 加速 {pp1['wall_ms_per_token']/pp4['wall_ms_per_token']:.2f}×，"
                   f"CPU 成本 {pp4['cpu_ms_per_token']/pp1['cpu_ms_per_token']:.2f}×（1 线程 = 1.00）")
    if tg4 and tg1:
        out.append(f"decode  4 线程 vs 1 线程：wall 加速 {tg1['wall_ms_per_token']/tg4['wall_ms_per_token']:.2f}×，"
                   f"CPU 成本 {tg4['cpu_ms_per_token']/tg1['cpu_ms_per_token']:.2f}×（1 线程 = 1.00）")

    # 量化：同一条 decode，只换权重体积
    out.append("")
    sizes = {"q2_k": 395.9, "q4_k_m": 462.96, "q8_0": 644.4}  # MiB，文件大小
    for tag, lo, name in [("tgq2", "tgq2_r1", "q2_k"), ("tgq8", "tgq8_r1", "q8_0")]:
        r = diff(tag + "_r5", lo, 4, 128, "tg")
        if not r:
            continue
        bw = sizes[name] * 1024 * 1024 / (r["wall_ms_per_token"] / 1000) / 1e9
        out.append(f"{name:6s} ({sizes[name]:6.1f} MiB)：每 token wall {r['wall_ms_per_token']:6.2f} ms，"
                   f"CPU {r['cpu_ms_per_token']:6.2f} ms，占 {r['util']:.2f} 核，"
                   f"{r['tps']:5.1f} t/s，等效带宽 {bw:5.1f} GB/s")
    if tg4:
        bw = sizes["q4_k_m"] * 1024 * 1024 / (tg4["wall_ms_per_token"] / 1000) / 1e9
        out.append(f"{'q4_k_m':6s} ({sizes['q4_k_m']:6.1f} MiB)：每 token wall {tg4['wall_ms_per_token']:6.2f} ms，"
                   f"CPU {tg4['cpu_ms_per_token']:6.2f} ms，占 {tg4['util']:.2f} 核，"
                   f"{tg4['tps']:5.1f} t/s，等效带宽 {bw:5.1f} GB/s")

    txt = "\n".join(out)
    print(txt)
    with open(os.path.join(RES, "runtime_summary.txt"), "w", encoding="utf-8") as f:
        f.write(txt + "\n")


if __name__ == "__main__":
    main()
