# -*- coding: utf-8 -*-
"""flame.py - 把 perf script 输出折叠成栈并画一张静态火焰图 SVG

用法: python3 flame.py perf.script out.svg [title]
不依赖 perl 的 FlameGraph 工具链，纯 Python 实现，便于在离线环境复现。
"""
import sys, re, html, hashlib

PATH_RE = re.compile(r"\s+\(([^)]*)\)\s*$")
ADDR_RE = re.compile(r"^[0-9a-fA-F]+\s+")


def norm(sym):
    sym = PATH_RE.sub("", sym).strip()
    sym = ADDR_RE.sub("", sym).strip()
    sym = re.sub(r"\+0x[0-9a-fA-F]+$", "", sym)
    if not sym or sym in ("[unknown]", "0"):
        return "[unknown]"
    return sym


def parse(path):
    stacks = {}
    stack = []
    value = 0
    total = 0
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            line = raw.rstrip("\n")
            if not line.strip():
                if stack:
                    key = ";".join(reversed(stack))
                    stacks[key] = stacks.get(key, 0) + 1
                    total += 1
                stack = []
                continue
            if line.startswith("\t") or line.startswith(" "):
                stack.append(norm(line))
            else:
                # 事件头行，忽略（其中可能自带一个符号）
                pass
        if stack:
            key = ";".join(reversed(stack))
            stacks[key] = stacks.get(key, 0) + 1
            total += 1
    return stacks, total


class Node:
    __slots__ = ("name", "value", "children")

    def __init__(self, name):
        self.name = name
        self.value = 0
        self.children = {}


def build(stacks):
    root = Node("all")
    for k, v in stacks.items():
        parts = k.split(";")
        cur = root
        cur.value += v
        for p in parts:
            if p not in cur.children:
                cur.children[p] = Node(p)
            cur = cur.children[p]
            cur.value += v
    return root


def color(name):
    h = int(hashlib.md5(name.encode()).hexdigest()[:6], 16)
    r = 190 + (h & 0x3F)
    g = 150 + ((h >> 6) & 0x5F)
    b = 120 + ((h >> 12) & 0x4F)
    return "#%02x%02x%02x" % (min(r, 255), min(g, 255), min(b, 255))


def render(root, out, title, width=1240, fh=17, pad=6):
    total = root.value or 1
    rows = []

    def walk(node, depth, x0, w):
        rows.append((node.name, depth, x0, w, node.value))
        y = depth
        cx = x0
        for name, ch in sorted(node.children.items(), key=lambda kv: -kv[1].value):
            cw = w * ch.value / node.value if node.value else 0
            walk(ch, depth + 1, cx, cw)
            cx += cw

    walk(root, 0, 0, width - 2 * pad)
    maxd = max(r[1] for r in rows) + 1
    H = maxd * fh + 40

    parts = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d" '
        'font-family="Consolas,Menlo,monospace">' % (width + 2 * pad, H, width + 2 * pad, H),
        '<rect width="100%%" height="100%%" fill="#ffffff"/>',
        '<text x="%d" y="14" font-size="12" fill="#333">%s</text>' % (pad, html.escape(title)),
    ]
    for name, depth, x0, w, val in rows:
        if w < 1.2:
            continue
        y = 22 + depth * fh
        pct = 100.0 * val / total
        parts.append(
            '<g><rect x="%.1f" y="%d" width="%.1f" height="%d" fill="%s" stroke="#fff" stroke-width="0.5"/>'
            % (pad + x0, y, max(w - 0.6, 0.6), fh - 2, color(name)))
        if w > 46:
            label = name if len(name) < int(w / 6.2) else name[:max(int(w / 6.2) - 1, 1)] + "…"
            parts.append('<text x="%.1f" y="%d" font-size="10.5" fill="#1a1a1a">%s</text>'
                         % (pad + x0 + 3, y + 11, html.escape(label)))
            if w > 110:
                parts.append('<text x="%.1f" y="%d" font-size="9" fill="#444">%.1f%%</text>'
                             % (pad + x0 + 3 + 6.2 * len(label) + 6, y + 11, pct))
        parts.append("</g>")
    parts.append("</svg>")
    open(out, "w", encoding="utf-8").write("\n".join(parts))
    return len(rows), total


if __name__ == "__main__":
    src = sys.argv[1] if len(sys.argv) > 1 else "perf.script"
    dst = sys.argv[2] if len(sys.argv) > 2 else "flame.svg"
    ttl = sys.argv[3] if len(sys.argv) > 3 else "CPU 火焰图（perf 采样，-g fp）"
    st, tot = parse(src)
    root = build(st)
    n, t = render(root, dst, ttl)
    print("frames=%d samples=%d svg=%s" % (n, t, dst))
    # 顺带输出 top 自身耗时（按叶子聚合）
    leaf = {}
    for k, v in st.items():
        leaf[k.split(";")[-1]] = leaf.get(k.split(";")[-1], 0) + v
    for k, v in sorted(leaf.items(), key=lambda kv: -kv[1])[:12]:
        print("LEAF|%s|%d|%.1f%%" % (k, v, 100.0 * v / max(t, 1)))
