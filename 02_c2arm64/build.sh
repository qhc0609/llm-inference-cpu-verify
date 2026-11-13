#!/usr/bin/env bash
# 02 · 同一份 C，两边编译，比较机器指令
# 对应文章：第 02 篇《从 C 代码到 ARM64 机器指令》
#
# 环境说明：本机 openEuler 24.03 是 x86-64 虚拟机，没有 ARM 硬件。
# aarch64 一侧用交叉编译器产生真实指令，用 QEMU 用户态执行。
# 因为 aarch64 glibc 在这个 qemu 下启动会崩，ARM 侧用 -nostdlib 纯系统调用。
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/02_c2arm64"

CS=/opt/arm64-sysroot
CROSS=""
for c in aarch64-linux-gnu-gcc aarch64-linux-gnueabi-gcc \
         /opt/cross/gcc_arm64le/bin/aarch64-linux-gnueabi-gcc; do
  if command -v "$c" >/dev/null 2>&1 || [ -x "$c" ]; then CROSS="$c"; break; fi
done
OBJDUMP=""
for o in aarch64-linux-gnu-objdump aarch64-linux-gnueabi-objdump \
         /opt/cross/gcc_arm64le/bin/aarch64-linux-gnueabi-objdump; do
  if command -v "$o" >/dev/null 2>&1 || [ -x "$o" ]; then OBJDUMP="$o"; break; fi
done
QEMU=""
for q in qemu-aarch64-static qemu-aarch64; do
  if command -v "$q" >/dev/null 2>&1; then QEMU="$q"; break; fi
done

echo "===== 0. 工具链 ====="
echo "CROSS   = ${CROSS:-未找到}"
echo "OBJDUMP = ${OBJDUMP:-未找到}"
echo "QEMU    = ${QEMU:-未找到}"

echo
echo "===== 1. x86-64 本机编译并执行 ====="
gcc -O2 -std=gnu11 -o dot.x86 dot.c || { echo "FAIL: x86 编译失败"; exit 2; }
./dot.x86 | tee res.x86.txt

echo
echo "===== 2. aarch64 交叉编译 ====="
if [ -z "$CROSS" ]; then echo "SKIP: 无交叉编译器"; exit 2; fi
"$CROSS" -O2 -std=gnu11 -static -nostdlib -no-pie -fno-stack-protector \
         -DBARE_METAL -o dot.aarch64 dot.c || { echo "FAIL: 交叉编译失败"; exit 2; }
file dot.aarch64

echo
echo "===== 3. aarch64 执行（QEMU 用户态）====="
if [ -n "$QEMU" ]; then
  "$QEMU" ./dot.aarch64 | tee res.aarch64.txt
  echo "qemu_rc=$?"
else
  echo "SKIP: 无 qemu-aarch64"
fi

echo
echo "===== 4. 两侧数值比对 ====="
if [ -f res.aarch64.txt ] && [ -s res.aarch64.txt ]; then
  if diff -q res.x86.txt res.aarch64.txt >/dev/null; then
    echo "PASS: x86-64 与 aarch64 输出完全一致（同一份 C，两种指令集）"
  else
    echo "FAIL: 不一致"
    diff res.x86.txt res.aarch64.txt
  fi
else
  echo "无法比对：aarch64 侧没有产出结果"
fi

echo
echo "===== 5. x86-64 的 dot 反汇编 ====="
objdump -d --no-show-raw-insn dot.x86 | awk '/<dot>:/,/^$/'

echo
echo "===== 6. aarch64 的 dot 反汇编 ====="
if [ -n "$OBJDUMP" ]; then
  "$OBJDUMP" -d --no-show-raw-insn dot.aarch64 | awk '/<dot>:/,/^$/'
else
  "$CROSS" -O2 -S -DBARE_METAL -o - dot.c | sed -n '/^dot:/,/\.size/p'
fi

echo
echo "===== 7. add_if：条件分支怎么编 ====="
echo "--- x86-64 ---"
objdump -d --no-show-raw-insn dot.x86 | awk '/<add_if>:/,/^$/'
echo "--- aarch64 ---"
if [ -n "$OBJDUMP" ]; then
  "$OBJDUMP" -d --no-show-raw-insn dot.aarch64 | awk '/<add_if>:/,/^$/'
fi
