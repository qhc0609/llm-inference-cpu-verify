# -*- coding: utf-8 -*-
"""build_med22.py - 把 results22/*.csv 汇总成 med_perf22.json，正文只读这个文件。"""
import csv, json, os, re, statistics as st

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "results22")


def rows(name):
    out = []
    p = os.path.join(RES, name)
    if not os.path.exists(p):
        print("MISSING", name)
        return out
    for ln in open(p, encoding="utf-8"):
        f = ln.strip().split("|")
        if not f or f[0] != "R" or len(f) < 11:
            continue
        out.append(dict(k=f[1], M=int(f[2]), N=int(f[3]), K=int(f[4]), T=int(f[5]),
                        ms=float(f[7]), gf=float(f[8]), gb=float(f[9]), err=float(f[10])))
    return out


def stat(vals):
    return dict(ms_min=round(min(vals), 3), ms_med=round(st.median(vals), 3),
                ms_max=round(max(vals), 3), n=len(vals),
                spread=round(max(vals) / min(vals), 2))


def group(rs, keyf):
    d = {}
    for r in rs:
        d.setdefault(keyf(r), []).append(r["ms"])
    return d


out = {}

# ---- 环境
env = {}
for ln in open(os.path.join(RES, "env.txt"), encoding="utf-8"):
    ln = ln.strip()
    if "model name" in ln:
        env["cpu"] = ln.split(":", 1)[1].strip()
    elif ln.startswith("L3"):
        env["l3"] = ln
    elif ln.startswith("L2"):
        env["l2"] = ln
    elif ln.startswith("peak="):
        env["peak"] = float(ln.split("|")[1])
    elif "gcc" in ln:
        env["gcc"] = ln
out["env"] = env

# ---- 512^3 阶梯
lad = rows("ladder.csv")
g = group(lad, lambda r: r["k"])
out["ladder"] = {}
for k, v in g.items():
    s = stat(v)
    s["gf"] = round(2 * 512 ** 3 / 1e9 / (s["ms_min"] / 1000), 2)
    s["err"] = max(r["err"] for r in lad if r["k"] == k)
    out["ladder"][k] = s

# ---- 关闭自动向量化
nv = rows("novec.csv")
out["novec"] = {}
for k, v in group(nv, lambda r: r["k"]).items():
    s = stat(v)
    s["gf"] = round(2 * 512 ** 3 / 1e9 / (s["ms_min"] / 1000), 2)
    out["novec"][k] = s

# ---- 1024^3 阶梯
l2 = rows("ladder1024.csv")
out["ladder1024"] = {}
for k, v in group(l2, lambda r: r["k"]).items():
    s = stat(v)
    s["gf"] = round(2 * 1024 ** 3 / 1e9 / (s["ms_min"] / 1000), 2)
    out["ladder1024"][k] = s

# ---- 调参后（MC=128 KC=64 NC=256）
tn = rows("tuned.csv")
out["tuned"] = {}
for r in tn:
    key = "%s_%d_%d" % (r["k"], r["M"], r["T"])
    d = out["tuned"].setdefault(key, {"k": r["k"], "M": r["M"], "T": r["T"], "vals": []})
    d["vals"].append(r["ms"])
for k, d in out["tuned"].items():
    s = stat(d["vals"])
    s["gf"] = round(2 * d["M"] ** 3 / 1e9 / (s["ms_min"] / 1000), 2)
    d.update(s)
    del d["vals"]

# ---- 大矩阵：t4 vs block
bg = rows("big.csv")
out["big"] = {}
for (k, m), v in group(bg, lambda r: (r["k"], r["M"])).items():
    s = stat(v)
    s["gf"] = round(2 * m ** 3 / 1e9 / (s["ms_min"] / 1000), 2)
    out["big"]["%s_%d" % (k, m)] = s

# ---- 分块扫描
sweep = []
for ln in open(os.path.join(RES, "block.csv"), encoding="utf-8"):
    f = ln.strip().split("|")
    if f and f[0] == "B":
        sweep.append(dict(mc=int(f[1]), kc=int(f[2]), nc=int(f[3]),
                          ms=float(f[4]), gf=float(f[5]), err=float(f[6])))
sweep.sort(key=lambda x: -x["gf"])
out["sweep"] = sweep

# ---- 多线程
th = rows("thr.csv")
out["thr"] = {}
for t, v in group(th, lambda r: r["T"]).items():
    s = stat(v)
    s["gf"] = round(2 * 1024 ** 3 / 1e9 / (s["ms_min"] / 1000), 2)
    out["thr"][str(t)] = s

# ---- 屋顶线
rf = rows("roof.csv")
out["roof"] = []
for m, v in group(rf, lambda r: r["M"]).items():
    s = stat(v)
    flops = 2.0 * m * 896 * 896
    byts = 4.0 * (m * 896 + 896 * 896 + m * 896)
    out["roof"].append(dict(M=m, ai=round(flops / byts, 2),
                            gf=round(flops / 1e9 / (s["ms_min"] / 1000), 2),
                            gbs=round(byts / 1e9 / (s["ms_min"] / 1000), 2),
                            ms=round(s["ms_min"], 3)))
out["roof"].sort(key=lambda x: x["M"])

# ---- 融合
fus = []
for name in ("fusion.csv", "fusion_big.csv"):
    p = os.path.join(RES, name)
    if not os.path.exists(p):
        continue
    for ln in open(p, encoding="utf-8"):
        f = ln.strip().split("|")
        if f and f[0] == "F":
            fus.append(dict(M=int(f[1]), N=int(f[2]), K=int(f[3]), sep=float(f[4]),
                            fused=float(f[5]), gf_sep=float(f[6]), gf_fused=float(f[7]),
                            err=float(f[8])))
for d in fus:
    d["gain"] = round(d["sep"] / d["fused"], 3)
    nb = d["M"] * d["N"]
    d["save_mb"] = round(2.0 * nb * 4 / 1048576, 1)   # 省掉两趟读+写
out["fusion"] = sorted(fus, key=lambda x: (x["M"], x["K"]))

# ---- int8
q = []
p = os.path.join(RES, "int8.csv")
for ln in open(p, encoding="utf-8"):
    f = ln.strip().split("|")
    if f and f[0] == "Q":
        q.append(dict(M=int(f[1]), N=int(f[2]), K=int(f[3]), ms=float(f[5]),
                      gf=float(f[6]), maxerr=float(f[7]), rel=float(f[8])))
out["int8"] = []
for m, v in group(q, lambda r: r["M"]).items():
    s = stat(v)
    top = [r for r in q if r["M"] == m][0]
    gfmin = 2.0 * m * 896 * 896 / 1e9 / (s["ms_min"] / 1000)
    out["int8"].append(dict(M=m, ms=round(s["ms_min"], 4), gf=round(gfmin, 2),
                            maxerr=top["maxerr"], rel=top["rel"],
                            gbs=round(896 * 896 * 1 / 1e9 / (s["ms_min"] / 1000), 2)))
out["int8"].sort(key=lambda x: x["M"])

json.dump(out, open(os.path.join(HERE, "med_perf22.json"), "w", encoding="utf-8"),
          ensure_ascii=False, indent=1)

print("peak", env.get("peak"))
for k in ("naive", "ikj", "simd1", "t1", "t2", "t4", "t6", "t8", "t4u2", "asm", "block"):
    d = out["ladder"].get(k)
    if d:
        print("  %-7s %8.2f ms  %6.2f GF  x%.2f" % (k, d["ms_min"], d["gf"], 302.5 / d["ms_min"]))
print("novec ikj", out["novec"]["ikj"]["gf"], "vs vec", out["ladder"]["ikj"]["gf"])
print("1024:", {k: v["gf"] for k, v in out["ladder1024"].items()})
print("tuned:", {k: (v["ms_min"], v["gf"]) for k, v in out["tuned"].items()})
print("thr:", {k: v["gf"] for k, v in out["thr"].items()})
print("sweep best", out["sweep"][0], "worst", out["sweep"][-1])
print("fusion", [(d["M"], d["K"], d["gain"]) for d in out["fusion"]])
print("int8", [(d["M"], d["ms"], d["rel"]) for d in out["int8"]])
