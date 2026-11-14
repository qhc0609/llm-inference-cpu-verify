#!/usr/bin/env bash
# 18_recheck —— 复核 6 条"被推翻"的结论，看哪些是真结论、哪些是我测错了
# 用法：bash run.sh            （在该目录下运行，结果写到 results/）
set -u
cd "$(dirname "$0")"
OUT=${OUT:-results}
mkdir -p "$OUT"

CC=${CC:-gcc}
CFLAGS="-O2 -std=gnu11 -Wall -Wextra -pthread"

for prog in thread_scale affinity_recheck memcpy_recheck; do
    src="$prog.c"
    [ -f "$src" ] || { echo "缺 $src"; exit 1; }
    echo "#### 编译 $prog"
    $CC $CFLAGS -o "$OUT/$prog" "$src" -lm || exit 1
done

echo "#### 运行"
for prog in thread_scale affinity_recheck memcpy_recheck; do
    echo "===== $prog ====="
    "$OUT/$prog" | tee "$OUT/$prog.txt"
    echo
done

grep -h '^RES|' "$OUT"/thread_scale.txt "$OUT"/affinity_recheck.txt \
    "$OUT"/memcpy_recheck.txt | sort -u > "$OUT/recheck_summary.txt"
echo "汇总行数: $(wc -l < "$OUT/recheck_summary.txt")"
