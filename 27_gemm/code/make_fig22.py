# -*- coding: utf-8 -*-
"""make_fig22.py - 第 22 篇的三张图（PIL 直画，无 matplotlib）"""
import json, math, os
from PIL import Image, ImageDraw, ImageFont

S = 2
INK, MUTED, GRID = (26, 32, 44), (110, 120, 134), (226, 232, 240)
BLUE, RED, GREEN, ORANGE, LBLUE = (30, 58, 138), (176, 42, 55), (16, 120, 96), (194, 84, 20), (147, 175, 220)
BG = (255, 255, 255)

HERE = os.path.dirname(os.path.abspath(__file__))
D = json.load(open(os.path.join(HERE, "med_perf22.json"), encoding="utf-8"))


def F(sz, bold=False):
    for p in (r"C:/Windows/Fonts/msyhbd.ttc" if bold else r"C:/Windows/Fonts/msyh.ttc",
              r"C:/Windows/Fonts/simhei.ttf"):
        if os.path.exists(p):
            try:
                return ImageFont.truetype(p, sz)
            except Exception:
                pass
    return ImageFont.load_default()


def canvas(w, h):
    im = Image.new("RGB", (w * S, h * S), BG)
    return im, ImageDraw.Draw(im)


def tw(d, s, f):
    b = d.textbbox((0, 0), s, font=f)
    return b[2] - b[0]


def dash_v(d, x, y0, y1, col, w=1, dash=6):
    y = y0
    while y < y1:
        d.line([x * S, y * S, x * S, min(y + dash, y1) * S], fill=col, width=w * S)
        y += dash * 2


def dash_h(d, y, x0, x1, col, w=1, dash=6):
    x = x0
    while x < x1:
        d.line([x * S, y * S, min(x + dash, x1) * S, y * S], fill=col, width=w * S)
        x += dash * 2


# ---------------------------------------------------------------- 图 1：优化阶梯
def fig_ladder():
    L = D["ladder"]
    T = D["tuned"]
    rows = [
        ("教科书三重循环", L["naive"]["gf"], "ijk 顺序，B 按列跳着读"),
        ("换循环顺序 ikj", L["ikj"]["gf"], "一行 A 乘遍整个 B，连续访存"),
        ("4 行寄存器分块", L["t4"]["gf"], "AVX2 + FMA，4 个累加器"),
        ("8 行寄存器分块", L["t8"]["gf"], "8 个累加器，B 复用 8 次"),
        ("二级分块 + 调参", T["block_512_1"]["gf"], "MC128 / KC64 / NC256"),
        ("再加 3 线程", T["mt_512_3"]["gf"], "按行切，3 个 pthread"),
    ]
    W, H = 1360, 620
    im, d = canvas(W, H)
    d.text((56 * S, 34 * S), "同一道矩阵乘法，六次改写", font=F(30 * S, True), fill=INK)
    d.text((56 * S, 78 * S),
           "512×512×512 · 单核 i5-7300HQ · 每个版本都校验过误差（≤3e-6）· 多轮取最快",
           font=F(17 * S), fill=MUTED)

    x0, x1 = 250, 1150
    top, gap = 150, 62
    lo, hi = 0.5, 200.0

    def X(v):
        return x0 + (math.log10(v) - math.log10(lo)) / (math.log10(hi) - math.log10(lo)) * (x1 - x0)

    # 网格
    for t in (0.5, 1, 2, 5, 10, 20, 50, 100, 200):
        xx = X(t)
        d.line([xx * S, top * S, xx * S, (top + gap * len(rows) + 6) * S], fill=GRID, width=1 * S)
        lab = ("%g" % t)
        d.text((xx * S - tw(d, lab, F(13 * S)) / 2, (top + gap * len(rows) + 14) * S), lab,
               font=F(13 * S), fill=MUTED)
    d.text(((x0 + x1) / 2 - 40, (top + gap * len(rows) + 44) * S), "GFLOPS（对数轴）",
           font=F(14 * S), fill=MUTED)

    base = L["naive"]["gf"]
    for i, (name, v, sub) in enumerate(rows):
        y = top + i * gap
        d.text((56 * S, (y + 4) * S), name, font=F(18 * S, True), fill=INK)
        d.text((56 * S, (y + 28) * S), sub, font=F(13 * S), fill=MUTED)
        col = RED if i == 0 else (GREEN if i >= 4 else BLUE)
        xa = X(lo)
        xb = X(v)
        d.rectangle([xa * S, (y + 6) * S, xb * S, (y + 34) * S], fill=col)
        lab = "%.1f  ×%.0f" % (v, v / base) if i else "%.2f  ×1" % v
        d.text(((xb + 10) * S, (y + 10) * S), lab, font=F(16 * S, True), fill=col)

    # 峰值线
    xp = X(D["env"]["peak"])
    dash_v(d, xp, top - 24, top + gap * len(rows) - 8, ORANGE, 2)
    d.text((xp * S + 8, (top - 30) * S), "单核 FMA 峰值 %.0f" % D["env"]["peak"],
           font=F(14 * S, True), fill=ORANGE)

    im.save(os.path.join(HERE, "fig22_ladder.png"))
    print("fig22_ladder.png", im.size)


# ---------------------------------------------------------------- 面板
def panel(d, x0, y0, x1, y1, tag, title):
    d.rectangle([x0 * S, y0 * S, (x1 - 1) * S, (y1 - 1) * S], outline=GRID, width=1 * S)
    d.text(((x0 + 16) * S, (y0 + 12) * S), tag, font=F(13 * S, True), fill=ORANGE)
    d.text(((x0 + 42) * S, (y0 + 11) * S), title, font=F(15 * S, True), fill=INK)


def axes(d, x0, y0, x1, y1):
    d.line([x0 * S, y0 * S, x0 * S, y1 * S], fill=MUTED, width=1 * S)
    d.line([x0 * S, y1 * S, x1 * S, y1 * S], fill=MUTED, width=1 * S)


# ---------------------------------------------------------------- 图 2：屋顶线 + 线程
def fig_roof():
    W, H = 1360, 640
    im, d = canvas(W, H)
    d.text((50 * S, 30 * S), "算术强度决定你能跑多快", font=F(30 * S, True), fill=INK)
    d.text((50 * S, 74 * S), "左：N=K=896 固定、只改 M；右：1024³ 加线程（同一颗核，只改线程数）",
           font=F(17 * S), fill=MUTED)

    # ---------------- 左
    panel(d, 48, 108, 658, 548, "A", "同一条线性层，批大小从 1 加到 512")
    px0, py0, px1, py1 = 150, 185, 620, 455
    axes(d, px0, py0, px1, py1)
    xmin, xmax, ymin, ymax = 0.4, 200.0, 0.6, 200.0

    def LX(v):
        return px0 + (math.log10(v) - math.log10(xmin)) / (math.log10(xmax) - math.log10(xmin)) * (px1 - px0)

    def LY(v):
        return py1 - (math.log10(v) - math.log10(ymin)) / (math.log10(ymax) - math.log10(ymin)) * (py1 - py0)

    for gx in (1, 10, 100):
        d.line([LX(gx) * S, py0 * S, LX(gx) * S, py1 * S], fill=GRID, width=1 * S)
        d.text((LX(gx) * S - tw(d, str(gx), F(13 * S)) / 2, (py1 + 8) * S), str(gx), font=F(13 * S), fill=MUTED)
    for gy in (1, 10, 100):
        d.line([px0 * S, LY(gy) * S, px1 * S, LY(gy) * S], fill=GRID, width=1 * S)
        d.text((px0 * S - 10 * S - tw(d, "%g" % gy, F(13 * S)), LY(gy) * S - 9 * S), "%g" % gy, font=F(13 * S), fill=MUTED)
    d.text(((px0 + px1) / 2 - 78, (py1 + 34) * S), "算术强度 FLOP/Byte", font=F(14 * S), fill=MUTED)
    d.text((px0 - 62, (py0 - 26) * S), "GFLOPS", font=F(13 * S), fill=MUTED)

    for bw, col in ((15.75, ORANGE), (22.0, GREEN)):
        last = None
        for k in range(0, 80):
            ai = xmin * (xmax / xmin) ** (k / 79.0)
            v = min(bw * ai, ymax)
            pt = (LX(ai), LY(v))
            if last:
                d.line([last[0] * S, last[1] * S, pt[0] * S, pt[1] * S], fill=col, width=2 * S)
            last = pt

    ypk = LY(D["env"]["peak"])
    dash_h(d, ypk, px0, px1, RED, 2)
    d.text(((px0 + 8) * S, (ypk - 22) * S), "单核峰值 %.0f GFLOPS" % D["env"]["peak"],
           font=F(13 * S, True), fill=RED)

    off = {1: (8, 6), 4: (9, -21), 16: (8, -21), 64: (9, -21), 256: (9, -22), 512: (2, 10)}
    for p in D["roof"]:
        x, y = LX(p["ai"]), LY(p["gf"])
        d.ellipse([(x - 5) * S, (y - 5) * S, (x + 5) * S, (y + 5) * S], fill=BLUE,
                  outline=(255, 255, 255), width=1 * S)
        dx, dy = off.get(p["M"], (8, -18))
        d.text(((x + dx) * S, (y + dy) * S), "M=%d" % p["M"], font=F(12 * S, True), fill=BLUE)

    # 图例
    lx, ly = px0 + 62, py1 - 72
    for i, (col, lab, dash) in enumerate(((ORANGE, "DRAM 15.75 GB/s", False),
                                          (GREEN, "L3 22 GB/s", False),
                                          (RED, "单核峰值", True))):
        yy = ly + i * 22
        if dash:
            dash_h(d, yy, lx, lx + 26, col, 2, 4)
        else:
            d.line([lx * S, yy * S, (lx + 26) * S, yy * S], fill=col, width=2 * S)
        d.text(((lx + 32) * S, (yy - 8) * S), lab, font=F(12 * S), fill=col)

    # ---------------- 右
    panel(d, 688, 108, 1312, 548, "B", "同一份代码，只加线程")
    qx0, qy0, qx1, qy1 = 790, 185, 1265, 455
    axes(d, qx0, qy0, qx1, qy1)
    th = D["thr"]
    keys = ["1", "2", "3", "4", "6", "8"]
    vmax = 115.0

    def QY(v):
        return qy1 - v / vmax * (qy1 - qy0)

    for gy in (0, 25, 50, 75, 100):
        d.line([qx0 * S, QY(gy) * S, qx1 * S, QY(gy) * S], fill=GRID, width=1 * S)
        d.text((qx0 * S - 10 * S - tw(d, str(gy), F(13 * S)), QY(gy) * S - 9 * S), "%d" % gy, font=F(13 * S), fill=MUTED)
    d.text((qx0 - 62, (qy0 - 26) * S), "GFLOPS", font=F(13 * S), fill=MUTED)
    d.text(((qx0 + qx1) / 2 - 24, (qy1 + 34) * S), "线程数", font=F(14 * S), fill=MUTED)

    slot = (qx1 - qx0) / len(keys)
    # 线性加速虚线（只画到 3 线程）
    d.line([qx0 * S, QY(th["1"]["gf"]) * S, (qx0 + slot * 3) * S, QY(min(th["1"]["gf"] * 3, vmax)) * S],
           fill=MUTED, width=2 * S)
    # 灰线为线性加速参考

    for i, k in enumerate(keys):
        v = th[k]["gf"]
        x = qx0 + i * slot + slot * 0.2
        wbar = slot * 0.58
        col = GREEN if k == "3" else BLUE
        d.rectangle([x * S, QY(v) * S, (x + wbar) * S, qy1 * S], fill=col)
        d.text((x * S + 4, (QY(v) - 22) * S), "%.0f" % v, font=F(14 * S, True), fill=col)
        d.text((x * S + wbar / 2 - 5, (qy1 + 8) * S), k, font=F(15 * S), fill=INK)

    d.text((qx0 * S, (qy1 + 56) * S), "3 线程最快；4 线程掉一成多——多出来的那个线程抢不到核，",
           font=F(14 * S), fill=INK)
    d.text((qx0 * S, (qy1 + 80) * S), "整组要等最慢的那个跑完。", font=F(14 * S), fill=INK)

    im.save(os.path.join(HERE, "fig22_roof.png"))
    print("fig22_roof.png", im.size)


# ---------------------------------------------------------------- 图 3：融合 + 低精度
def fig_detail():
    W, H = 1360, 640
    im, d = canvas(W, H)
    d.text((50 * S, 30 * S), "融合省的是内存，低精度省的是字节", font=F(30 * S, True), fill=INK)
    d.text((50 * S, 74 * S), "左：2048×2048，K 越小越接近纯访存；右：Qwen2.5-0.5B 的 896 维线性层",
           font=F(17 * S), fill=MUTED)

    # ---------------- 左：融合
    panel(d, 50, 110, 665, 556, "A", "GEMM + bias + GELU：三趟 vs 一趟")
    fx0, fy0, fx1, fy1 = 130, 180, 630, 452
    axes(d, fx0, fy0, fx1, fy1)
    f = [x for x in D["fusion"] if x["M"] == 2048]
    vmax = 95.0

    def FY(v):
        return fy1 - v / vmax * (fy1 - fy0)

    for gy in (0, 25, 50, 75):
        d.line([fx0 * S, FY(gy) * S, fx1 * S, FY(gy) * S], fill=GRID, width=1 * S)
        d.text((fx0 * S - 10 * S - tw(d, str(gy), F(13 * S)), FY(gy) * S - 9 * S), "%d" % gy, font=F(13 * S), fill=MUTED)
    d.text((fx0 - 62, (fy0 - 26) * S), "毫秒", font=F(14 * S), fill=MUTED)

    slot = (fx1 - fx0) / len(f)
    for i, p in enumerate(f):
        x = fx0 + i * slot + slot * 0.10
        wb = slot * 0.34
        d.rectangle([x * S, FY(p["sep"]) * S, (x + wb) * S, fy1 * S], fill=LBLUE)
        d.rectangle([(x + wb + 6) * S, FY(p["fused"]) * S, (x + 2 * wb + 6) * S, fy1 * S], fill=BLUE)
        d.text((x * S + wb + 4, (FY(p["fused"]) - 19) * S), "%.1f" % p["fused"], font=F(11 * S, True), fill=BLUE)
        d.text((x * S + 2, (fy1 + 8) * S), "K=%d" % p["K"], font=F(13 * S), fill=INK)
        g = p["gain"]
        d.text((x * S + 2, (fy1 + 28) * S), "×%.2f" % g, font=F(12 * S, True),
               fill=GREEN if g > 1.05 else MUTED)

    d.rectangle([fx0 * S, (fy1 + 58) * S, (fx0 + 12) * S, (fy1 + 70) * S], fill=LBLUE)
    d.text(((fx0 + 18) * S, (fy1 + 56) * S), "三趟，中间结果写回内存", font=F(13 * S), fill=MUTED)
    d.rectangle([(fx0 + 210) * S, (fy1 + 58) * S, (fx0 + 222) * S, (fy1 + 70) * S], fill=BLUE)
    d.text(((fx0 + 228) * S, (fy1 + 56) * S), "一趟", font=F(13 * S), fill=MUTED)

    # ---------------- 右：低精度
    panel(d, 695, 110, 1310, 556, "B", "同一层，Q8_0 与 fp32")
    qx0, qy0, qx1, qy1 = 800, 180, 1270, 452
    axes(d, qx0, qy0, qx1, qy1)
    q = {x["M"]: x for x in D["int8"]}

    def QY(v):
        return qy1 - (math.log10(max(v, 0.01)) + 2) / (math.log10(100) + 2) * (qy1 - qy0)

    for gy in (0.01, 0.1, 1, 10, 100):
        d.line([qx0 * S, QY(gy) * S, qx1 * S, QY(gy) * S], fill=GRID, width=1 * S)
        d.text((qx0 * S - 10 * S - tw(d, "%g" % gy, F(13 * S)), QY(gy) * S - 9 * S), "%g" % gy, font=F(13 * S), fill=MUTED)
    d.text((qx0 - 100, (qy0 - 26) * S), "毫秒（对数）", font=F(14 * S), fill=MUTED)

    fp = {p["M"]: p["ms"] for p in D["roof"]}
    pairs = [("M=1", "纯访存", q[1]["ms"], fp[1]),
             ("M=256", "算得动", q[256]["ms"], fp[256])]
    slot = (qx1 - qx0) / len(pairs)
    for i, (lab, sub, v8, v32) in enumerate(pairs):
        x = qx0 + i * slot + slot * 0.18
        wb = slot * 0.22
        d.rectangle([x * S, QY(v8) * S, (x + wb) * S, qy1 * S], fill=GREEN)
        d.rectangle([(x + wb + 10) * S, QY(v32) * S, (x + 2 * wb + 10) * S, qy1 * S], fill=ORANGE)
        d.text((x * S - 6, (QY(v8) - 20) * S), "%.3f" % v8, font=F(12 * S, True), fill=GREEN)
        d.text((x * S + wb + 6, (QY(v32) - 20) * S), "%.2f" % v32, font=F(12 * S, True), fill=ORANGE)
        d.text((x * S - 2, (qy1 + 8) * S), lab, font=F(14 * S, True), fill=INK)
        d.text((x * S - 2, (qy1 + 30) * S), sub, font=F(12 * S), fill=MUTED)

    d.rectangle([qx0 * S, (qy1 + 58) * S, (qx0 + 12) * S, (qy1 + 70) * S], fill=GREEN)
    d.text(((qx0 + 18) * S, (qy1 + 56) * S), "Q8_0 int8（1 字节/权重）", font=F(13 * S), fill=MUTED)
    d.rectangle([(qx0 + 250) * S, (qy1 + 58) * S, (qx0 + 262) * S, (qy1 + 70) * S], fill=ORANGE)
    d.text(((qx0 + 268) * S, (qy1 + 56) * S), "fp32（4 字节）", font=F(13 * S), fill=MUTED)

    im.save(os.path.join(HERE, "fig22_detail.png"))
    print("fig22_detail.png", im.size)


fig_ladder()
fig_roof()
fig_detail()
