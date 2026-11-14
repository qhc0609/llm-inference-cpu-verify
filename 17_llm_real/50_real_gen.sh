#!/usr/bin/env bash
# 50_real_gen.sh —— 真的让模型说话
#
# 前面的脚本测的是吞吐和内存。这一节只做一件事：
# 把模型对同一组问题的真实回答原样留下来。
#
# 为什么不能省：任何吞吐数字都能用循环空转伪造，
# 但一段接得住上下文、能自己算数、能写对 C 代码的中文回答伪造不了。
#
# 四个问题分别压四种能力：
#   fact   —— 常识性技术判断（会不会胡说）
#   reason —— 需要自己列算式（是不是在背模板）
#   code   —— 结构化成产物（token 序列是否连贯）
#   long   —— 长输入里的信息提取（上下文是否真的进了 KV cache）

set -u
. "$(dirname "$0")/lib.sh"

MODEL=${MODEL:-$Q4}
GEN=$OUT/gen
mkdir -p "$GEN"

ask() {
  local id=$1 prompt=$2
  local f=$GEN/$id.txt tg pp
  "$BIN/llama-cli" -m "$MODEL" -c 1024 -t 4 -n 220 --temp 0 -s 1234 -st \
      -p "$prompt" > "$f" 2>&1
  tg=$(grep -oE "Generation: *[0-9.]+ t/s" "$f" | grep -oE "[0-9.]+" | tail -1)
  pp=$(grep -oE "Prompt: *[0-9.]+ t/s" "$f" | grep -oE "[0-9.]+" | tail -1)
  res "gen|$id|pp_tps=${pp:-?}|tg_tps=${tg:-?}|bytes=$(stat -c%s "$f")"
  echo "--- $id 完成 ---"
}

ask fact   "用两句话解释：CPU 的 L1 缓存和内存，谁快、快多少、为什么。"
ask reason "一个模型有 5 亿参数，用 4 位量化。请估算它的权重文件大约多大，并写出你的计算过程。"
ask code   "写一个 C 函数，计算两个 float 数组的点积。只给代码和一行注释。"
ask long   "下面这段是 llama.cpp 的说明。读完后只用一句话回答：它在 CPU 上靠什么把矩阵乘法加速？
llama.cpp 的 ggml 后端会在启动时探测 CPU 支持的指令集。在 x86 上，如果检测到 AVX2 与 FMA，
量化权重会在内存里被重新排布成适合向量化乘加的分块布局，每次用 8 个 float 一起做乘加；
如果检测到 AVX512，则一次处理 16 个 float。在 ARM 上则使用 NEON 的 4 路 float 乘加。
所有后端都遵循同一个前提：权重只读一次，但要被反复取用，因此布局是否连续比指令数量更关键。"

echo "===== 生成结果目录 ====="
ls -la "$GEN" | awk 'NR>3{printf "%-14s %6d 字节\n", $9, $5}'
