"""共用：读原始输出、构造 HTML 片段、画内联 SVG 折线图。"""
import os, re, statistics as st

BASE = 'C:/Users/Administrator/WorkBuddy/2026-11-14-01-50-47'
D = os.path.join(BASE, '_repo', 'rerun_dsw')

def rd(n):
    p = os.path.join(D, n)
    return open(p, encoding='utf-8', errors='replace').read()

def med(xs):
    return st.median(xs) if xs else float('nan')

# ---------- HTML 片段 ----------
P = '<p style="margin:0 0 14px 0;font-size:15px;line-height:1.95;color:#2b3238;">%s</p>'
PS = '<p style="margin:0 0 10px 0;font-size:13.5px;line-height:1.8;color:#6b7680;">%s</p>'
H3 = '<h3 style="font-size:16px;margin:24px 0 8px 0;color:#1c3f55;font-weight:700;line-height:1.6;">%s</h3>'
TABLE = '<table style="width:100%;border-collapse:collapse;margin:14px 0 6px 0;">'
TH = '<th style="padding:8px 10px;text-align:left;border:1px solid #dde6ec;color:#1c3f55;font-weight:700;background:#eef3f7;font-size:13px;">%s</th>'
TD = '<td style="padding:7px 10px;border:1px solid #dde6ec;text-align:left;font-size:13px;color:#2b3238;line-height:1.6;">%s</td>'
CODE = '<code style="background:#f1f5f8;padding:1px 5px;border-radius:3px;font-size:13px;color:#1c3f55;">%s</code>'

def table(headers, rows):
    h = ''.join(TH % x for x in headers)
    body = ''.join('<tr>' + ''.join(TD % c for c in r) + '</tr>' for r in rows)
    return TABLE + '<tr>' + h + '</tr>' + body + '</table>'

def c(x):
    return CODE % x

# ---------- SVG 双轴折线图 ----------
def svg_line(series_a, series_b, label_a, label_b, w=920, h=340, y1_min=None, y1_max=None, y2_min=None, y2_max=None,
             a_color='#2c5f7c', b_color='#d9822b', title=''):
    """series_a / series_b: [(x, y), ...]"""
    pad_l, pad_r, pad_t, pad_b = 64, 64, 26, 44
    iw, ih = w - pad_l - pad_r, h - pad_t - pad_b
    xs = [p[0] for p in series_a] or [0, 1]
    x0, x1 = min(xs), max(xs)
    if x1 == x0:
        x1 = x0 + 1
    ya = [p[1] for p in series_a] or [0, 1]
    yb = [p[1] for p in series_b] or [0, 1]
    a0 = y1_min if y1_min is not None else min(ya) * 0.9
    a1 = y1_max if y1_max is not None else max(ya) * 1.08
    b0 = y2_min if y2_min is not None else min(yb) * 0.9
    b1 = y2_max if y2_max is not None else max(yb) * 1.08
    if a1 == a0: a1 = a0 + 1
    if b1 == b0: b1 = b0 + 1

    def X(x): return pad_l + (x - x0) / (x1 - x0) * iw
    def YA(y): return pad_t + ih - (y - a0) / (a1 - a0) * ih
    def YB(y): return pad_t + ih - (y - b0) / (b1 - b0) * ih

    def path(ps, f):
        return 'M ' + ' L '.join('%.1f %.1f' % (X(x), f(y)) for x, y in ps)

    g = []
    # 网格 + 左轴
    for i in range(5):
        yv = a0 + (a1 - a0) * i / 4
        yy = YA(yv)
        g.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="#eef2f5" stroke-width="1"/>' % (pad_l, yy, w - pad_r, yy))
        g.append('<text x="%d" y="%.1f" font-size="11" fill="#7b8790" text-anchor="end">%s</text>' % (pad_l - 8, yy + 4, ('%.4g' % yv)))
        yv2 = b0 + (b1 - b0) * i / 4
        g.append('<text x="%d" y="%.1f" font-size="11" fill="#b8792f" text-anchor="start">%s</text>' % (w - pad_r + 8, YB(yv2) + 4, ('%.4g' % yv2)))
    # x 轴
    g.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="#cfd8df" stroke-width="1"/>' % (pad_l, pad_t + ih, w - pad_r, pad_t + ih))
    ticks = 6
    for i in range(ticks + 1):
        xv = x0 + (x1 - x0) * i / ticks
        g.append('<text x="%.1f" y="%d" font-size="11" fill="#7b8790" text-anchor="middle">%s</text>' % (X(xv), pad_t + ih + 18, ('%.4g' % xv)))
    # 曲线
    g.append('<path d="%s" fill="none" stroke="%s" stroke-width="2.2"/>' % (path(series_a, YA), a_color))
    g.append('<path d="%s" fill="none" stroke="%s" stroke-width="2" stroke-dasharray="0"/>' % (path(series_b, YB), b_color))
    for x, y in series_a:
        g.append('<circle cx="%.1f" cy="%.1f" r="2.6" fill="%s"/>' % (X(x), YA(y), a_color))
    for x, y in series_b:
        g.append('<circle cx="%.1f" cy="%.1f" r="2.4" fill="%s"/>' % (X(x), YB(y), b_color))
    # 图例
    g.append('<rect x="%d" y="8" width="10" height="10" fill="%s"/>' % (pad_l, a_color))
    g.append('<text x="%d" y="17" font-size="12" fill="#3c4a54">%s</text>' % (pad_l + 15, label_a))
    g.append('<rect x="%d" y="8" width="10" height="10" fill="%s"/>' % (pad_l + 190, b_color))
    g.append('<text x="%d" y="17" font-size="12" fill="#3c4a54">%s</text>' % (pad_l + 205, label_b))
    return ('<svg viewBox="0 0 %d %d" width="100%%" style="display:block;margin:16px 0 6px 0;background:#fff;border:1px solid #eaf0f4;border-radius:6px;">%s</svg>'
            % (w, h, ''.join(g)))

def svg_bars(groups, values, labels, w=920, h=320, color='#2c5f7c', ylab=''):
    pad_l, pad_r, pad_t, pad_b = 64, 24, 24, 46
    iw, ih = w - pad_l - pad_r, h - pad_t - pad_b
    v0, v1 = 0, max(values) * 1.15
    bw = iw / len(values) * 0.5
    g = []
    for i in range(5):
        yv = v0 + (v1 - v0) * i / 4
        yy = pad_t + ih - (yv - v0) / (v1 - v0) * ih
        g.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="#eef2f5"/>' % (pad_l, yy, w - pad_r, yy))
        g.append('<text x="%d" y="%.1f" font-size="11" fill="#7b8790" text-anchor="end">%s</text>' % (pad_l - 8, yy + 4, '%.4g' % yv))
    for i, (lab, v) in enumerate(zip(labels, values)):
        cx = pad_l + iw * (i + 0.5) / len(values)
        hh = (v - v0) / (v1 - v0) * ih
        g.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="%s" rx="3"/>' % (cx - bw / 2, pad_t + ih - hh, bw, hh, color))
        g.append('<text x="%.1f" y="%.1f" font-size="12" fill="#1c3f55" text-anchor="middle">%s</text>' % (cx, pad_t + ih - hh - 8, ('%.4g' % v)))
        g.append('<text x="%.1f" y="%d" font-size="12" fill="#7b8790" text-anchor="middle">%s</text>' % (cx, pad_t + ih + 20, lab))
    if ylab:
        g.append('<text x="%d" y="14" font-size="11" fill="#7b8790">%s</text>' % (pad_l, ylab))
    return ('<svg viewBox="0 0 %d %d" width="100%%" style="display:block;margin:16px 0 6px 0;background:#fff;border:1px solid #eaf0f4;border-radius:6px;">%s</svg>'
            % (w, h, ''.join(g)))


def svg_one(series, label, w=920, h=320, color='#2c5f7c', y_pad=0.002):
    pad_l, pad_r, pad_t, pad_b = 64, 30, 26, 44
    iw, ih = w - pad_l - pad_r, h - pad_t - pad_b
    xs = [p[0] for p in series]; ys = [p[1] for p in series]
    x0, x1 = min(xs), max(xs)
    y0, y1 = min(ys) * (1 - y_pad), max(ys) * (1 + y_pad)
    if x1 == x0: x1 = x0 + 1
    if y1 == y0: y1 = y0 + 1
    X = lambda x: pad_l + (x - x0) / (x1 - x0) * iw
    Y = lambda y: pad_t + ih - (y - y0) / (y1 - y0) * ih
    g = []
    for i in range(5):
        yv = y0 + (y1 - y0) * i / 4
        g.append('<line x1="%d" y1="%.1f" x2="%d" y2="%.1f" stroke="#eef2f5"/>' % (pad_l, Y(yv), w - pad_r, Y(yv)))
        g.append('<text x="%d" y="%.1f" font-size="11" fill="#7b8790" text-anchor="end">%.2f</text>' % (pad_l - 8, Y(yv) + 4, yv))
    for i in range(7):
        xv = x0 + (x1 - x0) * i / 6
        g.append('<text x="%.1f" y="%d" font-size="11" fill="#7b8790" text-anchor="middle">%s</text>' % (X(xv), pad_t + ih + 18, ('%.4g' % xv)))
    g.append('<line x1="%d" y1="%d" x2="%d" y2="%d" stroke="#cfd8df"/>' % (pad_l, pad_t + ih, w - pad_r, pad_t + ih))
    g.append('<path d="M ' + ' L '.join('%.1f %.1f' % (X(x), Y(y)) for x, y in series) + '" fill="none" stroke="%s" stroke-width="2.2"/>' % color)
    for x, y in series:
        g.append('<circle cx="%.1f" cy="%.1f" r="2.8" fill="%s"/>' % (X(x), Y(y), color))
    g.append('<rect x="%d" y="8" width="10" height="10" fill="%s"/>' % (pad_l, color))
    g.append('<text x="%d" y="17" font-size="12" fill="#3c4a54">%s</text>' % (pad_l + 15, label))
    return ('<svg viewBox="0 0 %d %d" width="100%%" style="display:block;margin:16px 0 6px 0;background:#fff;border:1px solid #eaf0f4;border-radius:6px;">%s</svg>'
            % (w, h, ''.join(g)))
