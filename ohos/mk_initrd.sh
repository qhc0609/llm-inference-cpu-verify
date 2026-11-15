#!/bin/bash
# 把编译好的 24 个二进制 + init 打进一个 cpio.gz 的 initramfs
#
# 用法：BIN=./bin INIT=./init bash ohos/mk_initrd.sh ./initrd.img
set -uo pipefail

BIN=${BIN:-$(pwd)/bin}
INIT=${INIT:-$(pwd)/init}
OUT=${1:-$(pwd)/initrd.img}

[ -x "$INIT" ] || { echo "找不到静态 init: $INIT（见 ohos/initramfs/init.c）"; exit 2; }

ROOT=$(mktemp -d)
mkdir -p "$ROOT/bin" "$ROOT/dev" "$ROOT/proc" "$ROOT/sys" "$ROOT/tmp"
cp "$INIT" "$ROOT/init"
cp "$BIN"/* "$ROOT/bin/" 2>/dev/null
rm -f "$ROOT"/bin/*.log
chmod +x "$ROOT"/bin/*

( cd "$ROOT" && find . -print0 | cpio --null -o -H newc 2>/dev/null | gzip -1 > "$OUT" )
rc=$?
rm -rf "$ROOT"

ls -lh "$OUT"
echo "包含 $(ls -1 "$BIN" | grep -vc '\\.log$') 个程序"
exit $rc
