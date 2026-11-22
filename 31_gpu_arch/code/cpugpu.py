# -*- coding: utf-8 -*-
"""同一台机器上：8 核 CPU 与 A10 跑同样的形状，直接对比"""
import time

import torch

res = []


def rec(k, v):
    res.append('%-28s %s' % (k, v))
    print(res[-1], flush=True)


def gemm(dev, n=2048, iters=3, dt=torch.float32):
    a = torch.randn(n, n, device=dev, dtype=dt)
    b = torch.randn(n, n, device=dev, dtype=dt)
    c = a @ b
    if dev == 'cuda':
        torch.cuda.synchronize()
    t0 = time.perf_counter()
    for _ in range(iters):
        c = a @ b
    if dev == 'cuda':
        torch.cuda.synchronize()
    return 2.0 * n ** 3 * iters / (time.perf_counter() - t0) / 1e9


def gemv(dev, rows=4096, cols=4096, iters=10, dt=torch.float32):
    w = torch.randn(rows, cols, device=dev, dtype=dt)
    x = torch.randn(1, cols, device=dev, dtype=dt)
    y = x @ w.t()
    if dev == 'cuda':
        torch.cuda.synchronize()
    t0 = time.perf_counter()
    for _ in range(iters):
        y = x @ w.t()
    if dev == 'cuda':
        torch.cuda.synchronize()
    dt_s = time.perf_counter() - t0
    return rows * cols * torch.finfo(dt).bits / 8 * iters / dt_s / 1e9, dt_s / iters * 1e6


def model(dev, layers=12, hidden=1024, ff=4096, tokens=100, dt=torch.float32):
    mods = []
    for _ in range(layers):
        mods.append(torch.nn.Linear(hidden, ff))
        mods.append(torch.nn.Linear(ff, hidden))
    m = torch.nn.Sequential(*mods).to(dev, dt)
    nparam = sum(p.numel() for p in m.parameters())
    x = torch.randn(1, hidden, device=dev, dtype=dt)
    with torch.no_grad():
        for _ in range(3):
            y = m(x)
        if dev == 'cuda':
            torch.cuda.synchronize()
        t0 = time.perf_counter()
        for _ in range(tokens):
            y = m(x)
        if dev == 'cuda':
            torch.cuda.synchronize()
        dt_s = time.perf_counter() - t0
    return nparam / 1e6, tokens / dt_s, nparam * 4 * tokens / dt_s / 1e9


def main():
    rec('cpu_threads', torch.get_num_threads())
    rec('cpu_gemm2048_GFLOPS', '%.1f' % gemm('cpu'))
    rec('gpu_gemm2048_GFLOPS', '%.1f' % gemm('cuda'))
    g, us = gemv('cpu')
    rec('cpu_gemv_GBs', '%.1f' % g)
    rec('cpu_gemv_us', '%.1f' % us)
    g2, us2 = gemv('cuda', dt=torch.float16)
    rec('gpu_gemv_fp16_GBs', '%.1f' % g2)
    rec('gpu_gemv_us', '%.1f' % us2)
    p1, t1, b1 = model('cpu')
    rec('cpu_model_M', '%.1f' % p1)
    rec('cpu_tokens_per_s', '%.2f' % t1)
    rec('cpu_weight_GBs', '%.2f' % b1)
    p2, t2, b2 = model('cuda', dt=torch.float16)
    rec('gpu_tokens_per_s', '%.0f' % t2)
    rec('gpu_weight_GBs', '%.0f' % b2)
    rec('speedup_decode', '%.0fx' % (t2 / t1))
    with open('/tmp/r_cpugpu.txt', 'w') as f:
        f.write('\n'.join(res) + '\n')


if __name__ == '__main__':
    main()
