import os, re, json, statistics as st

D = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rerun_dsw')
def rd(n):
    p = os.path.join(D, n)
    return open(p, encoding='utf-8', errors='replace').read() if os.path.exists(p) else ''

def med(xs):
    return round(st.median(xs), 2) if xs else None

out = {}

# --- 24-B sustain (8 cols) ---
rows = []
for line in rd('secB_short.txt').splitlines():
    if line.startswith('##') or line.startswith('t_s') or line.startswith('###'):
        continue
    f = line.split(',')
    if len(f) == 8:
        rows.append((float(f[3]), float(f[4])))  # tok/s, cpu_us_per_token
out['B_short_tok'] = med([r[0] for r in rows])
out['B_short_us'] = med([r[1] for r in rows])

# --- 24-C / 24-D sustain2 (6 cols) ---
def sadj(name):
    tps, p10, p50 = [], [], []
    for line in rd(name).splitlines():
        if line.startswith('###') or line.startswith('t_s') or not line.strip():
            continue
        f = line.split(',')
        if len(f) == 6:
            tps.append(float(f[2])); p10.append(float(f[3])); p50.append(float(f[4]))
    return tps, p10, p50

tps, p10, p50 = sadj('secC2_single_p10.txt')
out['C_single'] = dict(tok=med(tps), p10=med(p10), p50=med(p50), n=len(p10))
tps2, p10b, p50b = sadj('secD2_quad_p10.txt')
out['D_quad'] = dict(tok=med(tps2), p10=med(p10b), p50=med(p50b), n=len(p10b))
if out['C_single']['tok'] and out['D_quad']['tok']:
    out['speedup_quad'] = round(out['D_quad']['tok'] / out['C_single']['tok'], 2)

# --- 24-E recovery ---
ts, mops = [], []
for line in rd('secE_recover.txt').splitlines():
    f = line.split(',')
    if len(f) == 2 and f[0].replace('.', '').isdigit():
        ts.append(float(f[0])); mops.append(float(f[1]))
out['E_recover'] = dict(n=len(mops), first=med(mops[:3]), mid=med(mops[len(mops)//2-1:len(mops)//2+2]), last=med(mops[-3:]))

# --- 24-F burst ---
rows = []
for line in rd('secF_burst.txt').splitlines():
    if line.startswith('###') or line.startswith('t_s'):
        continue
    f = line.split(',')
    if len(f) == 8:
        rows.append(tuple(float(x) for x in f))
on_tok = [r[3] for r in rows if r[3] > 1]
out['F_burst'] = dict(n=len(rows), tok_median=med(on_tok), tok_max=max(on_tok) if on_tok else None, tok_min=min(on_tok) if on_tok else None)

# --- 24-G share ---
share = {}
cur = None
for line in rd('secG_share.txt').splitlines():
    m = re.match(r'## k=(\d+)', line)
    if m:
        cur = int(m.group(1)); share[cur] = {'tok': [], 'us': []}
        continue
    f = line.split(',')
    if cur is not None and len(f) == 8:
        share[cur]['tok'].append(float(f[3])); share[cur]['us'].append(float(f[4]))
out['G_share'] = {('%d%%' % (100 // (k + 1))): dict(tok=med(v['tok']), us=med(v['us'])) for k, v in share.items()}

# --- ISA ---
isa = rd('sec0_isa.txt')
blocks = isa.split('## ')
for b in blocks:
    if b.startswith('-march=native'):
        v = [float(l.split(',')[4]) for l in b.splitlines() if len(l.split(',')) == 8]
        out['isa_native_us'] = med(v)
    if b.startswith('-march=x86-64-v3'):
        v = [float(l.split(',')[4]) for l in b.splitlines() if len(l.split(',')) == 8]
        out['isa_avx2_us'] = med(v)

# --- 25-A cache ---
cache = []
for line in rd('secA_cache.txt').splitlines():
    m = re.search(r'kb=(\d+).*ns_per_access=([\d.]+)', line)
    if m:
        cache.append((int(m.group(1)), float(m.group(2))))
out['cache'] = cache

# --- 25-B stream ---
stream = {}
for line in rd('secB_stream.txt').splitlines():
    m = re.search(r'mb=(\d+) mode=(\w+).*gbs=([\d.]+)', line)
    if m:
        stream.setdefault((int(m.group(1)), m.group(2)), []).append(float(m.group(3)))
out['stream'] = {('%dMB_%s' % k): med(v) for k, v in stream.items()}

# --- 25-C gemv ---
gemv = {}
for line in rd('secC_gemv.txt').splitlines():
    m = re.search(r'variant=(\S+).*gbs=([\d.]+)', line)
    if m:
        gemv.setdefault(m.group(1), []).append(float(m.group(2)))
out['gemv'] = {k: dict(med=med(v), n=len(v), mn=min(v), mx=max(v)) for k, v in gemv.items()}

# --- 25-D gemm ---
gemm = {}
for line in rd('secD_gemm.txt').splitlines():
    m = re.search(r'variant=(\S+) n=(\d+).*gflops=([\d.]+) checksum=(\S+)', line)
    if m:
        gemm.setdefault((int(m.group(2)), m.group(1)), []).append((float(m.group(3)), m.group(4)))
out['gemm'] = {('%d_%s' % k): dict(med=med([x[0] for x in v]), n=len(v), cks=sorted(set(x[1] for x in v))) for k, v in gemm.items()}

# --- 25-E affinity ---
aff = {}
cur = None
for line in rd('secE_affinity.txt').splitlines():
    if line.startswith('##'):
        cur = line.strip('# ').strip(); aff[cur] = []
        continue
    m = re.search(r'gbs=([\d.]+)', line)
    if cur and m:
        aff[cur].append(float(m.group(1)))
out['affinity'] = {k: dict(med=med(v), n=len(v), mn=min(v), mx=max(v), spread=round(max(v)/min(v), 2)) for k, v in aff.items() if v}

# --- 25-F before/after ---
ba = {}
cur = None
for line in rd('secF_before_after.txt').splitlines():
    if line.startswith('##'):
        cur = line.strip('# ').strip(); ba[cur] = []
        continue
    m = re.search(r'gbs=([\d.]+)', line)
    if cur and m:
        ba[cur].append(float(m.group(1)))
out['before_after'] = {k: med(v) for k, v in ba.items() if v}

# --- 25-G regress ---
reg = [float(m) for m in re.findall(r'gemv 32 avx2x4.*?gbs=([\d.]+)', '')]
lines = rd('secG_regress.txt').splitlines()
r1 = [float(re.search(r'gbs=([\d.]+)', l).group(1)) for l in lines[:10] if 'gbs=' in l]
out['regress10'] = dict(n=len(r1), med=med(r1), mn=min(r1) if r1 else None, mx=max(r1) if r1 else None,
                        spread=round(max(r1) / min(r1), 2) if r1 else None)
out['regress_tail'] = [l for l in lines if l.startswith('GEMM') or l.startswith('##')]

json.dump(out, open(os.path.join(D, 'stats.json'), 'w'), ensure_ascii=False, indent=1)
print(json.dumps(out, ensure_ascii=False, indent=1))
