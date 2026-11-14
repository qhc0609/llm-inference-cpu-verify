#!/usr/bin/env bash
# 00_setup.sh —— 准备真实大模型推理环境
#
# 目标：不再用微基准"模拟"机制，而是真的把模型权重读进来、真的算 token。
# 机器没有 GPU，模型选 Qwen2.5-0.5B-Instruct（GGUF），推理引擎选 llama.cpp。
#
# 为什么是 0.5B：这台虚拟机是 4 vCPU / 7 GB 内存 / 只剩 6 GB 磁盘的笔记本 VM，
# 7B 模型即使 Q4 也要 4.4 GB 权重 + KV，跑得动但会被磁盘和内存逼到换页，
# 测出来的每个数字都是 I/O 噪声。0.5B 能完整放进内存，测的才是算力和带宽。

set -e
ROOT=${ROOT:-/root/llm}
LLAMA=$ROOT/llama.cpp
MODELS=$ROOT/models
mkdir -p "$ROOT" "$MODELS"

echo "=== [1/4] 机器 ==="
grep -m1 "model name" /proc/cpuinfo
printf "指令集: "
grep -o -m1 -E "avx512f|avx2|fma|f16c" /proc/cpuinfo | sort -u | tr '\n' ' '
echo
printf "内存: "; free -m | awk 'NR==2{print $2" MB 总，可用 "$7" MB"}'
printf "磁盘: "; df -h / | awk 'NR==2{print $4" 可用"}'

echo "=== [2/4] llama.cpp ==="
if [ ! -d "$LLAMA/.git" ]; then
  git clone --depth 1 https://github.com/ggml-org/llama.cpp "$LLAMA"
fi
cd "$LLAMA"
echo "commit: $(git rev-parse --short HEAD)  $(git log -1 --format=%cd --date=short)"

echo "=== [3/4] 编译（Release + 本机指令集）==="
cmake -B build -DCMAKE_BUILD_TYPE=Release -DGGML_NATIVE=ON -DLLAMA_CURL=OFF >/dev/null
cmake --build build --config Release -j"$(nproc)" \
      --target llama-cli llama-bench llama-perplexity 2>&1 | tail -5

for b in llama-cli llama-bench llama-perplexity; do
  printf "%-16s %s\n" "$b" "$(ls -la "$LLAMA/build/bin/$b" 2>/dev/null | awk '{print $5" 字节"}' || echo 缺失)"
done

echo "=== [4/4] 下载模型（ModelScope）==="
dl() {
  local f=$1 size
  if [ -s "$MODELS/$f" ]; then echo "已存在: $f"; return; fi
  echo "下载 $f ..."
  local t0; t0=$(date +%s)
  curl -fL --retry 3 --retry-delay 2 -o "$MODELS/$f.part" \
       "https://modelscope.cn/models/Qwen/Qwen2.5-0.5B-Instruct-GGUF/resolve/master/$f"
  mv "$MODELS/$f.part" "$MODELS/$f"
  local t1; t1=$(date +%s)
  size=$(( $(stat -c%s "$MODELS/$f") / 1048576 ))
  echo "  $f  $size MB  用时 $((t1-t0)) 秒  平均 $(( size / (t1-t0>0?t1-t0:1) )) MB/s"
}

for m in qwen2.5-0.5b-instruct-q4_k_m.gguf \
         qwen2.5-0.5b-instruct-q8_0.gguf \
         qwen2.5-0.5b-instruct-q2_k.gguf; do
  dl "$m"
done

echo "=== 完成 ==="
ls -la "$MODELS" | awk '{print $5, $9}'
