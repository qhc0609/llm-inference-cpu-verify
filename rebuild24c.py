# -*- coding: utf-8 -*-
"""第 24 篇第三阶段：附录重写 + 个别措辞。"""
import os
import lib_html as L
c = L.c

ART = os.path.join(L.BASE, '第24篇-CPU功耗与温控优化.html')
h = open(ART, encoding='utf-8').read()

i = h.find('附：代码和原始输出')
i = h.rfind('<h2', 0, i)
j = h.find('<script', i)
assert i > 0 and j > i

blk = ('<h2 style="font-size:19.5px;margin:36px 0 14px 0;padding-left:12px;border-left:5px solid #2c5f7c;color:#1c3f55;font-weight:700;line-height:1.55;">附：代码和原始输出在哪</h2>'
       + L.P % ('这一篇的实验程序、复现脚本、八组原始输出都在同一个仓库里：'
                '<a href="https://github.com/qhc0609/llm-inference-cpu-verify" style="color:#2c5f7c;text-decoration:none;">https://github.com/qhc0609/llm-inference-cpu-verify</a>')
       + L.P % ('本篇用到的只有三样：一个测试程序 <code>pow24.c</code>、一个一键复现脚本 <code>repro_v2.sh</code>，以及 <code>rerun_dsw/</code> 目录下八组原样未改的终端输出。'
                '和本系列无关的工程代码不放在这个仓库里。')
       + L.table(['组', '文件', '内容', '对应正文'], [
           ['A', 'rerun_dsw/env.txt', '逐个读 cpufreq / thermal / powercap / MSR / perf / PSI 的原始回显，以及 lscpu、cgroup 配额', '10.1'],
           ['0', 'rerun_dsw/sec0_isa.txt', '同一份源码两种 -march 的对照（AVX-512 vs AVX2）', '10.2 末'],
           ['B', 'rerun_dsw/secB_short.txt', '五次 10 秒短测，看噪声量级', '10.3 前'],
           ['C', 'rerun_dsw/secC2_single_p10.txt', '单线程 240 秒，24 个 10 秒窗口', '10.3'],
           ['D', 'rerun_dsw/secD2_quad_p10.txt', '四线程 180 秒，钉 0,2,4,6 四个物理核', '10.4'],
           ['E', 'rerun_dsw/secE_recover.txt', '跑完之后的依赖链探针，120 秒 15 个点', '10.8'],
           ['F', 'rerun_dsw/rerun_dsw2_burst.txt', '40 秒跑 / 20 秒停，四轮', '10.6'],
           ['G', 'rerun_dsw/secG_share.txt', '同核竞争把份额压到 100/50/25%', '10.7'],
       ])
       + L.P % ('每一项都是程序的原样输出，没有手改过。两处必须说明：'
                '<strong>一是</strong>本文所有数字都来自 2026-11-23 在魔搭 DSW 云实例（Xeon 8369B / 8 vCPU 配额）上的这次重跑，'
                '仓库里另外还留着更早那次 4 vCPU 笔记本虚拟机的输出，两批不混用；'
                '<strong>二是</strong>文中的三张图是用 <code>rerun_dsw/</code> 里的数值现场画成内联 SVG 的，'
                '不是截图，所以换机器重跑后图会跟着数据一起变。')
       + L.P % ('复现命令只有一条（在任意 Linux 上，把 <code>code/</code> 和脚本放在同一层目录）：'
                '<code>bash repro_v2.sh</code>。它会先打环境自证，再编译（默认 <code>-march=x86-64-v3</code>），'
                '然后按 A–G 顺序跑完，最后把结果打成 <code>results_*.tar.gz</code>。')
       )

h = h[:i] + blk + h[j:]
h = h.replace('虚拟机上做功耗实验', '云实例上做功耗实验')

open(ART, 'w', encoding='utf-8').write(h)
print('附录已更新，总长度', len(h))
