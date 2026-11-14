#!/usr/bin/env python3
"""bench_parse.py —— 把 llama-bench 的 JSON 结果翻译成统一的 RES 行

坑：新版 llama-bench 的 JSON 里已经没有 "test"/"tps" 字段了。
    测试名要自己用 n_prompt / n_gen / n_depth 拼，吞吐叫 avg_ts，抖动叫 stddev_ts。
    老版脚本按 "tps" 取，取到 None，直接 TypeError —— 这就是第一版跑挂的原因。

用法:
    python3 bench_parse.py scan  <json> <tag>
    python3 bench_parse.py quant <json> <tag> <size_mb> <ppl> <ppl_err>
"""
import json
import sys


def test_name(r):
    p = r.get("n_prompt", 0) or 0
    n = r.get("n_gen", 0) or 0
    d = r.get("n_depth", 0) or 0
    if d:
        return "tg%d_d%d" % (n, d)
    if p and n:
        return "pp%d+tg%d" % (p, n)
    if p:
        return "pp%d" % p
    return "tg%d" % n


def load(path):
    with open(path) as f:
        return json.load(f)


def cmd_scan(path, tag):
    for r in load(path):
        tps = r.get("avg_ts")
        if tps is None:                     # 万一遇到更老的/更新的格式
            tps = r.get("tps", 0.0) or 0.0
        sd = r.get("stddev_ts")
        if sd is None:
            sd = r.get("stddev", 0.0) or 0.0
        print("RES|%s|%s|th=%s|p=%s|n=%s|d=%s|tps=%.2f|sd=%.2f"
              % (tag, test_name(r), r.get("n_threads"),
                 r.get("n_prompt", 0), r.get("n_gen", 0), r.get("n_depth", 0),
                 tps, sd))


def cmd_quant(path, tag, size_mb, ppl, ppl_err):
    d = {}
    for r in load(path):
        d[test_name(r)] = r
    pp = (d.get("pp512") or {}).get("avg_ts", 0.0) or 0.0
    tg = (d.get("tg128") or {}).get("avg_ts", 0.0) or 0.0
    print("RES|quant|%s|size_mb=%.1f|pp512=%.2f|tg128=%.2f|ppl=%s|ppl_err=%s"
          % (tag, float(size_mb), pp, tg, ppl, ppl_err))


def main():
    mode = sys.argv[1]
    if mode == "scan":
        cmd_scan(sys.argv[2], sys.argv[3])
    elif mode == "quant":
        cmd_quant(*sys.argv[2:7])
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
