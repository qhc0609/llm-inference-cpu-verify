#!/usr/bin/env bash
# 05_corpus.sh —— 造一份固定的评测语料
#
# 为什么需要它：困惑度（perplexity）只有在"同一份文本"上比较才有意义。
# 换一份文本，PPL 会整体平移，量化之间的差距就看不出来了。
#
# 用什么文本：llama.cpp 自带的文档（README、docs/*.md）。
# 理由：这是真实的技术英语散文，不是随机字符串；而且它跟着源码一起来，
# 不需要额外联网，谁复现都是同一份字节。
#
# 不用它的绝对值下结论——0.5B 模型在这份语料上的 PPL 数字本身没意义，
# 有意义的是 Q2_K / Q4_K_M / Q8_0 三个之间的相对差距。

set -eu
. "$(dirname "$0")/lib.sh"

CORPUS=$OUT/corpus.txt
LIMIT=${LIMIT:-120000}   # 字节上限，控制单次 PPL 的运行时间

find "$LLAMA_DIR/docs" "$LLAMA_DIR" -maxdepth 2 -name "*.md" -type f 2>/dev/null \
  | sort -u | head -40 | while read -r f; do
      cat "$f"; echo
    done | head -c "$LIMIT" > "$CORPUS"

BYTES=$(stat -c%s "$CORPUS")
LINES=$(wc -l < "$CORPUS")
WORDS=$(wc -w < "$CORPUS")
res "corpus|bytes|$BYTES|lines|$LINES|words|$WORDS"
echo "语料: $CORPUS  $BYTES 字节 / $WORDS 词"
head -3 "$CORPUS"
