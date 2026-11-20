# -*- coding: utf-8 -*-
"""build_med2.py - 第 21 篇第二轮数据汇总

为什么要重做：
  第一版把同一档连跑 5~9 次取 min，分母（单线程）恰好撞上宿主忙，
  加速比被算成 6.7×，而机器只有 4 个物理核。
  这一版改成"每轮把所有档各跑一次，连跑 7 轮"，并加入两个新指标：
    CPUS      只用 CPU 时间，剔除 vCPU 被宿主挂起的时间
    WALL/CPU  墙钟÷CPU 时间，>1.1 说明这一轮被挂起过
    PARALLEL  CPU 时间÷墙钟，直接读出"实际有几个核在干活"
"""
import json, re, os, statistics as st
HERE = os.path.dirname(os.path.abspath(__file__))
J = lambda x: os.path.join(HERE, x)

OLD = json.load(open(J("med_perf.json"), encoding="utf-8"))


def parse(path):
    txt = open(path, encoding="utf-8", errors="replace").read()
    loads, threads = {}, {}
    for b in re.split(r"RES\|", txt)[1:]:
        L = [x.strip() for x in b.strip().split("\n") if x.strip()]
        if not L:
            continue
        head = L[0].split("|")
        kv = dict(re.findall(r"^([A-Z_]+)=(\S+)", "\n".join(L[1:]), re.M))
        if head[0] == "load":
            loads.setdefault(head[1], []).append(kv)
        elif head[0] == "thread":
            threads.setdefault(int(head[1]), []).append(kv)
    return loads, threads


loads, threads = parse(J("results21c/rr.txt"))

out = {
    "pmu": {"hw": False, "note": "cycles/instructions <not supported>，vpmu.enable 无效"},
    "method": {
        "rounds": 7,
        "order": "每轮依次跑 dep, par4, 1/2/3/4/6/8 线程",
        "clock": "CLOCK_THREAD_CPUTIME_ID / CLOCK_PROCESS_CPUTIME_ID",
        "desktop": "isolated (multi-user.target)",
    },
    "loads": {},
}

for m, rs in loads.items():
    cyc = [float(x["CYCLES"]) / float(x["ITERS"]) for x in rs]
    cpu = [float(x["CPUS"]) / float(x["ITERS"]) for x in rs]
    wc = [float(x["WALL_CPU"]) for x in rs]
    out["loads"][m] = {
        "cyc_min": min(cyc), "cyc_med": st.median(cyc), "cyc_max": max(cyc),
        "cpu_min": min(cpu), "cpu_med": st.median(cpu),
        "wall_cpu_min": min(wc), "wall_cpu_max": max(wc),
        "vals": [round(v, 4) for v in cyc],
        # 单次 FMA 的代价：par4 一次迭代做 4 个独立 FMA
        "cyc_per_fma_min": min(cyc) / (4.0 if m == "par4" else 1.0),
        "cyc_per_fma_med": st.median(cyc) / (4.0 if m == "par4" else 1.0),
    }

# 每次 FMA 代价之比：逐轮算，看比值稳不稳
ratios = []
for a, b in zip(loads["dep"], loads["par4"]):
    d = float(a["CYCLES"]) / float(a["ITERS"])
    p = float(b["CYCLES"]) / float(b["ITERS"]) / 4.0
    if p > 0:
        ratios.append(d / p)
out["loads"]["fma_ratio"] = {
    "min": min(ratios), "med": st.median(ratios), "max": max(ratios),
    "vals": [round(x, 2) for x in ratios],
}

# 保留原文件里没变的部分
for k in ("branch", "wss", "ftrace", "hot", "cpu_util"):
    if k in OLD:
        out[k] = OLD[k]

# 顺序读带宽：旧值把"一遍的耗时"当成"三遍的总量"来算，虚高 3 倍。
# 下面是修正公式后重测 5 轮、每档取最快一轮的结果。
SEQ_FIX = {16384: 24.02, 65536: 24.58, 262144: 24.64, 1048576: 24.34,
           4194304: 20.33, 16777216: 15.96, 67108864: 16.98, 268435456: 15.56}
for x in out["wss"]["seq"]:
    if x["bytes"] in SEQ_FIX:
        x["gbps_old"] = x["gbps"]
        x["gbps"] = SEQ_FIX[x["bytes"]]
out["loads"]["chase"] = OLD["loads"]["chase"]

th = []
for t in sorted(threads):
    rs = threads[t]
    ns = [float(x["NS"]) for x in rs]
    par = [float(x["PARALLEL"]) for x in rs]
    thr = []
    for x in rs:
        thr.append({"ms": float(x["NS"]) / 1e6,
                    "cpus": float(x["CPUS"]) / 1e6,
                    "parallel": float(x["PARALLEL"]),
                    "sw": int(x.get("CTXT_SWITCHES", 0))})
    th.append({
        "threads": t,
        "ms_min": min(ns) / 1e6, "ms_med": st.median(ns) / 1e6, "ms_max": max(ns) / 1e6,
        "cpus_med": st.median([float(x["CPUS"]) for x in rs]) / 1e6,
        "para_max": max(par), "para_med": st.median(par),
        "samples": thr,
    })
out["threads"] = th

# GEMV：5 轮实测
GEMV_RUNS = [159.40, 145.22, 149.73, 143.55, 154.38]
g = dict(OLD["gemv"])
g.update({
    "ms_runs": GEMV_RUNS,
    "MS_PER_TOKEN": st.median(GEMV_RUNS),
    "ms_min": min(GEMV_RUNS), "ms_max": max(GEMV_RUNS),
})
out["gemv"] = g

json.dump(out, open(J("med_perf2.json"), "w", encoding="utf-8"),
          ensure_ascii=False, indent=1)

# 打印核对
print("=== dep / par4 周期每迭代（7 轮）===")
for m in ("dep", "par4"):
    d = out["loads"][m]
    print("%-5s vals=%s" % (m, d["vals"]))
    print("      min=%.2f med=%.2f max=%.2f  单FMA min=%.2f med=%.2f  WALL/CPU %.2f~%.2f"
          % (d["cyc_min"], d["cyc_med"], d["cyc_max"],
             d["cyc_per_fma_min"], d["cyc_per_fma_med"],
             d["wall_cpu_min"], d["wall_cpu_max"]))
r = out["loads"]["fma_ratio"]
print("逐轮比值 %s → min=%.2f med=%.2f max=%.2f" % (r["vals"], r["min"], r["med"], r["max"]))

print("\n=== 线程扩展 ===")
base = out["threads"][0]["ms_med"]
for x in out["threads"]:
    print("t=%-2d 中位 %6.0f ms  (%5.0f~%5.0f)  PARALLEL med=%.1f max=%.1f  吞吐 %5.2f×"
          % (x["threads"], x["ms_med"], x["ms_min"], x["ms_max"],
             x["para_med"], x["para_max"], base / x["ms_med"]))
