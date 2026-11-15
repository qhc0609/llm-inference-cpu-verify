#!/bin/bash
# 镜像还没有时，用编好的内核 + 自带 initramfs 直接起 QEMU，把 24 组跑在 OpenHarmony 内核上
#
# 用法：
#   BIN=./bin INITRD=./initrd.img KERNEL=/path/to/bzImage bash ohos/qemu_boot.sh
#
# 依赖：qemu-kvm、能用的 /dev/kvm（没有 KVM 会退化成 TCG，延迟类数据全部失真，别用）
set -uo pipefail

KERNEL=${KERNEL:-/compile/ohos/out/x86_64_virt/packages/phone/images/bzImage}
INITRD=${INITRD:-/tmp/initrd.img}
BIN=${BIN:-$(pwd)/bin}
LOG=${LOG:-$(pwd)/oh_qemu.log}
MEM=${MEM:-4096}
SMP=${SMP:-4}

[ -r "$KERNEL" ] || { echo "找不到内核: $KERNEL"; exit 2; }
[ -r "$INITRD" ] || { echo "找不到 initrd: $INITRD"; exit 2; }
[ -e /dev/kvm ] || echo "警告：没有 /dev/kvm，将走 TCG，数据不可用"

qemu-kvm -m "$MEM" -smp "$SMP" -enable-kvm -cpu host \
    -kernel "$KERNEL" -initrd "$INITRD" \
    -append "console=ttyS0 rdinit=/init loglevel=4" \
    -nographic -no-reboot > "$LOG" 2>&1

echo "--- 结果在 $LOG ---"
grep -a '^RES|' "$LOG" | sort -u > "${LOG%.log}_res.txt"
grep -c '^RES|' "${LOG%.log}_res.txt"
