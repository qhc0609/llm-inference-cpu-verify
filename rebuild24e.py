# -*- coding: utf-8 -*-
"""第 24 篇：删 PSI 列，重写 10.4 收尾与 10.5 归因"""
import re

P = 'C:/Users/Administrator/WorkBuddy/2026-11-14-01-50-47/第24篇-CPU功耗与温控优化.html'
h = open(P, encoding='utf-8').read()

TD = 'padding:7px 10px;border:1px solid #dde6ec;text-align:left;font-size:13px;color:#2b3238;line-height:1.6;'
TH = 'padding:8px 10px;text-align:left;border:1px solid #dde6ec;color:#1c3f55;font-weight:700;background:#eef3f7;font-size:13px;'
PS = 'margin:0 0 14px 0;font-size:15px;line-height:1.95;color:#2b3238;'
H3 = 'font-size:16px;margin:24px 0 8px 0;color:#1c3f55;font-weight:700;line-height:1.6;'
CODE = 'background:#f1f5f8;padding:1px 5px;border-radius:3px;font-size:13px;color:#1c3f55;'


# ── 1. 删掉两张表的 PSI 列 ──────────────────────────────────────────
n_th = len(re.findall(r'<th[^>]*>PSI</th>', h))
n_td = len(re.findall(r'<td[^>]*>-1</td>', h))
h = re.sub(r'<th[^>]*>PSI</th>', '', h)
h = re.sub(r'<td[^>]*>-1</td>', '', h)
print('removed th', n_th, 'td', n_td)

# ── 2. 10.3 正文一句话 ──────────────────────────────────────────────
a = 'PSI 这一列保留着，但这台机器读不到，所以全是 -1。'
b = ('PSI 这一列删掉了（原因见 10.1）：这台机器读不到，摆一列 -1 没有意义；'
     '「线程有没有在等」改用 schedstat 单独测，结果放在 10.5。')
if a in h:
    h = h.replace(a, b, 1)
else:
    print('MISS 10.3 note')

# ── 3. 10.4 收尾 ────────────────────────────────────────────────────
a = '当时只能靠 PSI 才能说清「是在被抢，不是降频」。这一次它稳到没有故事可讲——'
b = ('当时只能靠 PSI 才能说清「是在被抢，不是降频」。这一次同样的判决由 schedstat 给出：'
     '四个线程各占一个物理核，60 秒里平均等待占比 0.01%，没有一个窗口出现排队。它稳到没有故事可讲——')
if a in h:
    h = h.replace(a, b, 1)
else:
    print('MISS 10.4 note')

# ── 4. 重写 10.5 ────────────────────────────────────────────────────
i = h.find('>10.5 ')
i = h.rfind('<h3', 0, i)
j = h.find('>10.6 ')
j = h.rfind('<h3', 0, j)
print('10.5 span', i, j, 'len', j - i)

NEW = '''
<h3 style="@H3@">10.5 单核、四核、四线程挤一核：把「在等 CPU」量出来</h3>
<p style="@PS@">单核到四核，吞吐从 102.0 涨到 398.8 token/s，3.91 倍，四个核对四个线程，效率 97.8%。但「快 3.91 倍」只回答了「快了多少」，没回答「为什么不更快」。要回答后者，得知道线程有没有在排队——这正是 PSI 平时干的活。</p>
<p style="@PS@">这台机器没有 PSI，但有 <code style="@C@">schedstat</code>。写了一个和主实验同负载、同权重（32 MB）、同时长口径的探针，每 0.5 秒读一次每个 worker 的 <code style="@C@">/proc/self/task/&lt;tid&gt;/schedstat</code>，取第 2 字段 <code style="@C@">run_delay</code> 的增量，除以窗口时长，就是「这个线程有多少时间在 runqueue 里等」。三组各跑 60 秒：</p>
<table style="width:100%;border-collapse:collapse;margin:14px 0 6px 0;">
<tr><th style="@TH@">配置</th><th style="@TH@">吞吐中位 (token/s)</th><th style="@TH@">平均等待占比</th><th style="@TH@">单线程最大等待</th><th style="@TH@">忙占比</th></tr>
<tr><td style="@TD@">1 线程（钉核 0）</td><td style="@TD@">101.8</td><td style="@TD@">0.01%</td><td style="@TD@">0.02%</td><td style="@TD@">99.50%</td></tr>
<tr><td style="@TD@">4 线程（钉 0,2,4,6）</td><td style="@TD@">395.2</td><td style="@TD@">0.01%</td><td style="@TD@">0.02%</td><td style="@TD@">99.54%</td></tr>
<tr><td style="@TD@">4 线程（全挤核 0）</td><td style="@TD@">98.0</td><td style="@TD@"><strong>75.00%</strong></td><td style="@TD@">81.08%</td><td style="@TD@">24.87%</td></tr>
</table>
<p style="@PS@">第三行是全部实验里最干净的一条证据：四个线程挤一颗核，吞吐 98.0，和单线程 101.8 几乎一样；等待占比 75.00%，忙占比 24.87%——每个线程恰好拿到 1/4 的核，剩下 3/4 的时间在排队。而且这个 75% 不是估出来的：四个 worker 在每个 0.5 秒窗口里都有等待，<code style="@C@">n_waiting</code> 恒等于 4，单线程最大等待到过 81.08%。</p>
<p style="@PS@"><strong>吞吐不涨，不是因为频率掉了，是因为线程在等。</strong>这就是 PSI 想说的话，schedstat 说得更细：PSI 只给一个全局百分比，这里能落到每个线程，还能看出「是所有线程都在等，还是只有一个在等」。</p>
<p style="@PS@">反过来看第二行：四个线程各占一个物理核，等待 0.01%、忙 99.54%——CPU 供给是满的，所以 3.91 倍就是这台机器在这套负载上的真实上限，不是被谁偷走了一半。</p>

<h3 style="@H3@">那 i5 上的 2.24 倍是怎么来的</h3>
<p style="@PS@">i5 那次的 p10 是单核 16.10 ms、四核 15.95 ms，几乎不变；吞吐却只从 46.3 涨到 103.7。每 token 的 CPU 时间不变、总吞吐只涨 2.24 倍，只有一个解释：<strong>四个 vCPU 只拿到了 2.24 个核的 CPU 时间</strong>。那次用 cgroup 配额（100/50/25%）验证过：吞吐 47.2 / 26.1 / 14.0 近似线性，而 p10 反而 20.1→18.2→17.3 ms，方向与降频预测相反。</p>
<p style="@PS@">这次等于把同一件事反过来验：Xeon 上份额给足（等待 0.01%），3.91 倍；挤一颗核（等待 75%），98 token/s 不涨。两次实验、两个方向，指向同一个结论——<strong>这套负载的吞吐由「拿到多少 CPU 时间」决定，不由「CPU 跑多快」决定</strong>。这也解释了为什么这篇的标题是功耗与温控，结论却落在配额上：在一台读不到温度、读不到频率的机器上，你能管住的只有份额。</p>

<h3 style="@H3@">L3 和带宽在这里到底占多少</h3>
<p style="@PS@">换机器前我把 2.24 倍解释成「L3 装不下 32 MB 权重、四条流撞死内存带宽」。这个解释站不住，有两个反证。</p>
<p style="@PS@">第一，i5 那次每 token 的 CPU 时间没有变（16.10 → 15.95 ms）。如果真是带宽打满，单线程和四线程的每 token 时间应该一起变长，而不是几乎相同。</p>
<p style="@PS@">第二，把数算出来：四线程 398.8 token/s × 32 MB ≈ <strong>13.4 GB/s</strong>，而这台机器的 8 通道 DDR4-3200 理论带宽是 205 GB/s，只用掉 6.5%。离带宽墙还差一个数量级。</p>
<p style="@PS@">L3 确实起了作用，但不是决定性的：四个线程各自持有一份 32 MB 权重，合计 128 MB，装不进 48 MB 的 L3，所以四线程的 p10 从 8.96 ms 升到 9.95 ms（+11%），这就是真实的 L3 争用。但它只让每 token 慢 11%，不足以解释 2.24 与 3.91 的差别。</p>
<p style="@PS@">所以「要不要多开线程」的判断顺序应该改：先确认每个线程真的拿到了一个物理核（看 <code style="@C@">run_delay</code>），再看权重能不能进 L3，最后才看内存通道。反过来做——先上多线程、再猜是不是带宽不够——就是那颗 i5 上踩过的坑。</p>
'''.replace('@H3@', H3).replace('@PS@', PS).replace('@TD@', TD).replace('@TH@', TH).replace('@C@', CODE)

h = h[:i] + NEW.strip() + h[j:]

open(P, 'w', encoding='utf-8').write(h)
print('stage2 done, len', len(h))
