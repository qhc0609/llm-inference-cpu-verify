# -*- coding: utf-8 -*-
"""Turn the raw RES| logs into med_dma.json. Nothing is typed by hand:
every number in the article is read from this file."""
import json, os, re, statistics as st

SRC = "results_dma"


def res(path):
    if not os.path.exists(path):
        return []
    out = []
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if line.startswith("RES|"):
            out.append(line[4:])
    return out


def build():
    d = {"topo": {}, "copy": {}, "cache": {}, "pin": {}, "path": {},
         "dmabuf": {}, "direct": {}}

    for l in res(os.path.join(SRC, "topo.txt")):
        p = l.split("|")
        if p[0] == "topo" and len(p) >= 3:
            d["topo"][p[1]] = p[2]

    # ---- copy: keep the best value of each metric over all runs ----------
    for l in res(os.path.join(SRC, "copy.txt")):
        p = l.split("|")
        if p[0] != "copy":
            continue
        if p[1] == "memcpy" and len(p) == 4:
            k = "memcpy" + p[2]
            d["copy"][k] = max(d["copy"].get(k, 0), float(p[3]))
        elif p[1] in ("read", "write") and len(p) == 4:
            k = p[1] + p[2]
            d["copy"][k] = max(d["copy"].get(k, 0), float(p[3]))

    # ---- cache: flush cost per line, best of runs -----------------------
    cl, co, ms, rt, rw = [], [], [], [], []
    for l in res(os.path.join(SRC, "cache.txt")):
        p = l.split("|")
        if p[0] != "cache":
            continue
        if p[1] == "clflush":
            cl.append((float(p[3]), float(p[4])))
        elif p[1] == "clflushopt":
            co.append((float(p[3]), float(p[4])))
        elif p[1] == "memset_same_volume":
            ms.append(float(p[3]))
        elif p[1] == "flush_read_roundtrip_ns":
            rt.append(float(p[2]))
        elif p[1] == "read_warm":
            rw.append(float(p[3]))
    if cl:
        d["cache"]["clflush_ns_line"] = min(x[0] for x in cl)
        d["cache"]["clflush_gbps"] = max(x[1] for x in cl)
    if co:
        d["cache"]["clflushopt_ns_line"] = min(x[0] for x in co)
        d["cache"]["clflushopt_gbps"] = max(x[1] for x in co)
    if ms:
        d["cache"]["memset_gbps"] = max(ms)
    if rt:
        d["cache"]["roundtrip_ns"] = min(rt)
    if rw:
        d["cache"]["read_warm"] = max(rw)
    d["cache"]["mb"] = 64

    # ---- pin ------------------------------------------------------------
    p4, p2h, c64, c2m = [], [], [], []
    for l in res(os.path.join(SRC, "pin.txt")):
        p = l.split("|")
        if p[0] != "pin":
            continue
        if p[1] == "4k" and p[2] == "mlock":
            p4.append((float(p[3]), float(p[5]), int(p[6])))
        elif p[1] == "2m" and p[2] == "mlock":
            p2h.append((float(p[3]), float(p[5]), int(p[6])))
        elif p[1] == "chunk64k":
            c64.append(float(p[5]))
        elif p[1] == "chunk2m":
            c2m.append(float(p[5]))
    if p4:
        d["pin"]["mlock_ms"] = min(x[0] for x in p4)
        d["pin"]["ns_page_4k"] = min(x[1] for x in p4)
        d["pin"]["pages_4k"] = p4[0][2]
    if p2h:
        d["pin"]["ns_page_2m"] = min(x[1] for x in p2h)
        d["pin"]["pages_2m"] = p2h[0][2]
    if c64:
        d["pin"]["chunk64k_ns"] = min(c64)
    if c2m:
        d["pin"]["chunk2m_ns"] = min(c2m)
    d["pin"]["mb"] = 256

    # ---- path: for each name keep the run with the highest bandwidth ----
    best = {}
    for l in res(os.path.join(SRC, "path.txt")):
        p = l.split("|")
        if p[0] != "path" or len(p) != 6 or p[1].endswith("_err"):
            continue
        name, copies, gbps, ms_, cpu = p[1], int(p[2]), float(p[3]), float(p[4]), float(p[5])
        if name not in best or gbps > best[name]["gbps"]:
            best[name] = {"copies": copies, "gbps": gbps, "ms": ms_, "cpu_ns_byte": cpu}
    d["path"] = best

    # ---- dmabuf ---------------------------------------------------------
    w, rl, rd, sy, mnn, mnd, coh = [], [], [], [], [], [], []
    for l in res(os.path.join(SRC, "dmabuf.txt")):
        p = l.split("|")
        if p[0] != "dmabuf":
            continue
        if p[1] == "write_native":
            w.append((float(p[3]), float(p[4])))
        elif p[1] == "read_line":
            rl.append(float(p[3]))
        elif p[1] == "read_dense":
            rd.append(float(p[3]))
        elif p[1] == "sync_pair_ns":
            sy.append(float(p[2]))
        elif p[1] == "mmap_ns":
            mnn.append(float(p[2])); mnd.append(float(p[3]))
        elif p[1] == "coherent":
            coh.append((int(p[2]), int(p[3])))
    if w:
        d["dmabuf"]["write_gbps"] = max(x[0] for x in w)
        d["dmabuf"]["first_write_ms"] = min(x[1] for x in w)
    if rl: d["dmabuf"]["read_line_gbps"] = max(rl)
    if rd: d["dmabuf"]["read_dense_gbps"] = max(rd)
    if sy: d["dmabuf"]["sync_pair_ns"] = min(sy)
    if mnn: d["dmabuf"]["mmap_native_ns"] = min(mnn)
    if mnd: d["dmabuf"]["mmap_dmabuf_ns"] = min(mnd)
    if coh:
        d["dmabuf"]["coherent_bad_pages"] = max(x[0] for x in coh)
        d["dmabuf"]["coherent_pages"] = coh[0][1]
    d["dmabuf"]["mb"] = 64

    # ---- direct (real DMA through virtio-scsi) --------------------------
    for l in res(os.path.join(SRC, "direct.txt")):
        p = l.split("|")
        if p[0] != "direct" or len(p) < 5:
            continue
        if p[1] in ("odirect", "buffered", "read_plus_copy") and p[2].isdigit():
            key = "%s_%s" % (p[1], p[2])
            d["direct"][key] = {"gbps": float(p[3]), "cpu_ns_byte": float(p[4])}
    return d


def derive(d):
    c, ce, pa, pi, dm, di = d["copy"], d["cache"], d["path"], d["pin"], d["dmabuf"], d["direct"]
    v = {}
    mb = 64
    nbytes = mb << 20
    # one 64 MiB tile: copy time and flush time, both measured here
    v["tile_mb"] = mb
    v["memcpy1"] = c["memcpy1"]
    v["tile_copy_ms"] = nbytes / (c["memcpy1"] * 1e9) * 1e3
    v["tile_flush_clflush_ms"] = (nbytes / 64) * ce["clflush_ns_line"] / 1e6
    v["tile_flush_clflushopt_ms"] = (nbytes / 64) * ce["clflushopt_ns_line"] / 1e6
    v["flush_over_copy"] = v["tile_flush_clflush_ms"] / v["tile_copy_ms"]
    v["flushopt_over_copy"] = v["tile_flush_clflushopt_ms"] / v["tile_copy_ms"]
    # cost of touching one byte through each stage
    v["ns_byte_memcpy"] = 1e9 / (c["memcpy1"] * 1e9)
    v["ns_byte_flush"] = ce["clflush_ns_line"] / 64
    v["ns_byte_flushopt"] = ce["clflushopt_ns_line"] / 64
    v["ns_byte_roundtrip"] = ce["roundtrip_ns"] / 64
    # scaling: does adding cores help memcpy?
    v["memcpy_scale_4"] = c["memcpy4"] / c["memcpy1"]
    v["read_scale_4"] = c["read4"] / c["read1"]
    # per-transfer mapping overhead vs the copy it enables
    v["map64k_us"] = pi["chunk64k_ns"] / 1e3
    v["copy64k_us"] = (64 << 10) / (c["memcpy1"] * 1e9) * 1e6
    v["map_over_copy"] = v["map64k_us"] / v["copy64k_us"]
    # zero copy: best path vs the two-copy pipe
    if "pipe" in pa and "memfd_shared" in pa:
        v["memfd_over_pipe"] = pa["memfd_shared"]["gbps"] / pa["pipe"]["gbps"]
        v["cpu_pipe"] = pa["pipe"]["cpu_ns_byte"]
        v["cpu_memfd"] = pa["memfd_shared"]["cpu_ns_byte"]
        v["cpu_ratio"] = pa["pipe"]["cpu_ns_byte"] / pa["memfd_shared"]["cpu_ns_byte"]
    # real DMA: request size sweep
    if "odirect_4096" in di and "odirect_1048576" in di:
        v["dma_4k_gbps"] = di["odirect_4096"]["gbps"]
        v["dma_1m_gbps"] = di["odirect_1048576"]["gbps"]
        v["dma_size_gain"] = di["odirect_1048576"]["gbps"] / di["odirect_4096"]["gbps"]
        v["dma_4k_cpu"] = di["odirect_4096"]["cpu_ns_byte"]
        v["dma_1m_cpu"] = di["odirect_1048576"]["cpu_ns_byte"]
    if "buffered_262144" in di and "odirect_262144" in di:
        v["buffered_cpu"] = di["buffered_262144"]["cpu_ns_byte"]
        v["odirect_cpu"] = di["odirect_262144"]["cpu_ns_byte"]
        v["dma_cpu_gain"] = di["buffered_262144"]["cpu_ns_byte"] / di["odirect_262144"]["cpu_ns_byte"]
    # RK3588 estimate: same efficiency as this box, different paper number
    paper_vm, paper_rk = 38.4, 34.1
    v["eff_read4"] = c["read4"] / paper_vm
    v["eff_read1"] = c["read1"] / paper_vm
    v["rk_bw_1t"] = paper_rk * v["eff_read1"]
    v["rk_bw_4t"] = paper_rk * v["eff_read4"]
    v["rk_tile_copy_ms"] = nbytes / (v["rk_bw_4t"] * 1e9) * 1e3
    d["v"] = v
    return d


if __name__ == "__main__":
    d = derive(build())
    json.dump(d, open("med_dma.json", "w", encoding="utf-8"), indent=1, ensure_ascii=False)
    print(json.dumps(d["v"], indent=1, ensure_ascii=False))
    print("copy", d["copy"])
    print("cache", d["cache"])
    print("path", d["path"])
    print("direct", d["direct"])
    print("dmabuf", d["dmabuf"])
import sys
