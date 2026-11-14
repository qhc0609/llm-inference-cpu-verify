#!/usr/bin/env bash
# 60_stability.sh —— 虚拟机上的测量到底有多可信
#
# 起因：同一份模型、同样 4 线程的 pp512，
#       在线程实验里测到 199.79 t/s，在长度扫描里测到 166.26 t/s，差 17%。
# 这不是笔误，是两台时间上隔开几十秒的进程。
#
# 所以这里干一件事：同一条命令，独立跑 5 次，把每次的 pp512 / tg128 都留下。
# 看得见的是"跑分能不能复现"，看不见的是这台 VM 是宿主机分时给的 CPU。

set -u
. "$(dirname "$0")/lib.sh"

HERE=$(cd "$(dirname "$0")" && pwd)
MODEL=${MODEL:-$Q4}
echo "模型 $(basename "$MODEL")"

for i in 1 2 3 4 5; do
  "$BIN/llama-bench" -m "$MODEL" -p 512 -n 128 -t 4 -r 3 -o json 2>/dev/null \
      > "$OUT/stab_$i.json"
  python3 "$HERE/bench_parse.py" scan "$OUT/stab_$i.json" "stab_run$i"
done

# 顺手把 5 次的极差算出来，省得读者自己减
python3 - "$OUT" <<'PY'
import glob, json, os, sys
out = sys.argv[1]
vals = {"pp512": [], "tg128": []}
for f in sorted(glob.glob(os.path.join(out, "stab_*.json"))):
    for r in json.load(open(f)):
        t = "pp512" if r.get("n_prompt") else "tg128"
        vals[t].append(r["avg_ts"])
for k, v in vals.items():
    if not v:
        continue
    print("RES|stab|%s|n=%d|min=%.2f|max=%.2f|spread_pct=%.1f"
          % (k, len(v), min(v), max(v), (max(v) - min(v)) / min(v) * 100))
PY
