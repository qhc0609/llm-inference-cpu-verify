#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""probe.py —— 把任意命令包起来，在它运行期间读 /proc 采 CPU 行为。

采的是被测进程自己的内核计数器，不是程序自己 print 的计时：
  * 所有线程累加的 utime+stime（真实 CPU 毫秒，多核可超过 wall）
  * voluntary / nonvoluntary 上下文切换
  * schedstat 的 run 时间（在 CPU 上跑）与 wait 时间（在 runqueue 里等）
  * 峰值 RSS、每 20ms 一条的原始曲线

用法：
  python3 probe.py --cmd "llama-bench -m ... -p 512 -n 0 -r 5" --tag A
输出：RES|{json}，以及 LOG| 行（被包命令的最后若干行输出）。
"""
import argparse
import json
import os
import shlex
import subprocess
import threading
import time

CLK_TCK = os.sysconf("SC_CLK_TCK") or 100


def read_cpu_ticks(pid):
    """累加进程所有线程的 utime+stime（jiffies）。线程可能刚退出，忽略异常。"""
    total = 0
    try:
        tids = os.listdir(f"/proc/{pid}/task")
    except OSError:
        return None
    for tid in tids:
        try:
            with open(f"/proc/{pid}/task/{tid}/stat", "rb") as f:
                data = f.read().decode("utf-8", "replace")
        except OSError:
            continue
        rest = data[data.rfind(")") + 2:].split()
        try:
            total += int(rest[11]) + int(rest[12])
        except (IndexError, ValueError):
            continue
    return total


def read_status(pid):
    out = {"rss_kb": 0, "vol": 0, "nonvol": 0}
    try:
        with open(f"/proc/{pid}/status", "rb") as f:
            for line in f.read().decode("utf-8", "replace").splitlines():
                if line.startswith("VmRSS:"):
                    out["rss_kb"] = int(line.split()[1])
                elif line.startswith("voluntary_ctxt_switches:"):
                    out["vol"] = int(line.split()[1])
                elif line.startswith("nonvoluntary_ctxt_switches:"):
                    out["nonvol"] = int(line.split()[1])
    except OSError:
        pass
    return out


def read_schedstat(pid):
    run_ns = wait_ns = 0
    try:
        tids = os.listdir(f"/proc/{pid}/task")
    except OSError:
        return 0, 0
    for tid in tids:
        try:
            with open(f"/proc/{pid}/task/{tid}/schedstat", "rb") as f:
                p = f.read().decode().split()
            run_ns += int(p[0])
            wait_ns += int(p[1])
        except (OSError, IndexError, ValueError):
            continue
    return run_ns, wait_ns


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmd", required=True)
    ap.add_argument("--tag", default="run")
    ap.add_argument("--raw", default="")
    ap.add_argument("--interval", type=float, default=0.02)
    ap.add_argument("--log-tail", type=int, default=8)
    a = ap.parse_args()

    argv = shlex.split(a.cmd)
    t0 = time.monotonic()
    proc = subprocess.Popen(argv, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, bufsize=1,
                            universal_newlines=True, errors="replace",
                            stdin=subprocess.DEVNULL)

    samples = []
    lines = []
    stop = threading.Event()

    def sampler():
        while not stop.is_set():
            ticks = read_cpu_ticks(proc.pid)
            if ticks is None:
                break
            st = read_status(proc.pid)
            run_ns, wait_ns = read_schedstat(proc.pid)
            samples.append((time.monotonic(), ticks, st["rss_kb"],
                            st["vol"], st["nonvol"], run_ns, wait_ns))
            time.sleep(a.interval)

    th = threading.Thread(target=sampler, daemon=True)
    th.start()
    for line in proc.stdout:
        lines.append(line.rstrip())
    proc.wait()
    stop.set()
    th.join(timeout=1.0)
    t_end = time.monotonic()

    cpu_ms = wall_ms = 0.0
    vol = nonvol = run_ms = wait_ms = 0
    peak_rss = 0
    n = 0
    if len(samples) >= 2:
        f, l = samples[0], samples[-1]
        cpu_ms = (l[1] - f[1]) / CLK_TCK * 1000
        wall_ms = (l[0] - f[0]) * 1000
        vol = l[3] - f[3]
        nonvol = l[4] - f[4]
        run_ms = (l[5] - f[5]) / 1e6
        wait_ms = (l[6] - f[6]) / 1e6
        n = len(samples)
    peak_rss = max([s[2] for s in samples], default=0)

    out = {
        "tag": a.tag,
        "cmd": a.cmd,
        "exit": proc.returncode,
        "wall_ms": round((t_end - t0) * 1000, 1),
        "sampled_wall_ms": round(wall_ms, 1),
        "cpu_ms": round(cpu_ms, 1),
        "cpu_util": round(cpu_ms / wall_ms, 2) if wall_ms else None,
        "vol_cs": vol,
        "nonvol_cs": nonvol,
        "run_ms": round(run_ms, 1),
        "wait_ms": round(wait_ms, 1),
        "peak_rss_kb": peak_rss,
        "n_samples": n,
    }
    print("RES|" + json.dumps(out, ensure_ascii=False))
    for ln in lines[-a.log_tail:]:
        print("LOG|" + ln)
    if a.raw:
        with open(a.raw, "w") as f:
            for s in samples:
                f.write(" ".join(str(x) for x in s) + "\n")


if __name__ == "__main__":
    main()
