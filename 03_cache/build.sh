#!/usr/bin/env bash
# 03 · 编译 cache 延迟测试
# 对应文章：第 03 篇《CPU Cache 体系》
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CC=${CC:-gcc}
$CC -O2 -std=gnu11 -o "$ROOT/03_cache/cache_lat" "$ROOT/03_cache/cache_lat.c" -lm
echo "built: 03_cache/cache_lat"
