# -*- coding: utf-8 -*-
"""A10 上层的实测：CUDA Core vs Tensor Core 峰值达成率、发射开销、decode/prefill 两种形状、一次真实推理"""
import json
import os
import time

import torch

torch.backends.cuda.matmul.allow_tf32 = False
dev = 'cuda'
out = []


def rec(k, v):
    out.append((k, v))
    print('%-34s %s' % (k, v), flush=True)


def tf(x):
    return '%.1f' % x


def bench_gemm(n=4096, dtype=torch.float32, tf32=False, iters=5, tag=''):
    torch.backends.cuda.matmul.allow_tf32 = tf32
    a = torch.randn(n, n, device=dev, dtype=dtype)
    b = torch.randn(n, n, device=dev, dtype=dtype)
    for _ in range(2):
        c = a @ b
    torch.cuda.synchronize()
    t0 = time.perf_counter()
    for _ in range(iters):
        c = a @ b
    torch.cuda.synchronize()
    dt = time.perf_counter() - t0
    fl = 2.0 * n ** 3 * iters
    rec('gemm_%s_TFLOPS' % tag, tf(fl / dt / 1e12))
    del a, b, c
    torch.cuda.empty_cache()


def bench_gemv(rows=4096, cols=4096, batch=1, iters=20, tag=''):
    w = torch.randn(rows, cols, device=dev, dtype=torch.float16)
    x = torch.randn(batch, cols, device=dev, dtype=torch.float16)
    for _ in range(3):
        y = x @ w.t()
    torch.cuda.synchronize()
    t0 = time.perf_counter()
    for _ in range(iters):
        y = x @ w.t()
    torch.cuda.synchronize()
    dt = time.perf_counter() - t0
    nbytes = rows * cols * 2 * iters
    rec('gemv_%s_GBs' % tag, tf(nbytes / dt / 1e9))
    rec('gemv_%s_us_per_call' % tag, '%.1f' % (dt / iters * 1e6))
    del w, x, y
    torch.cuda.empty_cache()


def launch_overhead(iters=20000):
    a = torch.zeros(1024, device=dev)
    torch.cuda.synchronize()
    t0 = time.perf_counter()
    for _ in range(iters):
        a.add_(1.0)
    torch.cuda.synchronize()
    dt = time.perf_counter() - t0
    rec('launch_us_per_kernel', '%.2f' % (dt / iters * 1e6))
    rec('launch_kernels_per_s', tf(iters / dt))


def build_model(layers=12, hidden=1024, ff=4096):
    mods = []
    for _ in range(layers):
        mods.append(torch.nn.Linear(hidden, ff))
        mods.append(torch.nn.Linear(ff, hidden))
    m = torch.nn.Sequential(*mods).half().to(dev)
    return m


def inference_run(tokens=200):
    m = build_model()
    nparam = sum(p.numel() for p in m.parameters())
    rec('model_params_M', '%.1f' % (nparam / 1e6))
    x = torch.randn(1, 1024, device=dev, dtype=torch.float16)
    with torch.no_grad():
        for _ in range(5):
            x = m(x)
        torch.cuda.synchronize()
        t0 = time.perf_counter()
        for _ in range(tokens):
            x = m(x)
        torch.cuda.synchronize()
        dt = time.perf_counter() - t0
    rec('decode_tokens_per_s', tf(tokens / dt))
    rec('decode_GBs_from_weight', tf(nparam * 2 * tokens / dt / 1e9))
    del m, x
    torch.cuda.empty_cache()


def main():
    rec('torch', torch.__version__)
    rec('cuda', torch.version.cuda)
    rec('device', torch.cuda.get_device_name(0))
    rec('capability', '.'.join(map(str, torch.cuda.get_device_capability(0))))

    bench_gemm(4096, torch.float32, False, tag='fp32_notf32')
    bench_gemm(4096, torch.float32, True, tag='tf32')
    bench_gemm(4096, torch.float16, False, tag='fp16')
    bench_gemm(4096, torch.bfloat16, False, tag='bf16')

    bench_gemv(batch=1, tag='b1')
    bench_gemv(batch=8, tag='b8')
    bench_gemv(batch=32, tag='b32')

    launch_overhead()
    inference_run()

    with open('/tmp/r_torch.json', 'w') as f:
        json.dump(dict(out), f, indent=1)


if __name__ == '__main__':
    main()
