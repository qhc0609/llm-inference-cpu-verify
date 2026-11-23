import re, sys, os

def load(name):
    p = os.path.join('C:/Users/Administrator/WorkBuddy/2026-11-14-01-50-47', name)
    return open(p, encoding='utf-8').read()

def sections(html):
    # split on <h2 ...>...</h2> keeping order
    idx = [(m.start(), re.sub('<[^>]+>', '', m.group(0))) for m in re.finditer(r'<h2[^>]*>.*?</h2>', html, flags=re.S)]
    out = []
    for i, (pos, title) in enumerate(idx):
        end = idx[i + 1][0] if i + 1 < len(idx) else len(html)
        out.append((title.strip(), html[pos:end]))
    return out

if __name__ == '__main__':
    f = sys.argv[1]
    key = sys.argv[2] if len(sys.argv) > 2 else None
    mode = sys.argv[3] if len(sys.argv) > 3 else 'list'
    html = load(f)
    for title, block in sections(html):
        if key is None:
            print('%6d  %s' % (len(block), title))
        elif key in title:
            print(block if mode == 'dump' else block[:3000])
            break
