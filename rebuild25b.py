# -*- coding: utf-8 -*-
"""第 25 篇第二阶段：6.4~6.6、7.2~7.3、导语、附录。"""
import os, re
import lib_html as L
c = L.c

BASE = L.BASE
STAGE = os.path.join(BASE, '_repo', '_html25_stage1.html')
h = open(STAGE, encoding='utf-8').read()

def group(fname, prefix='GEMV'):
    g = {}
    for line in L.rd(fname).splitlines():
        m = re.search(r'variant=(\S+).*gbs=([\d.]+)', line)
        if m and line.startswith(prefix):
            g.setdefault(m.group(1), []).append(float(m.group(2)))
    return g
G = group('secC_gemv.txt')
def m(k):
    return L.med(G[k])

def gemm(fname):
    g = {}
    for line in L.rd(fname).splitlines():
        mm = re.search(r'variant=(\S+) n=(\d+).*gflops=([\d.]+) checksum=(\S+)', line)
        if mm:
            g.setdefault((int(mm.group(2)), mm.group(1)), []).append(float(mm.group(3)))
    return g
GM = gemm('secD_gemm.txt')
def gm(n, v):
    return L.med(GM[(n, v)])

def swap(h, start_key, end_key, new):
    a = h.find(start_key); a = h.rfind('<h3', 0, a)
    b = h.find(end_key); b = h.rfind('<h3', 0, b)
    assert a >= 0 and b > a, (start_key, a, b)
    return h[:a] + new + h[b:]

# ---------------- 6.4 ----------------
blk64 = L.H3 % '6.4 前后对比总表' + ''.join([
    L.table(['指标', '优化前', '优化后', '倍数', '靠什么'], [
        ['decode 带宽（32 MB GEMV）', '%.2f GB/s（标量）' % m('scalar'), '%.2f GB/s（AVX2×4 + 4 线程）' % m('mt:4'),
         '%.1f×' % (m('mt:4') / m('scalar')), '向量化 + 多累加器 + 多线程'],
        ['decode 单次搬运耗时', '9.8 ms', '0.40 ms', '%.1f×' % (m('mt:4') / m('scalar')), '同上'],
        ['prefill n=256', '%.2f GFLOPS' % gm(256, 'naive'), '%.2f GFLOPS' % gm(256, 'mt:4'), '%.1f×' % (gm(256, 'mt:4') / gm(256, 'naive')), '分块 64 + 4 线程'],
        ['prefill n=384', '%.2f GFLOPS' % gm(384, 'naive'), '%.2f GFLOPS' % gm(384, 'mt:4'), '%.1f×' % (gm(384, 'mt:4') / gm(384, 'naive')), '同上（mt:4 需先修 bug）'],
        ['prefill n=512', '%.2f GFLOPS' % gm(512, 'naive'), '%.2f GFLOPS' % gm(512, 'mt:4'), '%.1f×' % (gm(512, 'mt:4') / gm(512, 'naive')), '同上'],
        ['绑核', '全钉 1 个物理核 24.24 GB/s', '钉 4 个物理核 82.12 GB/s', '3.39×', '四个线程各占一条 L3 通道'],
        ['稳定性（AVX2×4 十次）', '28.99 ~ 30.68 ms', '离散度 1.06', '区间很窄', '钉核 + 配额充足'],
    ]),
    L.P % ('「优化前」指最朴素的实现：标量单线程、三重循环、不绑核。这是所有人写第一版代码时的样子。'),
    L.P % ('这张表最值得看的不是倍数，而是<strong>「倍数」和「瓶颈」的对应关系</strong>：'
           'decode 靠向量化拿 6.6 倍、再靠四线程拿 3.6 倍（因为权重进了 L3）；prefill 靠分块拿 15.6 倍、再靠线程拿 3 倍。'
           '两条流水线该做的事完全不同，但判据是同一个：<strong>工作集落在哪一级缓存里</strong>。'),
    L.P % ('还有一个反直觉的地方：<strong>同一个「加线程」动作，在换机器前是负收益（9.41 < 11.27），在这台机器上是 3.57 倍</strong>。'
           '所以「多线程对 decode 有没有用」这个问题没有统一答案，答案写在 L3 容量和内存通道数里。'),
])

# ---------------- 6.5 ----------------
def tok(bw, gb=14.0):
    return gb / bw, 1 / (gb / bw)
rows65 = []
for name, bw, note in [('标量单线程', m('scalar'), '实测带宽 ÷ 14 GB'),
                       ('AVX2 单线程', m('avx2'), '同上'),
                       ('AVX2×4 单线程', m('avx2x4'), '同上'),
                       ('AVX2×4 + 4 线程', m('mt:4'), '同上（中位口径）')]:
    t, r = tok(bw)
    rows65.append([name, '%.2f GB/s' % bw, '%.2f s' % t, '%.2f' % r, note])

blk65 = L.H3 % '6.5 换算成真模型：这台机器能跑多少 token/s' + ''.join([
    L.P % ('实验室里说 GB/s 不直观，换成 token/s 才有感觉。以 7B 模型、FP16 权重为例，一个 token 要把 14 GB 权重读一遍（这是 decode 阶段无法回避的下界）：'),
    L.table(['实现', '带宽', '每 token 理论耗时', '单核 token/s', '口径'], rows65),
    L.P % ('这是「权重读取」这一项的理论下界，不含注意力的 KV 读写、不含算子调度开销，所以是乐观值。'),
    L.P % ('换算下来：<strong>标量只有 0.24 token/s，向量化后 1.62，四线程能到 5.79 token/s</strong>。'
           '换机器前同一条除法算出的是 0.8——差别全部来自那 81 GB/s 里有多少是 L3 带宽。'
           '这也是为什么「端侧跑 7B」必须配合降精度：14 GB ÷ 81 GB/s 已经是最理想的情况了。'),
    L.P % ('端侧要跑到可用的速度，只有三条路：降精度（FP16 → INT8，字节数减半）、减参数量（7B → 1B）、'
           '或者把权重放进更快的存储（片上 SRAM）。这台机器上第三条是成立的——权重进 L3 之后，带宽直接翻了 3.5 倍。'),
    L.P % ('交叉验证：第二十四篇那个假模型是 32 MB 权重、<strong>标量</strong>实现，实测单线程 102.0 token/s，'
           '换算过来是 32 MB × 102.0 = <strong>3.26 GB/s</strong>，和这一篇标量 GEMV 的 3.42 GB/s 几乎相等；'
           '四线程 398.8 token/s × 32 MB = <strong>12.8 GB/s</strong>，正好是 3.42 的 4 倍（四个线程、标量路径）。'
           '两篇的数据自洽，说明「假模型确实复刻了 decode 的带宽特征」，而且它跑的是标量路径，没有偷用 SIMD。'),
    L.P % ('如果把那个假模型换成向量化，按同一算式 81.06 GB/s ÷ 32 MB ≈ 2400 token/s——这个数显然不可能实现，'
           '因为它假设了「除了搬权重什么都不做」。写在这里只是提醒：<strong>这类换算是下界，不是预测</strong>。'),
])

h = swap(h, '6.4 前后对比总表', '6.5 换算成真模型', blk64)
h = swap(h, '6.5 换算成真模型', '6.6 那两次', blk65)

# ---------------- 6.6 ----------------
i = h.find('6.6 那两次'); i = h.rfind('<h3', 0, i)
j = h.find('<h2', i)
blk66 = L.H3 % '6.6 那三次差点在错误的层上做优化' + ''.join([
    L.P % ('第五节留了一句话没兑现：「实验里至少两次差点在错误的层上做优化」。现在数据齐了，可以点名——这次是三处：'),
    L.table(['次数', '当时的错误判断', '真相', '纠正后'], [
        ['第一次', '顺序读 1 GB 只有 12.9 GB/s，判定「这台机器内存带宽极差」，准备去做预取和 NUMA 优化',
         '12.9 GB/s 是<strong>超出 L3</strong> 之后的值；8 MB 的数据集能跑 28.6 GB/s，写甚至 107.6 GB/s',
         '把工作集按 L3 分档测，才知道曲线在哪一级缓存掉下来'],
        ['第二次', 'decode 标量只有 3.42 GB/s，判定「带宽到顶了，加线程没用」',
         '3.42 是<strong>标量</strong>的上限，不是带宽的上限；向量化后 22.72，四线程 81.06',
         '先做向量化再谈并行，顺序反了就得出相反结论'],
        ['第三次', 'GEMM 多线程 checksum 对不上，判定「多线程把结果算错了」，准备放弃多线程',
         '是分块上界写错，最后一块重复算了下一线程的行；线程本身没问题',
         '改一行：gemm_thread 里把边界从 <code>i &lt; n</code> 改成 <code>i &lt; t-&gt;to</code>，n=384 的 checksum 立刻对上'],
    ]),
    L.P % ('三次里两次的共同点是：<strong>把「实现的缺陷」当成了「硬件的极限」</strong>。这也是为什么必须先做瓶颈判定，再动手。'),
    L.P % ('还有一条新的证据这次才拿到：那个越界 bug 在<strong>两台完全不同的机器上给出了同一个错值</strong>——'
           'n=384 mt:4 的 checksum 在 i5 上是 700705.291，在 Xeon 上还是 700705.291。'
           '确定性逻辑错就是这样：它跟硬件无关，跟性能曲线也无关，只有 checksum 能抓。'),
    L.P % ('这三条的共同教训可以合成一句：<strong>在怀疑硬件之前，先怀疑自己的测量代码。</strong>'
           '因为硬件的参数是固定的、可查的，而你的循环体里藏着一个依赖链还是藏着一个越界，只有一行一行看才知道。'),
])

h = h[:i] + blk66 + h[j:]

# ---------------- 7.2 bug 表 ----------------
old_tbl_start = h.find('尺寸', h.find('7.2 一个真 bug'))
# 用文本替换更稳：换修复前后两张表
h = h.replace('<tr><td style="padding:7px 10px;border:1px solid #dde6ec;text-align:left;font-size:13px;color:#2b3238;line-height:1.6;">n=384</td><td style="padding:7px 10px;border:1px solid #dde6ec;text-align:left;font-size:13px;color:#2b3238;line-height:1.6;">3.29 GFLOPS（但结果错）</td><td style="padding:7px 10px;border:1px solid #dde6ec;text-align:left;font-size:13px;color:#2b3238;line-height:1.6;">2.56 GFLOPS（中位）/ 3.19（最好）</td><td style="padding:7px 10px;border:1px solid #dde6ec;text-align:left;font-size:13px;color:#2b3238;line-height:1.6;">不再重复计算，时间变短，但「有效计算量」也变回真实值</td></tr>',
              '<tr><td style="padding:7px 10px;border:1px solid #dde6ec;text-align:left;font-size:13px;color:#2b3238;line-height:1.6;">n=384 mt:4</td><td style="padding:7px 10px;border:1px solid #dde6ec;text-align:left;font-size:13px;color:#2b3238;line-height:1.6;">22.63 GFLOPS（结果错）</td><td style="padding:7px 10px;border:1px solid #dde6ec;text-align:left;font-size:13px;color:#2b3238;line-height:1.6;">28.92 GFLOPS（checksum 正确）</td><td style="padding:7px 10px;border:1px solid #dde6ec;text-align:left;font-size:13px;color:#2b3238;line-height:1.6;">不再重复算同一批行，少了伪共享，<strong>修复后反而更快</strong></td></tr>')
h = h.replace('3.89 GFLOPS</td><td style="padding:7px 10px;border:1px solid #dde6ec;text-align:left;font-size:13px;color:#2b3238;line-height:1.6;">4.43 GFLOPS</td>',
              '29.55 GFLOPS</td><td style="padding:7px 10px;border:1px solid #dde6ec;text-align:left;font-size:13px;color:#2b3238;line-height:1.6;">29.20 GFLOPS</td>')
h = h.replace('2.49 GFLOPS</td><td style="padding:7px 10px;border:1px solid #dde6ec;text-align:left;font-size:13px;color:#2b3238;line-height:1.6;">1.99 GFLOPS</td>',
              '24.64 GFLOPS</td><td style="padding:7px 10px;border:1px solid #dde6ec;text-align:left;font-size:13px;color:#2b3238;line-height:1.6;">25.44 GFLOPS</td>')
h = h.replace('注意 n=384 修复后「变慢」了——因为之前那个 3.29 是把多算的工作量摊到更少的实际时间里算出来的假象。',
              '注意方向：<strong>这台机器上修复后反而更快</strong>（22.63 → 28.92），因为重复计算那批行除了白干活，还让 4 个线程同时写同一批 C 行，产生伪共享；'
              '而换机器前修复后是变慢的（3.29 → 2.56）。<strong>同一个 bug 对性能的影响方向都不固定</strong>——这正是「性能曲线看不出 bug」的最好证明。')
h = h.replace('700705 ÷ 560548 = 1.25，正好多出 25%', '700705 ÷ 560548 = 1.25，正好多出 25%（这个比值和机器无关，两台机器上一模一样）')

# ---------------- 7.3 ----------------
h = h.replace('53.87 ~ 73.33 ms', '28.99 ~ 30.68 ms')
h = h.replace('9.15 ~ 12.46 GB/s', '21.87 ~ 23.15 GB/s')
h = h.replace('64.56 ms', '29.70 ms')
h = h.replace('10.83 GB/s', '22.62 GB/s')
h = h.replace('36%', '5.8%')
h = h.replace('只报 12.46 会让人以为这是稳定值，实际上你得按 9.15 来规划容量', '只报 23.15 会让人以为这是稳定值，实际上你得按 21.87 来规划容量')
h = h.replace('这 10 次的耗时从 53.87 到 73.33 连续分布', '这 10 次的耗时从 28.99 到 30.68 连续分布')
h = h.replace('一半在 55 ms、一半在 70 ms', '一半在 29 ms、一半在 30.5 ms')

open(os.path.join(BASE, '_repo', '_html25_stage2.html'), 'w', encoding='utf-8').write(h)
print('25 stage2 ok', len(h))
