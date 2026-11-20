# -*- coding: utf-8 -*-
import json, os, re, hashlib
from PIL import Image, ImageDraw, ImageFont

S = 2
INK, MUTED, GRID = (26, 32, 44), (110, 120, 134), (226, 232, 240)
RED, ORANGE, BLUE, GREEN = (176, 42, 55), (194, 84, 20), (30, 58, 138), (16, 120, 96)
BG = (255, 255, 255)

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "results21")


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


PATH_RE = re.compile(r"\s+\(([^)]*)\)\s*$")
ADDR_RE = re.compile(r"^[0-9a-fA-F]+\s+")


def norm(sym):
    sym = PATH_RE.sub("", sym).strip()
    sym = ADDR_RE.sub("", sym).strip()
    sym = re.sub(r"\+0x[0-9a-fA-F]+$", "", sym)
    sym = re.sub(r"\.(constprop|isra|part)\.\d+$", "", sym)
    return sym if sym and sym not in ("[unknown]", "0") else None


def load_folded(path):
    stacks, stack, total = {}, [], 0
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.rstrip("\n")
            if not line.strip():
                if stack:
                    k = ";".join(reversed(stack))
                    stacks[k] = stacks.get(k, 0) + 1
                    total += 1
                stack = []
            elif line.startswith("\t") or line.startswith("  "):
                s = norm(line)
                if s:
                    stack.append(s)
    if stack:
        k = ";".join(reversed(stack))
        stacks[k] = stacks.get(k, 0) + 1
        total += 1
    return stacks, total


class Node:
    __slots__ = ("name", "value", "kids")

    def __init__(self, n):
        self.name, self.value, self.kids = n, 0, {}


def build(stacks):
    root = Node("全部样本")
    for k, v in stacks.items():
        cur = root
        cur.value += v
        for p in k.split(";"):
            cur = cur.kids.setdefault(p, Node(p))
            cur.value += v
    return root


def col(name, hot):
    if hot:
        h = int(hashlib.md5(name.encode()).hexdigest()[:4], 16)
        return (200 + h % 40, 70 + h % 50, 45 + h % 40)
    h = int(hashlib.md5(name.encode()).hexdigest()[:4], 16)
    return (150 + h % 60, 175 + h % 50, 205 + h % 40)


def fig_flame():
    stacks, total = load_folded(os.path.join(RES, "perf.script"))
    root = build(stacks)
    W = 1760
    im, d = canvas(W, 1100)   # 先按大画布画，最后按实际内容裁切
    d.text((64 * S, 40 * S), "时间去哪了：Decode 一步的 CPU 火焰图", font=F(34 * S, True), fill=INK)
    d.text((64 * S, 90 * S),
           "openEuler 24.03 · perf record -g -e cpu-clock · 采样 8 268 次 · 横条宽度=占比，纵向=调用深度",
           font=F(18 * S), fill=MUTED)

    x0, y0, rw = 64, 150, W - 128
    fh = 34
    rows = []

    def walk(node, depth, x, w):
        rows.append((node.name, depth, x, w, node.value))
        cx = x
        for name, ch in sorted(node.kids.items(), key=lambda kv: -kv[1].value):
            cw = w * ch.value / node.value if node.value else 0
            walk(ch, depth + 1, cx, cw)
            cx += cw

    cx = x0
    for name, ch in sorted(root.kids.items(), key=lambda kv: -kv[1].value):
        cw = rw * ch.value / root.value if root.value else 0
        walk(ch, 0, cx, cw)
        cx += cw
    maxd = max(r[1] for r in rows) + 1

    drawn = 1
    for name, depth, x, w, v in rows:
        if w < 1.0:
            continue
        drawn = max(drawn, depth + 1)
        y = y0 + depth * fh
        pct = 100.0 * v / total
        hot = name in ("gemv_q8_0",)
        fill = col(name, hot)
        d.rectangle([x * S, y * S, (x + w) * S, (y + fh - 4) * S], fill=fill,
                    outline=(255, 255, 255) if not hot else (140, 30, 40), width=1 * S)
        if w > 60:
            label = name if len(name) * 8.2 < w else name[:max(int(w / 8.2) - 1, 2)] + "…"
            tc = (255, 255, 255) if hot else (20, 26, 36)
            d.text(((x + 6) * S, (y + 7) * S), label, font=F(15 * S), fill=tc)
            if w > 150:
                d.text(((x + w - 62) * S, (y + 7) * S), "%.1f%%" % pct, font=F(14 * S, True), fill=tc)

    # 右侧注释
    ly = y0 + min(drawn, 5) * fh + 24
    d.text((64 * S, ly * S), "读法：从下往上是一条完整调用链，同层的条越宽，说明时间越多花在这里。",
           font=F(17 * S), fill=INK)
    d.text((64 * S, (ly + 30) * S),
           "gemv_q8_0 独占 96.1% —— Decode 一步里，几乎全部时间都在做「权重 × 激活」这一个动作。",
           font=F(17 * S), fill=RED)
    d.text((64 * S, (ly + 60) * S),
           "attention 1.8%、silu_mul 0.3%、rms_norm 0.1%：其余算子加起来不到 3%，优化它们没有意义。",
           font=F(17 * S), fill=MUTED)
    H = int(min(ly + 96, 1100))
    im = im.crop((0, 0, W * S, H * S)).resize((W, H), Image.LANCZOS)
    im.save(os.path.join(HERE, "fig21_flame.png"))
    print("flame ok", total)


def fig_cmp():
    D = json.load(open(os.path.join(HERE, "med_perf2.json"), encoding="utf-8"))
    L, B, T = D["loads"], D["branch"], D["threads"]
    W, H = 1760, 700
    im, d = canvas(W, H)
    d.text((64 * S, 40 * S), "三把尺子量同一颗核：依赖、分支、几个核在干活", font=F(34 * S, True), fill=INK)
    d.text((64 * S, 92 * S), "openEuler 24.03 虚拟机实测 · 4 vCPU（i5-7300HQ 2.5 GHz）· L3 6 MiB · 每档 7 轮",
           font=F(18 * S), fill=MUTED)

    base, top = 560, 250
    span = base - top

    def bar(x, w, v, maxv, color, label, sub):
        h = span * v / maxv
        d.rectangle([x * S, (base - h) * S, (x + w) * S, base * S], fill=color)
        d.text(((x + w / 2) * S, (base - h - 12) * S), label, font=F(18 * S, True), fill=color, anchor="ms")
        d.text(((x + w / 2) * S, (base + 14) * S), sub, font=F(15 * S), fill=MUTED, anchor="ma")

    # A. 依赖链 vs 四路并行（中位）
    px = 64
    d.text((px * S, 158 * S), "A. 同样的乘法：依赖链 vs 四路并行", font=F(21 * S, True), fill=INK)
    d.text((px * S, 190 * S), "纵轴：执行一次 FMA 的周期（中位，越小越快）", font=F(15 * S), fill=MUTED)
    dep = L["dep"]["cyc_per_fma_med"]
    par = L["par4"]["cyc_per_fma_med"]
    mx = max(dep, par) * 1.32
    bar(px + 60, 130, dep, mx, RED, "%.2f" % dep, "依赖链")
    bar(px + 240, 130, par, mx, GREEN, "%.2f" % par, "四路并行")
    d.text(((px + 60 + 65) * S, (base + 44) * S), "每迭代 1 次", font=F(14 * S), fill=MUTED, anchor="ma")
    d.text(((px + 240 + 65) * S, (base + 44) * S), "每迭代 4 次", font=F(14 * S), fill=MUTED, anchor="ma")
    d.text((px * S, (base + 84) * S), "相差 %.1f 倍：芯片没变，只是把等待藏起来了" % (dep / par),
           font=F(16 * S, True), fill=INK)

    # B. 分支
    px = 600
    d.text((px * S, 158 * S), "B. 同一批数据：排不排序", font=F(21 * S, True), fill=INK)
    d.text((px * S, 190 * S), "纵轴：处理每个元素的周期", font=F(15 * S), fill=MUTED)
    sv = B["sorted"]["cyc_per_elem"]["min"]
    rv = B["random"]["cyc_per_elem"]["min"]
    mx = max(sv, rv) * 1.2
    bar(px + 60, 130, sv, mx, GREEN, "%.2f" % sv, "已排序")
    bar(px + 240, 130, rv, mx, RED, "%.2f" % rv, "随机")
    d.text(((px + 60 + 65) * S, (base + 44) * S), "分支可预测", font=F(14 * S), fill=MUTED, anchor="ma")
    d.text(((px + 240 + 65) * S, (base + 44) * S), "半数猜错", font=F(14 * S), fill=MUTED, anchor="ma")
    d.text((px * S, (base + 84) * S), "相差 %.1f 倍：每次猜错要赔上十几个周期" % (rv / sv),
           font=F(16 * S, True), fill=INK)

    # C. 有效并行度
    px = 1112
    d.text((px * S, 158 * S), "C. 加到 8 线程，几个核在干活", font=F(21 * S, True), fill=INK)
    d.text((px * S, 190 * S), "纵轴：有效并行度 = CPU 时间 ÷ 墙钟", font=F(15 * S), fill=MUTED)
    mx = 4.65
    y4 = base - span * 4.0 / mx
    for xx in range(int(px * S), int((px + 6 * 70) * S), 26):
        d.line([xx, y4 * S, xx + 16, y4 * S], fill=RED, width=3)
    d.text(((px + 6 * 70 - 4) * S, (y4 - 12) * S), "4 个 vCPU", font=F(13 * S, True), fill=RED, anchor="rs")
    bw, gap = 52, 18
    for i, th in enumerate(T):
        v = th["para_max"]
        t = th["threads"]
        x = px + i * (bw + gap)
        c = BLUE if v >= 3.0 else GREEN
        h = span * v / mx
        d.rectangle([x * S, (base - h) * S, (x + bw) * S, base * S], fill=c)
        d.text(((x + bw / 2) * S, (base - h - 10) * S), "%.1f" % v, font=F(14 * S, True), fill=c, anchor="ms")
        d.text(((x + bw / 2) * S, (base + 14) * S), "%d" % t, font=F(15 * S), fill=INK, anchor="ma")
    d.text((px * S, (base + 44) * S), "线程数（个）", font=F(14 * S), fill=MUTED)
    d.text((px * S, (base + 84) * S), "8 线程，仍然只有 3.5 个核在干活",
           font=F(16 * S, True), fill=INK)

    im = im.resize((W, H), Image.LANCZOS)
    im.save(os.path.join(HERE, "fig21_cmp.png"))
    print("cmp ok")


if __name__ == "__main__":
    fig_flame()
    fig_cmp()
