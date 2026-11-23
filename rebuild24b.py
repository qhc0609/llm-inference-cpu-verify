# -*- coding: utf-8 -*-
"""第 24 篇第二阶段：10.5~10.9、11.2、附。"""
import os, re
import lib_html as L
c = L.c

ART = os.path.join(L.BASE, '第24篇-CPU功耗与温控优化.html')
STAGE = os.path.join(L.BASE, '_repo', '_html24_stage1.html')
html = open(STAGE, encoding='utf-8').read()

# --- 数据 ---
def sustain2(name):
    rows = []
    for line in L.rd(name).splitlines():
        f = line.split(',')
        if len(f) != 6:
            continue
        try:
            rows.append(dict(t=float(f[0]), tps=float(f[2]), p10=float(f[3]), p50=float(f[4])))
        except ValueError:
            pass
    return rows

single, quad = sustain2('secC2_single_p10.txt'), sustain2('secD2_quad_p10.txt')
S_tps, S_p10, S_p50 = L.med([r['tps'] for r in single]), L.med([r['p10'] for r in single]), L.med([r['p50'] for r in single])
Q_tps, Q_p10, Q_p50 = L.med([r['tps'] for r in quad]), L.med([r['p10'] for r in quad]), L.med([r['p50'] for r in quad])
SPD = Q_tps / S_tps

burst = []
for line in L.rd('rerun_dsw2_burst.txt').splitlines():
    f = line.split(',')
    if len(f) == 5 and f[0].isdigit():
        burst.append(dict(cyc=int(f[0]), tok=int(f[2]), tps=float(f[3]), us=float(f[4])))

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
        sh_try = (float(f[3]), float(f[4]))
        share[cur]['tok'].append(sh_try[0]); share[cur]['us'].append(sh_try[1])

SH = []
for k in sorted(share):
    SH.append((100 // (k + 1), k, L.med(share[k]['tok']), L.med(share[k]['us'])))

def swap(h, start_key, end_key, new):
    a = h.find(start_key); a = h.rfind('<h3', 0, a)
    b = h.find(end_key); b = h.rfind('<h3', 0, b)
    assert a >= 0 and b > a, (start_key, a, b)
    return h[:a] + new + h[b:]

# ---------------- 10.5 ----------------
fig3 = L.svg_bars([], [S_tps, Q_tps], ['1 线程（钉 1 核）', '4 线程（钉 4 物理核）'],
                  color='#2c5f7c', ylab='吞吐 token/s')
blk105 = L.H3 % '10.5 单核与四核放一起：为什么这次是 3.91 倍' + ''.join([
    L.table(['配置', '吞吐中位', 'p10 中位', 'p50 中位'], [
        ['1 线程（钉核 0）', '%.1f token/s' % S_tps, '%.0f μs' % S_p10, '%.0f μs' % S_p50],
        ['4 线程（钉 0,2,4,6）', '%.1f token/s' % Q_tps, '%.0f μs' % Q_p10, '%.0f μs' % Q_p50],
    ]),
    fig3,
    L.PS % ('上图：核数从 1 加到 4，吞吐从 102.0 涨到 398.8（3.91 倍），每 token 的 CPU 时间从 8.96 ms 涨到 9.95 ms（+11%）。'),
    L.P % ('<strong>3.91 倍</strong>，四个核对四个线程，效率 97.8%。这就是「资源给足」时这套负载该有的样子。'),
    L.P % ('为什么上一版只有 2.24 倍，这一版接近线性？差别不在代码，在芯片的两项参数：'),
    L.table(['参数', '上一版：i5-7300HQ', '这一版：Xeon 8369B', '对这套负载的意义'], [
        ['L3 容量', '6 MB', '48 MB', '32 MB 权重在 Xeon 上<strong>装得进 L3</strong>，四线程读的是同一份缓存；在 i5 上装不下，四条流全落到内存'],
        ['内存通道', '双通道 DDR4-2400（≈38 GB/s 理论）', '8 通道 DDR4-3200（≈205 GB/s 理论）', '四线程同时流式读时，Xeon 的带宽余量是 i5 的 5 倍以上'],
        ['物理核数', '4 核 4 线程（无 SMT）', '32 核 64 线程（配额 8）', '钉 4 个物理核不会撞到超线程兄弟'],
    ]),
    L.P % ('上一版把 2.24 倍解释成「四个线程抢同一份内存带宽」，这个解释在 i5 上是对的：6 MB 的 L3 装不下 32 MB 权重，四个线程各自流式读，'
           '很快就撞死在双通道内存上。这一次同样的代码、同样的权重，48 MB 的 L3 把整个权重装进去了，于是四个线程从「抢带宽」变成了「各自算」，'
           '加核就几乎线性。'),
    L.P % ('所以「4 核只快 2.24 倍」不是这条负载的固有属性，而是<strong>那颗芯片的算力带宽比</strong>。这也修正了上一版一个容易外推的结论：'
           '当时写「加核能加算力，加不了带宽，所以四个线程挤在一起谁也跑不满」——这句话只在 L3 装不下权重时成立。'
           '一旦权重大小落在 L3 之内，加核就是加算力。'),
    L.P % ('把这条接到端侧：判断「要不要多开线程」的正确顺序是<strong>先看权重能不能进末级缓存</strong>，再看内存通道数，最后才看核数。'
           '反过来做——先上多线程再发现没加速——就是上一版踩过的坑。'),
])

# ---------------- 10.6 ----------------
rows_b = [[str(b['cyc']), '%.1f' % 40.0, str(b['tok']), '%.1f' % b['tps'], '%.0f' % b['us']] for b in burst]
blk106 = L.H3 % '10.6 第三组：跑 40 秒停 20 秒，连做四轮' + ''.join([
    L.P % ('真实设备很少连续满载。更常见的是「用一阵、歇一阵」：语音唤醒、拍一张照、跑一段推理、然后待机。这种模式下热量会不会累积？第四轮会不会比第一轮慢？'),
    L.table(['轮次', '运行时长(s)', 'token 数', 'token/s', '每 token CPU 时间 (μs)'], rows_b),
    L.P % ('四轮几乎一模一样：398.3 / 398.3 / 398.7 / 398.6 token/s，每 token 9994 / 9991 / 9986 / 9988 μs。'
           '<strong>第一轮到第四轮，差 0.1%</strong>，在测量噪声之内。'),
    L.P % ('这和上一版形成了最直接的反差：笔记本上四轮是 92.4 → 99.3 → 86.5 → 74.8，最后一轮掉了 19%，每 token 时间从 34.2 ms 涨到 40.7 ms，'
           '两个数一起坏。当时只能写「观察到劣化，但无法归因到温度」。这一次连劣化的影子都没有。'),
    L.P % ('原因不神秘。这台机器的 CPU 时间由宿主的 cgroup 配额统一调度，散热也由宿主管——<strong>本容器无论跑多久，看到的都是同一份算力</strong>。'
           '只要 8 vCPU 配额没被别人抢走，就不会出现「跑热了变慢」这种形态；真出现变慢，也只会以「被限流」的形式出现，而那是 10.7 要拆的事。'),
    L.P % ('一句必须说的边界：<strong>「四轮没劣化」不等于「真机不会热降频」</strong>。它只说明在这台云实例上、这四分钟里，没有可观测的累积成本。'
           '要论证热降频，必须有温度或频率读数，而这台机器一条都没有。'),
])

# ---------------- 10.7 ----------------
fig4 = L.svg_bars([], [s[2] for s in SH], ['份额 %d%%' % s[0] for s in SH], color='#2c5f7c', ylab='吞吐 token/s')
rows_sh = [['%d%%' % s[0], str(s[1]), '%.1f' % s[2], '%.0f' % s[3], '%.2f' % (s[2] / SH[0][2])] for s in SH]
blk107 = L.H3 % '10.7 第四组：用「同核竞争」做一次份额对照' + ''.join([
    L.P % ('前面所有推断都建立在一个假设上：<strong>「每秒 token 数下降」可以由「分到的时间变少」引起，而「每 token 的 CPU 时间不变」</strong>。'
           '这个假设如果不验证，整篇就只是讲故事。'),
    L.P % ('上一版用的是 cgroup v2 的 <code>cpu.max</code>，思路很干净：不改频率、不改电压，只规定「每 100 ms 让你跑 X ms」。'
           '但这台机器上这条路走不通——它是 cgroup v1，而且 <code>/sys/fs/cgroup</code> 是只读挂载的：'),
    L.P % (c('mkdir /sys/fs/cgroup/cpu/wbexp') + ' → <code>Read-only file system</code>。容器里既不能建子组，也改不了自己的 <code>cpu.cfs_quota_us</code>。'),
    L.P % ('于是换了一个等价做法：<strong>在同一个物理核上放 k 个空转进程，把被测进程的时间份额压到 1/(k+1)</strong>。'
           'k=0 是满份额，k=1 是 50%，k=3 是 25%。原理和配额一样——不让它跑得更慢，只让它跑得更少。'),
    L.table(['理论份额', '空转进程数', 'token/s（中位）', '每 token CPU 时间（中位）', '相对吞吐'], rows_sh),
    fig4,
    L.PS % '上图：份额 100% / 50% / 25% 对应 101.9 / 50.9 / 25.4 token/s，几乎严格按比例。',
    L.P % ('结果比上一版还干净。吞吐 101.9 → 50.9 → 25.4，比例 <strong>1 : 0.50 : 0.25</strong>，和份额比例完全吻合；'
           '而每 token 的 CPU 时间 9768 → 9802 → 9846 μs，只涨了 0.8%，基本不动。'),
    L.P % ('上一版的同一实验是 47.2 / 26.1 / 14.0，比例 1 : 0.55 : 0.30——方向对，但偏离了理论值，当时解释为「时间片切碎后缓存更热」。'
           '这一次没有偏离，原因是竞争进程和被测算的是同一份紧循环，切换开销更小、也不涉及 cgroup 的 throttle 记账。'),
    L.P % ('这一组把方法验证了：<strong>「吞吐腰斩」完全可以在「每 token CPU 时间不变」的情况下发生</strong>。'
           '所以 10.3 和 10.4 里没有出现的那件事（吞吐掉、每 token 时间也涨），才是真正该警惕的降频信号。'),
    L.P % ('一处口径要说明：这一组的 100% 是 101.9 token/s，而 10.3 的 240 秒长跑是 102.0 token/s，两者一致；'
           '每 token 时间却差了 8.96 ms（p10）与 9.77 ms（本组用的是 getrusage 全窗口平均）。'
           '<strong>p10 与平均值不可混用</strong>，所以这一组只做内部横向比较，不跨组比绝对值。'),
])

# ---------------- 10.8 ----------------
first = L.med([m for _, m in rec[:3]]); mid = L.med([m for _, m in rec[len(rec) // 2 - 1:len(rec) // 2 + 2]]); last = L.med([m for _, m in rec[-3:]])
fig5 = L.svg_line([(t, m) for t, m in rec], [(t, m) for t, m in rec], '依赖链速度 Mops/s', '同一曲线（参考）',
                  y2_min=min(m for _, m in rec) * 0.999, y2_max=max(m for _, m in rec) * 1.001)
blk108 = L.H3 % '10.8 跑完之后的恢复曲线：有没有留下后遗症' + ''.join([
    L.P % ('五组跑完，最后一件事是「歇下来之后有没有恢复」。做法是跑一条纯依赖链（每步结果喂给下一步），'
           '它的速度正比于有效频率，而且不吃内存带宽——所以它能区分「频率掉了」和「带宽被占了」。'),
    fig5,
    L.PS % '上图：120 秒内每 8 秒一个点，速度从头到尾是一条直线。',
    L.table(['阶段', '依赖链速度（Mops/s）'], [
        ['前 24 秒', '%.2f' % first],
        ['中段', '%.2f' % mid],
        ['最后 24 秒', '%.2f' % last],
        ['全程极差', '%.2f（%.3f%%）' % (max(m for _, m in rec) - min(m for _, m in rec),
                                        (max(m for _, m in rec) - min(m for _, m in rec)) / mid * 100)],
    ]),
    L.P % ('864 Mops/s，全程在 <strong>864.19~864.48 之间</strong>，极差 0.03%。也就是说：跑完四组压力测试之后，'
           '这颗核的速度和刚开始时一模一样，<strong>没有留下任何可以测到的后遗症</strong>。'),
    L.P % ('这个结果比上一版更"干净"：上一版是 169.0 Mops/s 起步、203.2 收尾，中间有一段爬升（虚拟机冷启动时的缓存预热）。'
           '这次一开始就在稳态，因为容器是常驻的，探针跑之前已经热身过。'),
])

# ---------------- 10.9 ----------------
blk109 = L.H3 % '10.9 五组数据合起来，能说什么、不能说什么' + ''.join([
    L.table(['组', '配置', '关键数字', '判定'], [
        ['一', '单线程 240 s', '102.0 token/s，p10 8.96 ms', '全程平直，无漂移'],
        ['二', '四线程 180 s（4 物理核）', '398.8 token/s，p10 9.95 ms', '3.91 倍，接近线性，无抖动'],
        ['三', '40 s 跑 / 20 s 停 ×4', '398.3→398.6 token/s', '四轮无累积劣化'],
        ['四', '份额 100/50/25%', '101.9 / 50.9 / 25.4 token/s，每 token 9.77~9.85 ms', '吞吐严格按份额，方法自证'],
        ['五', '恢复探针 120 s', '864.2~864.5 Mops/s', '无后遗症'],
    ]),
    L.P % ('<strong>能说的三句：</strong>'),
    L.P % ('一、这套负载在这台机器上<strong>没有降频迹象</strong>。五组里没有任何一组出现「吞吐降 + 每 token 时间升」的组合，'
           '而这是降频唯一可靠的指纹。'),
    L.P % ('二、<strong>加核接近线性</strong>（3.91 倍），因为 32 MB 权重装得进 48 MB 的 L3。上一版的 2.24 倍是那颗芯片 L3 装不下权重导致的，'
           '不是「多线程没用」。这条修正对端侧选型影响很大：权重能不能进末级缓存，比核数更决定并行收益。'),
    L.P % ('三、<strong>「变慢」和「变少」可以完全分开</strong>。份额实验里吞吐砍到 1/4，每 token 时间只涨 0.8%——这就是「变少」；'
           '间歇实验里四轮一模一样——连「变少」都没有。'),
    L.P % ('<strong>不能说的三句：</strong>'),
    L.P % ('一、<strong>不能说「这颗 CPU 不会热降频」</strong>。这台机器没有温度、没有频率、没有功耗，四分钟也没跑出任何热积累。'
           '诚实的说法是「在本环境、本时长内未观察到」。'),
    L.P % ('二、<strong>不能把 3.91 倍外推到别的模型</strong>。32 MB 权重刚好落在 48 MB L3 之内是这份数据好看的前提；'
           '换成 7B 模型（fp16 约 14 GB），L3 一定装不下，四线程立刻退化回带宽受限。'),
    L.P % ('三、<strong>不能拿绝对吞吐和上一版比</strong>。一个是 Skylake 笔记本 4 核、AVX2、双通道；一个是 Ice Lake 服务器 8 配额、AVX2（刻意避开 AVX-512）、8 通道。'
           '能比的只有「比例」和「形态」，不能比绝对值。'),
])

html = swap(html, '10.5 单核与四核', '10.6 第三组', blk105)
html = swap(html, '10.6 第三组', '10.7 第四组', blk106)
html = swap(html, '10.7 第四组', '10.8 跑完之后', blk107)
html = swap(html, '10.8 跑完之后', '10.9 五组数据', blk108)
a = html.find('10.9 五组数据'); a = html.rfind('<h3', 0, a)
b = html.find('<h2', a)
html = html[:a] + blk109 + html[b:]

# ---------------- 11.2 ----------------
i = html.find('11.2 本机填完的样子'); i = html.rfind('<h3', 0, i)
j = html.find('11.3 换到真机上'); j = html.rfind('<h3', 0, j)
blk112 = L.H3 % '11.2 本机填完的样子' + ''.join([
    L.table(['项目', '内容'], [
        ['内核 / 环境', 'Ubuntu 22.04.5，内核 5.10.134-17.3.al8，Xeon Platinum 8369B（1 路 32 核 / 64 线程，L3 48 MB），cgroup 配额 8 vCPU，内存 28 GiB'],
        ['被测负载', '32 MB 权重逐 token 全量读取，算术强度 ≈ 0.25 FLOP/字节'],
        ['可读接口', '仅 <code>/proc/cpuinfo</code>（宿主采样 MHz，3400~3500 之间跳）、<code>getrusage</code>、cgroup v1 的配额文件（只读）'],
        ['不可读接口', 'cpufreq / thermal zone / cooling_device / powercap / MSR / perf / PSI，<strong>七路全空</strong>'],
        ['核心指标', '每秒 token 数（吞吐）＋ 每 token 的 CPU 时间 p10（频率代理）'],
        ['单线程 240 s', '吞吐中位 102.0 token/s，p10 中位 8.96 ms，无单调上升'],
        ['四线程 180 s（4 物理核）', '吞吐中位 398.8 token/s，p10 中位 9.95 ms'],
        ['并行效率', '1 → 4 线程 3.91 倍（97.8%），判定为带宽不再是瓶颈（权重可进 L3）'],
        ['间歇 4 轮', '398.3 / 398.3 / 398.7 / 398.6 token/s，<strong>无劣化</strong>'],
        ['份额 100/50/25%', '101.9 / 50.9 / 25.4 token/s，每 token 9768 / 9802 / 9846 μs'],
        ['恢复探针', '864.19~864.48 Mops/s，极差 0.03%，无后遗症'],
        ['一条额外发现', '<code>-march=native</code> 走 AVX-512 时每 token 25.14 ms，AVX2 只要 9.93 ms，<strong>差 2.53 倍</strong>'],
        ['结论边界', '本环境无温度/频率通道，「未观察到降频」不可外推为「真机不会降频」'],
    ]),
    L.P % '这张表就是报告的主体。注意倒数第二行——那条 AVX-512 的差是在换机器后才暴露的，它说明「同一份代码、不同编译参数」的差异，可以比「单核到四核」还大。',
])

html = html[:i] + blk112 + html[j:]

# 11.3 措辞：虚拟机 -> 云实例
html = html.replace('这份报告在虚拟机上完成，方法可以搬走，但有四处必须换：', '这份报告在云实例上完成，方法可以搬走，但有四处必须换：')
html = html.replace('虚拟机上的做法', '云实例上的做法')
html = html.replace('这台机器连温度都读不到，也连不上任何降温设备', '这台机器连温度都读不到，也连不上任何降温设备')

open(os.path.join(L.BASE, '_repo', '_html24_stage2.html'), 'w', encoding='utf-8').write(html)
print('stage2 ok, len', len(html))
