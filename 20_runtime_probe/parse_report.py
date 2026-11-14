#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""parse_report.py —— 把 perf report 的 --stdio 输出压成一张热点表。

只看 Self 列：Children 列会把调用者也算进去，同一个函数会被统计两次。
libgomp 在 openEuler 上是 strip 过的，只能看到地址，统一归成"OpenMP 同步"。
符号名里有空格（模板、命名空间），所以按"行内连续 4 个以上空格"切列。
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "results")

ROW = re.compile(r"^\s*([\d.]+)%\s+([\d.]+)%\s+(\S+)\s+\[(.)\]\s?(.*)$")


def parse(path, label):
    agg = {}
    total = 0.0
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = ROW.match(line)
            if not m:
                continue
            self_pct = float(m.group(2))
            dso = m.group(3).strip("[]")
            sym = re.split(r"\s{4,}", m.group(5))[0].strip()
            if not sym or self_pct < 0.05:
                continue
            if "libgomp" in dso:
                sym = "libgomp：GOMP barrier / spin 等待"
            elif "kernel.kallsyms" in dso:
                sym = "[内核] " + sym
            elif dso.startswith("unknown"):
                sym = "[无符号地址] " + sym
            agg[sym] = agg.get(sym, 0.0) + self_pct
            total += self_pct
    top = sorted(agg.items(), key=lambda kv: -kv[1])[:10]
    out = [f"===== {label}（已解析符号合计 {total:.1f}%）====="]
    for sym, pct in top:
        out.append(f"{pct:6.2f}%  {sym[:76]}")
    return out


def main():
    blocks = []
    for tag, label in [("pp4", "prefill 512 token / 4 线程"),
                       ("tg4", "decode 128 token / 4 线程")]:
        p = os.path.join(RES, f"report_{tag}.txt")
        if os.path.exists(p):
            blocks.extend(parse(p, label))
            blocks.append("")
    txt = "\n".join(blocks)
    print(txt)
    with open(os.path.join(RES, "functions.txt"), "w", encoding="utf-8") as f:
        f.write(txt + "\n")


if __name__ == "__main__":
    main()
