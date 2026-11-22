# -*- coding: utf-8 -*-
"""CPU-GPU 协同实测：推理时 GPU 忙到什么程度、功耗温度、CPU 在干什么、模型第一次进显存要走多久"""
import os
import subprocess
import threading
import time

import torch

dev = 'cuda'
res = []


def rec(k, v):
    res.append('%-30s %s' % (k, v))
    print(res[-1], flush=True)


def cpu_times():
    with open('/proc/stat') as f:
        p = f.readline().split()[1:]
    v = [int(x) for x in p]
    idle = v[3] + v[4]
    return sum(v), idle


def build(layers=12, hidden=1024, ff=4096):
    mods = []
    for _ in range(layers):
        mods.append(torch.nn.Linear(hidden, ff))
        mods.append(torch.nn.Linear(ff, hidden))
    return torch.nn.Sequential(*mods)


def main():
    rec('cuda', '%d.%d' % torch.cuda.get_device_capability(0))

    # 1) 权重第一次进显存：H2D 走 PCIe
    m = build()
    nparam = sum(p.numel() for p in m.parameters())
    rec('params_M', '%.1f' % (nparam / 1e6))
    mb = nparam * 2 / 1e6
    m = m.half()
    torch.cuda.synchronize()
    t0 = time.perf_counter()
    m = m.to(dev)
    x = torch.randn(1, 1024, device=dev, dtype=torch.float16)
    with torch.no_grad():
        y = m(x)
    torch.cuda.synchronize()
    dt = time.perf_counter() - t0
    rec('first_load_MB', '%.0f' % mb)
    rec('first_load_ms', '%.0f' % (dt * 1e3))
    rec('first_load_GBs', '%.1f' % (mb / 1e3 / dt))

    # 2) 稳态推理 + 同时采样 GPU 与 CPU
    dmon = open('/tmp/r_dmon.txt', 'w')
    p = subprocess.Popen(
        ['nvidia-smi', '--query-gpu=utilization.gpu,power.draw,temperature.gpu,clocks.sm,memory.used',
         '--format=csv,noheader,nounits', '-l', '1'], stdout=dmon, stderr=subprocess.DEVNULL)

    samples = []
    stop = [False]

    def mon():
        s0, i0 = cpu_times()
        while not stop[0]:
            time.sleep(1.0)
            s1, i1 = cpu_times()
            if s1 > s0:
                samples.append(100.0 * (1 - (i1 - i0) / (s1 - s0)))
            s0, i0 = s1, i1

    th = threading.Thread(target=mon)
    th.start()

    with torch.no_grad():
        t0 = time.perf_counter()
        for _ in range(4000):
            y = m(x)
        torch.cuda.synchronize()
        dt = time.perf_counter() - t0
    stop[0] = True
    th.join()
    p.terminate()
    dmon.close()
    rec('steady_tokens_per_s', '%.0f' % (4000 / dt))
    rec('steady_weight_GBs', '%.0f' % (mb / 1e3 * 4000 / dt))
    if samples:
        rec('cpu_busy_pct_mean', '%.1f' % (sum(samples) / len(samples)))
        rec('cpu_busy_pct_max', '%.1f' % max(samples))
        rec('cpu_samples', '%d' % len(samples))

    with open('/tmp/r_coop.txt', 'w') as f:
        f.write('\n'.join(res) + '\n')


if __name__ == '__main__':
    main()
