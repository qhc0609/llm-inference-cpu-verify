#!/bin/bash
# 20a_topo.sh - what this box actually has for DMA and sharing.
set -u
K=$(uname -r)
echo "== kernel"; uname -a
echo "RES|topo|kernel|$K"
echo "== iommu"; dmesg 2>/dev/null | grep -iE "DMAR|IOMMU|Default domain" | head -5
D=$(dmesg 2>/dev/null | grep -oiE "Default domain type: [A-Za-z]+" | head -1 | awk '{print $4}')
echo "RES|topo|iommu_domain|${D:-unknown}"
echo "== iommu groups"; ls /sys/kernel/iommu_groups 2>/dev/null | wc -l
echo "RES|topo|iommu_groups|$(ls /sys/kernel/iommu_groups 2>/dev/null | wc -l)"
echo "== dmabuf core"; grep -E "CONFIG_DMA_SHARED_BUFFER|CONFIG_UDMABUF|CONFIG_DMABUF_SYSFS|CONFIG_DMA_HEAP" /proc/config.gz 2>/dev/null | head -5
if [ -r /proc/config.gz ]; then
  for k in CONFIG_DMA_SHARED_BUFFER CONFIG_UDMABUF CONFIG_DMABUF_SYSFS_STATS; do
    v=$(zcat /proc/config.gz | grep -E "^$k=" | head -1)
    echo "RES|topo|$k|${v:-unset}"
  done
fi
echo "RES|topo|dma_heap|$([ -d /dev/dma_heap ] && echo yes || echo no)"
echo "RES|topo|udmabuf|$([ -e /dev/udmabuf ] && echo yes || echo no)"
echo "RES|topo|dmabuf_sysfs|$([ -d /sys/kernel/dmabuf ] && echo yes || echo no)"
echo "== drm"; for f in /sys/class/drm/card*/device/uevent; do
  [ -r "$f" ] && { drv=$(grep -h DRIVER "$f"); echo "RES|topo|drm|$(dirname "$f" | xargs basename)|$drv"; }
done
echo "RES|topo|render_nodes|$(ls /dev/dri/renderD* 2>/dev/null | wc -l)"
echo "RES|topo|vmwgfx_ver|$(cat /sys/module/vmwgfx/version 2>/dev/null)"
echo "== cpu"; echo "RES|topo|cacheline|$(getconf LEVEL1_DCACHE_LINESIZE)"
echo "RES|topo|l1d|$(getconf LEVEL1_DCACHE_SIZE)"
echo "RES|topo|clflush|$(grep -qw clflush /proc/cpuinfo && echo yes || echo no)"
echo "RES|topo|clflushopt|$(grep -qw clflushopt /proc/cpuinfo && echo yes || echo no)"
echo "RES|topo|clwb|$(grep -qw clwb /proc/cpuinfo && echo yes || echo no)"
echo "RES|topo|cpus|$(nproc)"
echo "== memory"; echo "RES|topo|memtotal_mb|$(awk '/MemTotal/{print int($2/1024)}' /proc/meminfo)"
echo "RES|topo|thp|$(cat /sys/kernel/mm/transparent_hugepage/enabled)"
echo "RES|topo|pagesize|$(getconf PAGESIZE)"
export PATH=$PATH:/usr/sbin
echo "== numa"; numactl --hardware 2>/dev/null | head -8
