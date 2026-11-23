# -*- coding: utf-8 -*-
"""第 25 篇：6.1~6.3 重写。"""
import os, re
import lib_html as L
c = L.c

BASE = L.BASE
ART = os.path.join(BASE, '第25篇-热点与瓶颈.html')
h = open(ART, encoding='utf-8').read()

def stat(name, key='gbs'):
    v = []
    for line in L.rd(name).splitlines():
        m = re.search(r'%s=([\d.]+)' % key, line)
        if m and line.startswith('GEMV'):
            v.append(float(m.group(1)))
    return v

def group(fname):
    g = {}
    for line in L.rd(fname).splitlines():
        m = re.search(r'variant=(\S+).*gbs=([\d.]+)', line)
        if m:
            g.setdefault(m.group(1), []).append(float(m.group(2)))
    return g

G = group('secC_gemv.txt')
def gs(k):
    v = G[k]
    return '%.2f' % L.med(v), '%.2f' % min(v), '%.2f' % max(v)

A = {}
cur = None
for line in L.rd('secE_affinity.txt').splitlines():
    if line.startswith('##'):
        cur = line.strip('# ').strip(); A[cur] = []; continue
    m = re.search(r'gbs=([\d.]+)', line)
    if cur and m:
        A[cur].append(float(m.group(1)))

def swap(h, start_key, end_key, new):
    a = h.find(start_key); a = h.rfind('<h3', 0, a)
    b = h.find(end_key); b = h.rfind('<h3', 0, b)
    assert a >= 0 and b > a, (start_key, a, b)
    return h[:a] + new + h[b:]

# ---------------- 6.1 ----------------
rows = []
for k, name in [('scalar', '标量'), ('avx2', 'AVX2'), ('avx2x4', 'AVX2×4'), ('mt:1', 'mt:1'), ('mt:2', 'mt:2'), ('mt:4', 'mt:4')]:
    m, lo, hi = gs(k)
    rows.append([name, m, lo + ' ~ ' + hi])

fig = L.svg_bars([], [L.med(G[k]) for k in ('scalar', 'avx2', 'avx2x4', 'mt:2', 'mt:4')],
                 ['标量', 'AVX2', 'AVX2×4', 'mt:2', 'mt:4'], color='#2c5f7c', ylab='带宽 GB/s')

blk61 = L.H3 % '6.1 decode：向量化 6.6 倍，这次线程也吃到了' + ''.join([
    L.P % ('decode 阶段算的是 GEMV：32 MB 权重、每个 token 全量读一遍。六个变体各跑 5 次，钉在核 0 上（mt 变体钉 0,2,4,6）：'),
    L.table(['变体', '带宽中位（GB/s）', '区间'], rows),
    fig,
    L.PS % '上图：标量 3.42 → AVX2 21.38 → AVX2×4 22.72；线程一路加，mt:4 到 81.06 GB/s。',
    L.P % ('三个数字值得分开看：'),
    L.P % ('<strong>第一，向量化拿了 6.6 倍</strong>（3.42 → 22.72）。从 AVX2 到 AVX2×4 只多 6.3%，说明「一条指令算 8 个」的收益已经吃完，再加累加器只是把流水线填得更满。'
           '这个比例和换机器前的 6.0 倍几乎一样——向量化的收益跟 CPU 关系不大，跟「有没有 SIMD」关系很大。'),
    L.P % ('<strong>第二，这次线程真的有用</strong>：mt:4 到 81.06 GB/s，是单线程 AVX2×4 的 <strong>3.57 倍</strong>。'
           '换机器前 mt:4 只有 9.41，比单线程的 11.27 还低 17%——当时的结论是「内存总线已经满了，再开线程只会互拖」。'
           '这台机器上完全不同，原因只有一个：<strong>32 MB 权重能装进 48 MB 的 L3</strong>。'),
    L.P % ('<strong>第三，这 81 GB/s 主要是 L3 带宽，不是 DRAM 带宽</strong>。同一次实验里，顺序读 1 GB 数据时单线程只有 12.9 GB/s（见 6.3 后的 STREAM 表），'
           '而 32 MB 的 GEMV 能跑出 22.7 GB/s 单线程——差这么多，就是因为权重一直在 L3 里反复读。'
           '所以后面所有「带宽」数字，读的时候都要带上这个前提。'),
    L.P % ('这也是为什么这一篇的结论必须按机器分开说：<strong>同一份代码，在 L3 装不下权重的机器上是「带宽受限、加线程无益」，在 L3 装得下的机器上是「加线程接近线性」</strong>。'
           '判据不是「几个核」，而是「权重 ÷ 末级缓存」。'),
])

# ---------------- 6.2 ----------------
arows = []
order = ['4 线程全钉 1 个物理核（0）', '4 线程钉 4 个物理核（0,2,4,6）', '4 线程钉 0-3（=2 物理核 + 超线程）', '4 线程不钉核（64 逻辑核）']
label = {'4 线程全钉 1 个物理核（0）': '全钉 1 个物理核（0）',
         '4 线程钉 4 个物理核（0,2,4,6）': '钉 4 个物理核（0,2,4,6）',
         '4 线程钉 0-3（=2 物理核 + 超线程）': '钉 0-3（2 物理核 + 超线程）',
         '4 线程不钉核（64 逻辑核）': '不钉核'}
for k in order:
    v = A[k]
    arows.append([label[k], '%.2f' % L.med(v), '%.2f ~ %.2f' % (min(v), max(v)), '%.2f' % (max(v) / min(v))])

fig2 = L.svg_bars([], [L.med(A[k]) for k in order],
                  ['钉 1 核', '钉 4 物理核', '钉 0-3（SMT）', '不钉核'], color='#2c5f7c', ylab='带宽 GB/s')

blk62 = L.H3 % '6.2 绑核：这一次，钉四颗核比钉一颗快 3.4 倍' + ''.join([
    L.P % ('同样的 mt:4（四个线程、32 MB 权重、20 轮），只改绑核方式，五种摆法：'),
    L.table(['绑核方式', '带宽中位（GB/s）', '区间', '离散度'], arows),
    fig2,
    L.PS % '上图：全钉一颗核 24.24，钉四颗物理核 82.12——差 3.39 倍；钉 0-3（两个物理核 + 超线程）只有 53.18。',
    L.P % ('结论和换机器前<strong>完全相反</strong>：那时候「四个线程钉一颗核」比「散在四颗核上」快 31%，这一篇第二节还专门解释过为什么。'
           '在这台机器上是反过来的——钉四颗核比钉一颗快 3.39 倍，不钉核（84.74）和钉四颗（82.12）只差 3%。'),
    L.P % ('为什么反？还是那个 L3。换机器前那颗 i5 只有 6 MB L3，四个线程各自从内存拉 32 MB 权重，四条流把双通道打死，'
           '钉在一颗核上反而能让四个线程共享那 256 KB 的 L2、少发点内存请求。这台机器有 48 MB L3，权重整个躺在里面，'
           '四个线程读的是同一份缓存行，钉四颗核就是四条独立的 L3 通道，自然是越多越快。'),
    L.P % ('<strong>钉 0-3 那一行最值得看</strong>：53.18 GB/s，只有钉四颗物理核的 65%。因为超线程是交错的（cpu0 的兄弟是 cpu1），'
           '钉 0-3 实际上是四个线程挤在两个物理核上，两两共用执行单元。这也解释了为什么离散度是 1.24——比另外三种都大，'
           '两个线程抢同一个重排序缓冲区，抖动一定更凶。'),
    L.P % ('所以「绑核」不是一个可以背下来的动作，它是「权重能不能进 L3」和「SMT 拓扑怎么排」这两个事实的推论。'
           '换一颗 CPU，可能要反过来做。'),
])

# ---------------- 6.3 ----------------
def gemm(fname):
    g = {}
    for line in L.rd(fname).splitlines():
        m = re.search(r'variant=(\S+) n=(\d+).*gflops=([\d.]+) checksum=(\S+)', line)
        if m:
            g.setdefault((int(m.group(2)), m.group(1)), []).append((float(m.group(3)), m.group(4)))
    return g

GM = gemm('secD_gemm.txt')

def g5(n, v):
    xs = GM[(n, v)]
    return '%.2f' % L.med([x[0] for x in xs]), xs[0][1]

rows63 = []
for v, name in [('naive', 'naive'), ('block:32', 'block:32'), ('block:64', 'block:64'), ('block:128', 'block:128'), ('mt:2', 'mt:2'), ('mt:4', 'mt:4')]:
    vals = []
    for n in (256, 384, 512):
        med, cks = g5(n, v)
        vals.append(med + ('*' if n == 384 and v == 'mt:4' else ''))
    rows63.append([name] + vals)

fig3 = L.svg_bars([], [L.med([x[0] for x in GM[(512, v)]]) for v in ('naive', 'block:32', 'block:64', 'block:128', 'mt:2', 'mt:4')],
                  ['naive', 'b32', 'b64', 'b128', 'mt:2', 'mt:4'], color='#2c7c6a', ylab='GFLOPS（n=512）')

blk63 = L.H3 % '6.3 prefill：分块 15.6 倍，多线程再 3 倍' + ''.join([
    L.P % ('prefill 算的是 GEMM，三个尺寸 × 六个变体 × 各 3 次。表里是 GFLOPS 中位：'),
    L.table(['变体', 'n=256', 'n=384', 'n=512'], rows63),
    fig3,
    L.PS % '上图：n=512 时 naive 0.64 → 分块 10.0 → 四线程 29.55 GFLOPS。（* n=384 的 mt:4 是修复后的值，见 7.2）',
    L.P % ('以 n=512 为例：<strong>分块从 0.64 拉到 10.0，15.6 倍</strong>；再叠四线程到 29.55，<strong>累计 46 倍</strong>。'
           '分块为什么这么狠？因为 naive 版的内层循环按 i-j-k 走，每算一个 C[i][j] 都要把整行 A 和整列 B 从内存捞一遍，'
           '512×512 的矩阵每个元素被捞 512 次，缓存完全装不住。分块把工作集压到 64×64 个 double（32 KB），刚好落进 L1/L2。'),
    L.P % ('n=256 的倍数小得多（1.99 → 10.09，5.1 倍），因为 256 的矩阵只有 512 KB，本来就没怎么溢出 L2。'
           '这也说明分块不是「越大越好」的万能手段，它的收益正比于「原来有多糟」。'),
    L.P % ('数字和换机器前对比，倍数差得远（那时是分块 2.2 倍、线程再 3.4 倍），原因还是缓存：'
           'i5 的 L2 是 256 KB、L3 是 6 MB，n=512 的工作集在分块后仍然溢出到内存，所以分块只拿到 2.2 倍；'
           '这台机器的 L2 是 1.25 MB/核，64×64 的分块整个躺在 L2 里，收益自然大得多。'),
    L.P % ('<strong>n=384 的 mt:4 是修复后的数（22.63 → 28.92）</strong>，修复前后的原始输出都在仓库里。'
           '为什么修复后反而更快，7.2 会讲——这和换机器前的结论正好相反，也正说明「性能数字看不出这个 bug」。'),
])

h = swap(h, '6.1 decode', '6.2 绑核', blk61)
h = swap(h, '6.2 绑核', '6.3 prefill', blk62)
h = swap(h, '6.3 prefill', '6.4 前后对比总表', blk63)

open(os.path.join(BASE, '_repo', '_html25_stage1.html'), 'w', encoding='utf-8').write(h)
print('25 stage1 ok', len(h))
