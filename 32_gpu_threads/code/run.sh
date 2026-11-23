#!/bin/bash
# A10 上跑全部实验，输出存到 raw/
set -e
cd "$(dirname "$0")"
mkdir -p raw
nvcc -O3 -arch=native -o gpu02 gpu02.cu 2>&1 | tee raw/build.log
for m in dev map lane smid div ballot occ tail red launch sched; do
  ./gpu02 $m > raw/r_$m.txt 2>&1
  echo "done $m"
done
echo "ALL_DONE"
