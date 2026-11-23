# -*- coding: utf-8 -*-
"""按新补的三组证据改写第 24 篇：perf / PSI / SMT / TSC / 10.1 差异段"""
import re

P = 'C:/Users/Administrator/WorkBuddy/2026-11-14-01-50-47/第24篇-CPU功耗与温控优化.html'
h = open(P, encoding='utf-8').read()

TD = 'padding:7px 10px;border:1px solid #dde6ec;text-align:left;font-size:13px;color:#2b3238;line-height:1.6;'
CODE = 'background:#f1f5f8;padding:1px 5px;border-radius:3px;font-size:13px;color:#1c3f55;'
PSTYLE = 'margin:0 0 14px 0;font-size:15px;line-height:1.95;color:#2b3238;'


def rep(a, b, n=1):
    global h
    if a not in h:
        print('MISS:', a[:70])
        return
    h = h.replace(a, b, n)


# ── 1. perf 行 ───────────────────────────────────────────────────────
rep('perf: command not found', '已装 perf 5.15.193')
rep('连 perf 都没装',
    '但容器 seccomp 拦下 perf_event_open，连 task-clock 都报 No permission')

# ── 2. PSI 行 ────────────────────────────────────────────────────────
rep('上一次还能读 PSI，这一版连「被抢」的直接证据都拿不到',
    '内核 5.10 未编入 PSI，用户态造不出来；改用 schedstat 的 run_delay 逐线程记账（10.5）')

# ── 3. 插入 schedstat 与 TSC 两行 ────────────────────────────────────
anchor = '改用 schedstat 的 run_delay 逐线程记账（10.5）</td></tr>'
rows = (
    anchor
    + '<tr><td style="%s">每线程排队时间</td><td style="%s"><code style="%s">/proc/self/task/&lt;tid&gt;/schedstat</code></td>'
      '<td style="%s">可读，第 2 字段单位 ns</td><td style="%s">本次替代 PSI 的主证据；<code style="%s">sched_schedstats</code> 已经是 1</td></tr>'
      % (TD, TD, CODE, TD, TD, CODE)
    + '<tr><td style="%s">周期计数</td><td style="%s"><code style="%s">rdtsc</code></td>'
      '<td style="%s">恒定 2.70 GHz</td><td style="%s">把「降频」和「代码路径变长」分开（10.2）</td></tr>'
      % (TD, TD, CODE, TD, TD)
)
if anchor in h:
    h = h.replace(anchor, rows, 1)
else:
    print('MISS anchor rows')

# ── 4. lscpu 行 ──────────────────────────────────────────────────────
rep('Xeon 8369B，64 逻辑核 / 32 核 / 2 线程',
    'Xeon 8369B @ 2.70 GHz；lscpu 报 64 逻辑核，cpuset 只给 0-7')
rep('单路 32 核，L3 48 MB，超线程是<strong>交错</strong>的（cpu0 的兄弟是 cpu1）',
    '单路 32 核、L3 48 MB；实测 <code style="%s">core_id</code> 为 0,0,1,1,2,2,3,3，即 4 个物理核 × 2 SMT' % CODE)

# ── 5. 三处差异段 ────────────────────────────────────────────────────
a = h.rfind('<p', 0, h.find('这张表里有三处和上一次'))
b = h.find('</p>', h.find('10.4 会直接影响结论')) + 4
NEW = (
    '<p style="@PS@">这三处差异我没有绕过去，而是逐条补齐了。</p>'
    '<p style="@PS@"><strong>perf 装上了，但装上也读不到。</strong>从清华源取了 '
    '<code style="@C@">linux-tools-5.15.0-164</code>，<code style="@C@">perf --version</code> 正常回 5.15.193。'
    '但 <code style="@C@">perf stat -e cycles</code> 报 No permission，换成纯软件的 '
    '<code style="@C@">task-clock</code> 也一样。排查下来：<code style="@C@">perf_event_paranoid</code> 是 -1，'
    'CapEff 里没有 CAP_PERFMON，而 <code style="@C@">Seccomp: 2</code>——真正的拦截者是容器的 seccomp 过滤器，'
    '它在 <code style="@C@">perf_event_open</code> 这个系统调用上直接返回 EPERM。'
    '这是容器的边界，不是配置问题，装什么包都解不开。</p>'
    '<p style="@PS@"><strong>PSI 是内核没编，但同一事件的账还能翻。</strong>'
    '<code style="@C@">/proc/pressure</code> 整个目录不存在，cgroup 里也没有 pressure 文件，用户态造不出来。'
    '但 PSI 在 CPU 上核算的本就是「runnable 任务的排队时间」，而这台机器的 '
    '<code style="@C@">sched_schedstats</code> 是 1，每个线程的 <code style="@C@">schedstat</code> 第二个字段就是 '
    '<code style="@C@">run_delay</code>（累计等了多久，单位 ns）。这比 PSI 更细：PSI 只给一个全局百分比，'
    'schedstat 能落到每个线程。10.5 用它把「线程在等 CPU」直接量了出来。</p>'
    '<p style="@PS@"><strong>超线程确实交错，但「不用超线程」这件事要分负载说。</strong>'
    '<code style="@C@">core_id</code> 是 0,0,1,1,2,2,3,3，cpuset 只给 0-7，也就是 <strong>4 个物理核</strong>。'
    '所以并行组全部固定 <code style="@C@">0,2,4,6</code>。但我另外补了一组 <code style="@C@">0-3</code> 做对照，'
    '结果值得单独说：它和 <code style="@C@">0,2,4,6</code> 几乎同速（393 对 395 token/s）。'
    '原因是这套 GEMV 只有一个累加器、走的是依赖链，单个线程每 4 个周期才出一个乘加，SMT 兄弟正好把空出来的执行单元填满。'
    '同一个 <code style="@C@">0-3</code> 在第 25 篇那个用四个累加器、已经吃满 SIMD 吞吐的 GEMV 上就只有 65%——'
    'SMT 有没有损失，取决于负载是「等延迟」还是「抢吞吐」。</p>'
).replace('@PS@', PSTYLE).replace('@C@', CODE)
h = h[:a] + NEW + h[b:]

# ── 6. 10.2 AVX-512 段 ───────────────────────────────────────────────
rep('cpu MHz 同时显示 3.4~3.5 GHz，说明这不是「降频到 2 GHz」那么简单，而是这条 512 位路径本身在这颗 U 上更亏。',
    '<code style="%s">cpu MHz</code> 显示 3.4~3.5 GHz，但那是宿主采样值，不能拿来定案。'
    '真正能分清的是 TSC：这是一枚恒定 2.70 GHz 的计数器，降频不会让它变慢。同一份源码，AVX2 路径每 token 花 '
    '<strong>27.25 M 周期</strong>，AVX-512 路径花 <strong>67.93 M 周期</strong>，比值 2.49；'
    '而墙钟时间的比值也是 2.49（10.09 ms 对 25.16 ms）。'
    '<strong>周期数和时间一起变大，说明不是降频——降频只会让时间变长、周期数不变——而是这条 512 位路径本身多花了 1.5 倍周期。</strong>' % CODE)

open(P, 'w', encoding='utf-8').write(h)
print('stage1 done, len', len(h))
