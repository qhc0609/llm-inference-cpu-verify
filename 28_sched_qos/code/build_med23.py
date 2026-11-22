#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""build_med23.py —— 把 results/sec*.txt 里的原始行解析成 RES| 汇总，并算派生指标。

用法：
    python3 build_med23.py [results 目录] > results/summary.txt

原则：只做解析和单位换算，不改写任何原始数字；算不出来的项一律留空而不是补一个。
"""
import glob
import os
import re
import sys

KV = re.compile(r'(\w+)=([^\s]+)')

# 解码实验的权重规模 -> 名义带宽。
# 程序里 wmb 的语义是 MiB，但正文报的是「MB / 毫秒」这个名义口径（与 23_membw 一致），
# 所以这里按十进制 MB 换算：GB/s = MB / ms。要换成 MiB 口径，乘以 1.048576 即可。
def mb_to_bytes(mb):
    return float(mb) * 1e6


def parse(path):
    rows = []
    for line in open(path, encoding='utf-8'):
        line = line.rstrip('\n')
        if not line or line.startswith('#'):
            continue
        tag = line.split(' ', 1)[0]
        if tag in ('DEC', 'PP', 'WORK', 'CPUPROBE', 'WEIGHT', 'STARVE'):
            d = dict(KV.findall(line))
            d['_tag'] = tag
            d['_file'] = os.path.basename(path)
            d['_raw'] = line
            rows.append(d)
    return rows


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), '..', 'results')
    files = sorted(glob.glob(os.path.join(root, 'sec*.txt')))
    if not files:
        sys.exit('no results: %s' % root)
    out = ['# 由 build_med23.py 生成，原始行见各 sec*.txt，本文件只是同一批数字的汇总']

    for f in files:
        out.append('')
        out.append('## %s' % os.path.basename(f))
        for d in parse(f):
            if d['_tag'] == 'DEC':
                ms = float(d['mean']) / 1e6
                bw = mb_to_bytes(d['wmb']) / ms / 1e6
                out.append('RES|%s|wmb=%sMB|each_token_mean_ms=%.2f|each_token_p50_ms=%.2f'
                           '|wait_pct=%s|slices=%s|equiv_bw_GBps=%.2f|%s'
                           % (d.get('policy'), d['wmb'], ms, float(d['p50']) / 1e6,
                              d['wait_pct'], d['slices'], bw, d['_raw']))
            elif d['_tag'] == 'PP':
                out.append('RES|%s|spin=%s|same=%s|n=%s|p50_us=%.2f|p99_us=%.2f'
                           '|p999_us=%.2f|max_us=%.2f|%s'
                           % (d.get('policy'), d['spin'], d['same'], d['n'],
                              float(d['p50']) / 1e3, float(d['p99']) / 1e3,
                              float(d['p999']) / 1e3, float(d['max']) / 1e3, d['_raw']))
            elif d['_tag'] == 'WORK':
                it = int(d['total_iters'])
                ms = float(d['wall_ms'])
                out.append('RES|WORK|threads=%s|total_iters=%d|wall_ms=%.2f'
                           '|kevents_per_s=%.1f|%s'
                           % (d['threads'], it, ms, it / ms, d['_raw']))
            elif d['_tag'] == 'CPUPROBE':
                out.append('RES|%s|cpu_pct=%s|wall_ms=%s|cpu_ms=%s|%s'
                           % (d.get('policy'), d['cpu_pct'], d['wall_ms'],
                              d['cpu_ms'], d['_raw']))
            elif d['_tag'] == 'WEIGHT':
                out.append('RES|WEIGHT|wa=%s|wb=%s|ratio_b_over_a=%s|%s'
                           % (d['wa'], d['wb'], d['ratio_wb_over_wa'], d['_raw']))
            else:
                out.append('RES|STARVE|%s' % d['_raw'])
    print('\n'.join(out))


if __name__ == '__main__':
    main()
