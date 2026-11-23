# -*- coding: utf-8 -*-
"""把第 24 篇的数据与结论替换为魔搭 DSW（Xeon 8369B / 8 vCPU）实测结果。"""
import os, re
import lib_html as L
c = L.c

ART = os.path.join(L.BASE, '第24篇-CPU功耗与温控优化.orig.html')
html = open(ART, encoding='utf-8').read()
orig_len = len(html)

# ---------------- 读原始输出 ----------------
def sustain2(name):
    rows = []
    for line in L.rd(name).splitlines():
        f = line.split(',')
        if len(f) != 6:
            continue
        try:
            rows.append(dict(t=float(f[0]), tps=float(f[2]), p10=float(f[3]), p50=float(f[4])))
        except ValueError:
            continue
    return rows

single = sustain2('secC2_single_p10.txt')
quad = sustain2('secD2_quad_p10.txt')
burst = []
for line in L.rd('secF_burst.txt').splitlines():
    f = line.split(',')
    if len(f) == 5 and f[0].isdigit():
        burst.append(dict(cyc=int(f[0]), on=float(f[1]), tok=int(f[2]), tps=float(f[3]), us=float(f[4])))
rec = []
for line in L.rd('secE_recover.txt').splitlines():
    f = line.split(',')
    if len(f) == 2 and re.match(r'^[\d.]+$', f[0]):
        rec.append((float(f[0]), float(f[1])))
share = {}
cur = None
for line in L.rd('secG_share.txt').splitlines():
    m = re.match(r'## k=(\d+)', line)
    if m:
        cur = int(m.group(1)); share[cur] = {'tok': [], 'us': []}; continue
    f = line.split(',')
    if cur is not None and len(f) == 8:
        share[cur]['tok'].append(float(f[3])); share[cur]['us'].append(float(f[4]))

S_tps = L.med([r['tps'] for r in single]); S_p10 = L.med([r['p10'] for r in single]); S_p50 = L.med([r['p50'] for r in single])
Q_tps = L.med([r['tps'] for r in quad]); Q_p10 = L.med([r['p10'] for r in quad]); Q_p50 = L.med([r['p50'] for r in quad])
SPD = Q_tps / S_tps
SH = [(100 // (k + 1), L.med(v['tok']), L.med(v['us'])) for k, v in sorted(share.items())]

# ---------------- 头图与首段 ----------------
html = html.replace(
    '2026-11-22 · 系列第 24 篇 · 实验环境：Ubuntu 6.8.0 / 4 vCPU 虚拟机',
    '2026-11-23 复测 · 系列第 24 篇 · 实验环境：Ubuntu 22.04 / 8 vCPU 云实例（Xeon Platinum 8369B）')

# ---------------- 10.1 ----------------
i = html.find('10.1 先摸清一件事')
i = html.rfind('<h3', 0, i)
j = html.find('10.2 温度读不到')
j = html.rfind('<h3', 0, j)

blk101 = L.H3 % '10.1 先摸清一件事：这台机器到底能读到什么' + ''.join([
    L.P % '动手之前先做了一件事，后来证明这是整篇最有价值的一步：<strong>把「想读的接口」一个一个去读，读不到的记下来</strong>。这不是浪费时间，因为一个测不出来的数，如果你假装测到了，后面所有结论都是假的。',
    L.table(['想读的东西', '读法', '本机结果', '意味着'], [
        ['当前频率', c('cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq'), '目录不存在', '没有 cpufreq 驱动，频率对软件不可见'],
        ['可用调频档', c('ls …/cpufreq/scaling_available_frequencies'), '不存在', '不能手动限频'],
        ['温度', c('cat /sys/class/thermal/thermal_zone*/temp'), '0 个 zone', '连 thermal 目录本身都是空的'],
        ['降温设备', c('ls /sys/class/thermal/'), '空', '没有任何 cooling_device'],
        ['功耗', c('ls /sys/class/powercap'), '空', '拿不到 RAPL 能耗计数'],
        ['实际频率（MSR）', c('rdmsr 0x198'), c('/dev/cpu') + ' 不存在', '这一层比上一版更彻底，设备节点都没给'],
        ['硬件计数', c('perf stat -e cycles,instructions'), 'perf: command not found', '连 perf 都没装'],
        ['内核压力指数 PSI', c('cat /proc/pressure/cpu'), '文件不存在', '上一版还能读 PSI，这一版连「被抢」的直接证据都拿不到'],
        ['标称频率', c('grep MHz /proc/cpuinfo'), '3400~3500 之间跳动', '这是宿主采样值，不是本容器的频率'],
        ['CPU 型号', c('lscpu'), 'Xeon 8369B，64 逻辑核 / 32 核 / 2 线程', '单路 32 核，L3 48 MB，超线程是<strong>交错</strong>的（cpu0 的兄弟是 cpu1）'],
        ['CPU 配额', c('cat /sys/fs/cgroup/cpu/cpu.cfs_quota_us'), '800000 / 100000', 'cgroup v1，配额正好 8 vCPU；可见 64 个逻辑核但只能用 8 个核的时间'],
        ['内存上限', c('cat …/memory.limit_in_bytes'), '30064771072', '28 GiB 硬上限，超了直接 OOM'],
    ]),
    L.P % '一份「读不到」的清单，比一份「读到了」的清单更该先写下来——它决定了后面所有结论能用什么口径说。',
    L.P % ('这张表里有三处和上一版（笔记本上的 4 vCPU 虚拟机）不同，而且都影响口径：'
           '一是 <strong>PSI 没有了</strong>，上一版靠它证明「线程在等 CPU」，这一版只能拿份额实验（10.7）来替代；'
           '二是 <strong>perf 根本没装</strong>，连 PMU 被 paranoid 关掉这一步都省了；'
           '三是 <strong>超线程是交错的</strong>，<code>cpu0</code> 的兄弟是 <code>cpu1</code>，所以「钉 4 个核」必须写成 '
           + c('taskset -c 0,2,4,6') + ' 而不是 ' + c('taskset -c 0-3') + '，后者其实只落在 2 个物理核上——这一点后面 10.4 会直接影响结论。'),
    L.P % ('这一段必须诚实标注：<strong>本机所有频率、温度、功耗读数均不可用</strong>。下面给出的所有数据，都是<strong>间接推断</strong>，不是直接测量。'),
])

# ---------------- 10.2 末尾追加「先钉死指令集」 ----------------
anchor102 = '10.3 第一组'
k = html.find(anchor102); k = html.rfind('<h3', 0, k)
add102 = (L.P % ('在正式开跑之前还有一步，是这次新加的：<strong>先把指令集路径钉死</strong>，否则后面所有数字都不可比。'
                 '这台机器是 Ice Lake，' + c('-march=native') + ' 会让 GCC 生成 AVX-512 代码；同一份源码用两种编译参数各跑 15 秒，结果差了 2.53 倍：')
         + L.table(['编译参数', '每 token CPU 时间（中位）', '吞吐'], [
             ['-march=native（走 AVX-512）', '25138.5 μs', '39.6 token/s'],
             ['-march=x86-64-v3（AVX2，与原文同路径）', '9931.5 μs', '100.2 token/s'],
         ])
         + L.P % ('<code>cpu MHz</code> 同时显示 3.4~3.5 GHz，说明这不是「降频到 2 GHz」那么简单，而是这条 512 位路径本身在这颗 U 上更亏。'
                  '上一版在 Skylake 笔记本上不会碰到这个问题，所以这一条是换机器后才暴露出来的。'
                  '后面所有实验统一用 <code>-march=x86-64-v3</code>，与原文的 AVX2 路径保持可比。'))

# ---------------- 10.3 ----------------
def rows_of(rows, want=8):
    step = max(1, len(rows) // want)
    pick = rows[::step][:want]
    if rows[-1] not in pick:
        pick = pick[:-1] + [rows[-1]]
    return [[('%.1f' % r['t']), str(int(r['tps'] * 10) if False else int(round(r['tps']))), ('%.1f' % r['tps']),
             ('%.0f' % r['p10']), ('%.0f' % r['p50']), '-1' if r['p10'] else '-'] for r in pick]

i = html.find('10.3 第一组'); i = html.rfind('<h3', 0, i)
j = html.find('10.4 第二组'); j = html.rfind('<h3', 0, j)

fig1 = L.svg_line([(r['t'], r['tps']) for r in single], [(r['t'], r['p10'] / 1000) for r in single],
                  '吞吐 token/s', 'p10 (ms)', y1_min=80, y1_max=115, y2_min=0, y2_max=12, a_color='#2c5f7c', b_color='#d9822b')

blk103 = L.H3 % '10.3 第一组：单线程跑 240 秒，每 10 秒记一个窗口' + ''.join([
    L.P % ('单线程、32 MB 权重、240 秒，钉在 CPU 0 上。每 10 秒结算一个窗口：这个窗口生成了多少 token、吞吐多少、窗口内每 token CPU 时间的 p10 和 p50 各是多少。'
           'PSI 这一列保留着，但这台机器读不到，所以全是 -1。'),
    L.table(['时间(s)', 'token 数', 'token/s', 'p10 (μs)', 'p50 (μs)', 'PSI'], [
        [('%.1f' % r['t']), ('%.0f' % (r['tps'] if False else r['tps'] * 10)), ('%.1f' % r['tps']), ('%.0f' % r['p10']), ('%.0f' % r['p50']), '-1']
        for r in [single[k] for k in (0, 3, 6, 9, 12, 15, 21, 23) if k < len(single)]
    ]),
    fig1,
    L.PS % '上图：蓝线是吞吐（token/s），橙线是每 token 的 CPU 时间 p10（毫秒）。240 秒里两条线都是平的——吞吐 101.8~102.0，p10 8.95~8.96 ms。',
    L.P % ('240 秒内 24 个窗口，吞吐中位 <strong>102.0 token/s</strong>，p10 中位 <strong>8957 μs</strong>（8.96 ms），p50 中位 9951 μs。'
           '首窗口 101.8、末窗口 102.0，差 0.2%，<strong>没有任何单调上升</strong>。'),
    L.P % ('这条曲线干净得有点意外。上一版在笔记本上同样跑 240 秒，吞吐是在 44.8~47.8 之间小幅抖、p10 在 16.0~16.1 ms 之间稳，'
           '那种「微微抖」的形态被解读成「宿主机上还有别人」。这一次连抖都没有，原因很直接：钉了一个物理核、配额有 8 核、'
           '整个窗口里没人和你抢那一个核。'),
    L.P % ('顺带一个数：p10 是 8.96 ms，p50 是 9.95 ms，<strong>p10 比 p50 快 11%</strong>。这个差不是噪声，是采样窗口切分造成的——'
           'p10 取的是最顺的那 10% 的迭代，代表「没有被打断时的真实速度」；p50 才是平均体感。后面所有比较都用 p10，口径统一。'),
])

# ---------------- 10.4 ----------------
i2 = html.find('10.4 第二组'); i2 = html.rfind('<h3', 0, i2)
j2 = html.find('10.5 单核与四核'); j2 = html.rfind('<h3', 0, j2)

fig2 = L.svg_line([(r['t'], r['tps']) for r in quad], [(r['t'], r['p10'] / 1000) for r in quad],
                  '吞吐 token/s', 'p10 (ms)', y1_min=380, y1_max=420, y2_min=0, y2_max=12)

blk104 = L.H3 % '10.4 第二组：四线程跑 180 秒，压力翻四倍' + ''.join([
    L.P % ('四个线程，钉在 <strong>0,2,4,6</strong> 四个不同的物理核上（不是 0-3，原因见 10.1 最后一条），跑 180 秒，每 10 秒一个窗口。'),
    L.table(['时间(s)', 'token 数', 'token/s', 'p10 (μs)', 'p50 (μs)', 'PSI'], [
        [('%.1f' % r['t']), ('%.0f' % (r['tps'] * 10)), ('%.1f' % r['tps']), ('%.0f' % r['p10']), ('%.0f' % r['p50']), '-1']
        for r in [quad[k] for k in (0, 2, 4, 6, 8, 10, 15, 17) if k < len(quad)]
    ]),
    fig2,
    L.PS % '上图：四线程下吞吐稳定在 397.9~398.9 token/s，p10 稳定在 9.92~9.95 ms。',
    L.P % ('180 秒内 18 个窗口，吞吐中位 <strong>398.8 token/s</strong>，p10 中位 <strong>9947 μs</strong>（9.95 ms），p50 中位 9958 μs。'
           '窗口间最大偏差 0.3%。'),
    L.P % ('和上一版对比一下就能看出差别有多大：笔记本上四线程的吞吐是在 <strong>45.6 到 136.8 之间来回跳</strong>，'
           '一倍多的摆动，当时只能靠 PSI 才能说清「是在被抢，不是降频」。这一次它稳到没有故事可讲——'
           '四个线程各占一个物理核、配额够用、没有邻居，于是它就跑出了这台机器应有的样子。'),
    L.P % ('这组数据本身的价值不在「好看」，而在于它把上一版的一个不确定项消掉了：<strong>当资源真的给足时，这套负载是可以线性扩展的</strong>。'
           '如果这里仍然只有 2 倍多，那才说明瓶颈在带宽。'),
])

def swap(h, start_key, end_key, new):
    a = h.find(start_key); a = h.rfind('<h3', 0, a)
    b = h.find(end_key); b = h.rfind('<h3', 0, b)
    assert a >= 0 and b > a, (start_key, a, b)
    return h[:a] + new + h[b:]

html = open(ART, encoding='utf-8').read()
html = html.replace('2026-11-22 · 系列第 24 篇 · 实验环境：Ubuntu 6.8.0 / 4 vCPU 虚拟机',
                    '2026-11-23 复测 · 系列第 24 篇 · 实验环境：Ubuntu 22.04 / 8 vCPU 云实例（Xeon Platinum 8369B）')
html = swap(html, '10.1 先摸清一件事', '10.2 温度读不到', blk101)
a = html.find('10.3 第一组'); a = html.rfind('<h3', 0, a)
html = html[:a] + add102 + html[a:]
html = swap(html, '10.3 第一组', '10.4 第二组', blk103)
html = swap(html, '10.4 第二组', '10.5 单核与四核', blk104)

print('10.1~10.4 完成，长度 %d -> %d' % (orig_len, len(html)))
open(os.path.join(L.BASE, '_repo', '_html24_stage1.html'), 'w', encoding='utf-8').write(html)
