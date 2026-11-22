#!/usr/bin/env bash
# GPU 第 01 篇 · A10 微架构实测 · 一键复现
# 环境：Ubuntu 22.04 + CUDA 13.0 + torch 2.13.0+cu130 + nvidia-smi 570.211.01
set -x

mkdir -p /tmp/gpu01 && cd /tmp/gpu01 || exit 1
cp "$(dirname "$0")"/gpu01.cu .
cp "$(dirname "$0")"/bench.py .
cp "$(dirname "$0")"/coop.py .
cp "$(dirname "$0")"/cpugpu.py .

# 1) 硬件身份
{ echo '### nvidia-smi -L'; nvidia-smi -L
  echo; echo '### query'
  nvidia-smi --query-gpu=name,compute_cap,memory.total,memory.used,clocks.max.sm,clocks.max.mem,\
power.limit,power.max_limit,temperature.gpu,pcie.link.gen.max,pcie.link.width.max,driver_version,vbios_version \
    --format=csv
  echo; echo '### nvcc'; nvcc --version | tail -3
} > r_e1.txt 2>&1
nvidia-smi -q > r_smiq.txt 2>&1
nvidia-smi topo -m >> r_env.txt 2>&1

# 2) 微基准
nvcc -O3 -arch=sm_86 -o gpu01 gpu01.cu || exit 1
for m in dev chase bw fma div occ pcie; do
  ./gpu01 "$m" > "r_$m.txt" 2>&1
done

# 3) 上层：精度、发射开销、decode、协同
python3 bench.py  > r_torch.txt 2>&1
python3 coop.py   > r_coop.txt  2>&1

# 4) 同机 CPU 对照
python3 cpugpu.py > r_cpugpu.txt 2>&1

echo 'ALL DONE'
ls -l r_*.txt
