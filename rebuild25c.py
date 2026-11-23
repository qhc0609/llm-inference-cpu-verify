# -*- coding: utf-8 -*-
"""第 25 篇第三阶段：头部、附录、收尾。"""
import os
import lib_html as L
c = L.c

BASE = L.BASE
src = os.path.join(BASE, '_repo', '_html25_stage3.html')
h = open(src, encoding='utf-8').read()

h = h.replace('2026-11-22 · 系列第 25 篇 · 实验环境：Ubuntu 6.8.0 / 4 vCPU 虚拟机 / AVX2',
              '2026-11-23 复测 · 系列第 25 篇 · 实验环境：Ubuntu 22.04 / 8 vCPU 云实例（Xeon 8369B，32 MB 权重可进 48 MB L3）')

i = h.find('附：代码和原始输出')
i = h.rfind('<h2', 0, i)
j = h.find('<script', i)
assert i > 0 and j > i

blk = ('<h2 style="font-size:19.5px;margin:36px 0 14px 0;padding-left:12px;border-left:5px solid #2c5f7c;color:#1c3f55;font-weight:700;line-height:1.55;">附：代码和原始输出在哪</h2>'
       + L.P % ('这一篇的测试程序、复现脚本、九组原始输出都在同一个仓库里：'
                '<a href="https://github.com/qhc0609/llm-inference-cpu-verify" style="color:#2c5f7c;text-decoration:none;">https://github.com/qhc0609/llm-inference-cpu-verify</a>')
       + L.P % ('本篇用到的只有三样：一个测试程序 <code>hot25.c</code>（chase / stream / gemv / gemm 四种模式）、一个复现脚本 <code>repro_v2.sh</code>，'
                '以及 <code>rerun_dsw/</code> 下九组原样未改的终端输出。和本系列无关的工程代码不放在这个仓库里。')
       + L.table(['组', '文件', '内容', '对应正文'], [
           ['A', 'rerun_dsw/secA_cache.txt', '随机指针追逐，14 档工作集（4 KB ~ 64 MB）', '第三节 / 6.1 引子'],
           ['B', 'rerun_dsw/secB_stream.txt', '顺序读 / 写 / 拷贝，8 MB、128 MB、1 GB 三档', '6.1 / 6.6'],
           ['C', 'rerun_dsw/secC_gemv.txt', 'GEMV 六个变体 × 5 次', '6.1 / 6.4'],
           ['D', 'rerun_dsw/secD_gemm.txt', 'GEMM 三尺寸 × 六变体 × 3 次（含 n=384 的错误值）', '6.3 / 7.2'],
           ['E', 'rerun_dsw/secE_affinity.txt', '四种绑核方式 × 5 次', '6.2'],
           ['F', 'rerun_dsw/secF_before_after.txt', '优化前后三档对照', '6.4'],
           ['G', 'rerun_dsw/secG_regress.txt', 'AVX2×4 连跑十次 + GEMM 一致性 + n=384 复测', '7.1 / 7.3'],
           ['H', 'rerun_dsw/rerun_dsw2_burst.txt', '（第 24 篇共用）间歇四轮', '第 24 篇 10.6'],
           ['I', '修复版源码', '把 <code>i &lt; n</code> 改成 <code>i &lt; t-&gt;to</code> 之后重编的 hot25，n=384 的 checksum 变为 560548.199', '7.2'],
       ])
       + L.P % ('每一项都是程序的原样输出，没有手改过。三点说明：'
                '<strong>一是</strong>本文数字全部来自 2026-11-23 在魔搭 DSW 云实例（Xeon 8369B / 8 vCPU 配额）上的重跑，'
                '仓库里还留着更早那次 4 vCPU 笔记本虚拟机的输出，两批不混用；'
                '<strong>二是</strong>编译统一用 <code>-O3 -march=x86-64-v3 -mavx2</code>——'
                '在这颗 Ice Lake 上用 <code>-march=native</code> 会走 AVX-512，同一份代码实测慢 2.5 倍（第 24 篇 10.2 有对照）；'
                '<strong>三是</strong>文中两张图是用 <code>rerun_dsw/</code> 的数值现场画的内联 SVG，不是截图。')
       + L.P % ('复现命令：<code>bash repro_v2.sh</code>。'
                '只想复现 bug 的话，把 <code>hot25.c</code> 里 gemm_thread 的边界从 <code>i &lt; n</code> 改回 <code>i &lt; t-&gt;to</code> 再跑 '
                '<code>hot25 gemm 384 mt:4</code>，checksum 会从 700705.291 变回 560548.199——'
                '两台不同机器上都一样，这是确定性逻辑错，不是浮点噪声。')
       )

h = h[:i] + blk + h[j:]
open(os.path.join(BASE, '第25篇-热点与瓶颈.html'), 'w', encoding='utf-8').write(h)
print('25 final', len(h))
