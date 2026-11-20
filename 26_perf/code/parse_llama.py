# -*- coding: utf-8 -*-
"""parse_llama.py - 把 llama-bench 三轮结果汇总成 JSON，供正文引用。"""
import json, re, statistics as st, io

RUNS = io.open("results21/llama_bench_r3.md", encoding="utf-8").read()
d = {}
for m in re.finditer(r"\|\s*CPU\s*\|\s*(\d+)\s*\|\s*tg64\s*\|\s*([\d.]+)\s*±\s*([\d.]+)\s*\|", RUNS):
    t, v = int(m.group(1)), float(m.group(2))
    d.setdefault(t, []).append(v)

SIZE_MIB = 638.74
SIZE_MB = round(SIZE_MIB * 1.048576, 1)      # 669.8 MB
PARAMS_M = 630.17

med = {t: st.median(v) for t, v in d.items()}
out = {
    "model": {"file": "qwen2.5-0.5b-instruct-q8_0.gguf", "size_mib": SIZE_MIB,
              "size_mb": SIZE_MB, "params_m": PARAMS_M,
              "source": "modelscope / Qwen/Qwen2.5-0.5B-Instruct-GGUF"},
    "build": "llama.cpp master (2026-11-20 tarball), GCC 12.3.1, -DGGML_NATIVE=ON",
    "rounds": 3, "repeat_per_round": 5,
    "runs": {str(k): v for k, v in sorted(d.items())},
    "med_tps": {str(k): round(v, 2) for k, v in sorted(med.items())},
    "ms_per_token": {str(k): round(1000.0 / v, 1) for k, v in sorted(med.items())},
    "bandwidth_gbps": {str(k): round(SIZE_MB * v / 1000.0, 2) for k, v in sorted(med.items())},
    "sd_rounds": {str(k): round(st.pstdev(v), 2) for k, v in sorted(d.items())},
    "hot": {"sym": "ggml_vec_dot_q8_0_q8_0", "pct": 77.85, "threads": 3,
            "samples": 16784, "note": "perf report 自占比；另有约 12% 落在 OpenMP 同步"},
}

json.dump(out, io.open("llama_bench.json", "w", encoding="utf-8"), ensure_ascii=False, indent=1)
print(json.dumps(out["med_tps"], ensure_ascii=False))
print(json.dumps(out["ms_per_token"], ensure_ascii=False))
print(json.dumps(out["bandwidth_gbps"], ensure_ascii=False))
