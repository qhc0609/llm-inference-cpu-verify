# -*- coding: utf-8 -*-
"""build_med_perf.py - 把 results21 里的原始日志汇总成 med_perf.json

解析规则与 run21.sh 的输出严格对应：
  RES|<kind>|<arg>|<round>   之后的 perf stat CSV 与 KEY=VALUE 行属于同一轮
多轮取中位数（时间类取最小值，避免宿主抢占导致的抖动）。
"""
import os, sys, json, re, statistics as st

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "results21")


def read(name):
    p = os.path.join(OUT, name)
    if not os.path.exists(p):
        return ""
    return open(p, encoding="utf-8", errors="replace").read()


def kv(block):
    d = {}
    for line in block.splitlines():
        m = re.match(r"^([A-Z_]+)=(.+)$", line.strip())
        if m:
            d[m.group(1)] = m.group(2)
    return d


def perf_events(block):
    """解析 perf stat -x, 的 CSV：value,unit,event,run_time,pcnt,metric"""
    ev = {}
    for line in block.splitlines():
        parts = line.split(",")
        if len(parts) < 3:
            continue
        name = parts[2].strip()
        if not name or " " in name:
            continue
        v = parts[0].strip()
        try:
            fv = float(v)
        except ValueError:
            continue
        # 同名事件可能有多轮（-r 3），保留列表
        ev.setdefault(name, []).append(fv)
    return {k: st.median(v) for k, v in ev.items() if v}


def split_blocks(text, kind):
    """按 RES|kind| 切块，返回 [(arg, round, body)]"""
    out = []
    cur = None
    for line in text.splitlines():
        m = re.match(r"^RES\|%s\|(.*)$" % kind, line)
        if m:
            if cur:
                out.append(cur)
            cur = [m.group(1), []]
        elif cur is not None:
            cur[1].append(line)
    if cur:
        out.append(cur)
    res = []
    for arg, body in out:
        parts = arg.split("|")
        res.append((parts[0], parts[1] if len(parts) > 1 else "", "\n".join(body)))
    return res


def med(values, pick=st.median):
    vals = [v for v in values if v is not None]
    return pick(vals) if vals else None


def main():
    d = {}
    env = read("env.txt")
    d["env"] = {}
    for key, pat in [("kernel", r"^\d+\.\d+\.\d+\S*$"), ("model", r"Model name.*?:\s*(.+)$")]:
        m = re.search(pat, env, re.M)
        if m:
            d["env"][key] = (m.group(1) if m.groups() else m.group(0)).strip()
    m = re.search(r"^CPU\(s\):\s*(\d+)", env, re.M)
    if m:
        d["env"]["cpus"] = int(m.group(1))
    m = re.search(r"L3 cache:\s*(\S+)", env)
    if m:
        d["env"]["l3"] = m.group(1)

    d["pmu"] = {}
    for k, v in re.findall(r"([A-Z]+)=(\d)", read("pmu.txt")):
        d["pmu"][k] = bool(int(v))

    # ---- 三种负载 ----
    d["loads"] = {}
    for fname, kind in [("loads.txt", "load"), ("chase.txt", "chase")]:
        txt = read(fname)
        for arg, rnd, body in split_blocks(txt, kind):
            k = kv(body)
            ev = perf_events(body)
            key = "chase" if kind == "chase" else arg
            item = d["loads"].setdefault(key, {})
            item.setdefault("_cyc", []).append(float(k["CYC_PER_ITER"]) if "CYC_PER_ITER" in k else None)
            item.setdefault("_ns", []).append(float(k["CYC_PER_ITER"]) if "NS_PER_ITER" in k else None)
            if "CYC_PER_ITER" in k:
                item["cyc_per_iter"] = item.get("cyc_per_iter", [])
                item["cyc_per_iter"].append(float(k["CYC_PER_ITER"]))
            if "NS_PER_ITER" in k:
                item.setdefault("ns_per_iter", []).append(float(k["NS_PER_ITER"]))
            if "cycles" in ev and "instructions" in ev and ev["cycles"] > 0:
                item.setdefault("ipc", []).append(ev["instructions"] / ev["cycles"])
                item.setdefault("cycles", []).append(ev["cycles"])
                item.setdefault("instructions", []).append(ev["instructions"])
    for k, v in d["loads"].items():
        for f in list(v.keys()):
            if f.startswith("_"):
                del v[f]
            else:
                v[f] = {"med": st.median(v[f]), "min": min(v[f]), "n": len(v[f])}

    # ---- 分支 ----
    d["branch"] = {}
    for order, rnd, body in split_blocks(read("branch.txt"), "branch"):
        k = kv(body)
        ev = perf_events(body)
        it = d["branch"].setdefault(order, {})
        if "NS_PER_ELEM" in k:
            it.setdefault("ns_per_elem", []).append(float(k["NS_PER_ELEM"]))
        if "CYC_PER_ELEM" in k:
            it.setdefault("cyc_per_elem", []).append(float(k["CYC_PER_ELEM"]))
        if "branch-misses" in ev:
            it.setdefault("branch_misses", []).append(ev["branch-misses"])
        if "branches" in ev:
            it.setdefault("branches", []).append(ev["branches"])
        if "instructions" in ev and "cycles" in ev and ev["cycles"] > 0:
            it.setdefault("ipc", []).append(ev["instructions"] / ev["cycles"])
    for o, v in d["branch"].items():
        for f in list(v.keys()):
            v[f] = {"med": st.median(v[f]), "min": min(v[f])}
        if "branch_misses" in v and "branches" in v and v["branches"]["med"]:
            v["miss_rate"] = v["branch_misses"]["med"] / v["branches"]["med"]

    # ---- 工作集 ----
    d["wss"] = {"seq": [], "rnd": []}
    cur = {}
    for kind, arg, rnd, body in [(m.group(1), m.group(2), m.group(3), None)
                                 for m in re.finditer(r"^RES\|(wss_\w+)\|(\d+)\|(\d+)$", read("wss.txt"), re.M)]:
        pass
    # 逐块解析（上面用正则不方便，改为顺序扫描）
    seq_map, rnd_map = {}, {}
    curmode, cursize, buf = None, None, []
    for line in read("wss.txt").splitlines():
        m = re.match(r"^RES\|(wss_seq|wss_rnd)\|(\d+)\|(\d+)$", line)
        if m:
            if curmode and buf:
                k = kv("\n".join(buf))
                tgt = seq_map if curmode == "wss_seq" else rnd_map
                tgt.setdefault(int(cursize), []).append(k)
            curmode, cursize, buf = m.group(1), m.group(2), []
        elif curmode:
            buf.append(line)
    if curmode and buf:
        k = kv("\n".join(buf))
        tgt = seq_map if curmode == "wss_seq" else rnd_map
        tgt.setdefault(int(cursize), []).append(k)

    for size, items in sorted(seq_map.items()):
        g = [float(i["GBPS"]) for i in items if "GBPS" in i]
        n = [float(i["NS_PER_LINE"]) for i in items if "NS_PER_LINE" in i]
        if g:
            d["wss"]["seq"].append({"bytes": size, "gbps": max(g), "gbps_med": st.median(g),
                                    "ns_per_line": st.median(n) if n else None})
    for size, items in sorted(rnd_map.items()):
        n = [float(i["NS_PER_ACCESS"]) for i in items if "NS_PER_ACCESS" in i]
        if n:
            d["wss"]["rnd"].append({"bytes": size, "ns": min(n), "ns_med": st.median(n)})

    # ---- PMU 缺失率 ----
    d["wss_pmu"] = []
    cursize, buf = None, []
    for line in read("wss_pmu.txt").splitlines():
        m = re.match(r"^RES\|wss_pmu\|(\d+)$", line)
        if m:
            if cursize and buf:
                d["wss_pmu"].append({"bytes": int(cursize), "ev": perf_events("\n".join(buf))})
            cursize, buf = m.group(1), []
        elif cursize:
            buf.append(line)
    if cursize and buf:
        d["wss_pmu"].append({"bytes": int(cursize), "ev": perf_events("\n".join(buf))})

    # ---- 线程 ----
    d["threads"] = []
    tmap = {}
    for arg, rnd, body in split_blocks(read("threads.txt"), "thread"):
        k = kv(body)
        if "NS" not in k:
            continue
        tmap.setdefault(int(arg), []).append((float(k["NS"]), float(k.get("CTXT_SWITCHES", 0))))
    for t in sorted(tmap):
        ns = [x[0] for x in tmap[t]]
        sw = [x[1] for x in tmap[t]]
        d["threads"].append({"threads": t, "ns": min(ns), "ns_med": st.median(ns),
                             "switches": st.median(sw)})

    # ---- ftrace ----
    d["ftrace"] = kv(read("ftrace_stats.txt"))
    try:
        d["ftrace"]["count"] = int(read("ftrace_count.txt").strip() or 0)
    except ValueError:
        d["ftrace"]["count"] = 0

    # ---- gemv ----
    d["gemv"] = kv(read("gemv.txt"))
    for k in list(d["gemv"]):
        try:
            d["gemv"][k] = float(d["gemv"][k])
        except ValueError:
            pass

    # ---- 热点（perf report）----
    d["hot"] = []
    for line in read("perf_report.txt").splitlines():
        m = re.match(r"^\s*([\d.]+)%\s+(\S+)\s+(\S+)\s+\[.\]\s+(.*)$", line)
        if m:
            d["hot"].append({"pct": float(m.group(1)), "comm": m.group(2),
                             "dso": m.group(3), "sym": m.group(4).strip()})
    if not d["hot"]:
        for line in read("perf_report.txt").splitlines():
            m = re.match(r"^\s*([\d.]+)%.*?\s(\S+)$", line)
            if m:
                d["hot"].append({"pct": float(m.group(1)), "sym": m.group(2)})

    # ---- CPU 利用率 ----
    raw = read("cpu_util_raw.txt").split()
    if len(raw) >= 4:
        busy0, idle0, busy1, idle1 = map(float, raw[:4])
        db, di = busy1 - busy0, idle1 - idle0
        d["cpu_util"] = round(100.0 * db / (db + di), 2) if (db + di) else None
    d["top_busy"] = read("top_busy.txt")
    d["top_idle"] = read("top_idle.txt")

    with open(os.path.join(os.path.dirname(OUT), "med_perf.json"), "w", encoding="utf-8") as f:
        json.dump(d, f, ensure_ascii=False, indent=1)
    print(json.dumps(d, ensure_ascii=False, indent=1)[:3000])
    return d


if __name__ == "__main__":
    main()
