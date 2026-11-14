#!/usr/bin/env bash
# run_all.sh —— 依次跑完全部真模型实验，汇总所有 RES 行
#
# 环境要求（已经在这台机器上验证过）：
#   openEuler 24.03 LTS-SP4 / x86_64 / 4 vCPU / 7 GB / AVX2+FMA
#   llama.cpp 已编译（00_setup.sh），三个 GGUF 已在 /root/llm/models
#
# 用法：
#   bash run_all.sh                  # 全跑
#   bash run_all.sh 30 40            # 只跑编号以 30/40 开头的步骤
#
# 长时间运行请放后台：setsid nohup bash run_all.sh > /root/llm/run_all.log 2>&1 &

set -u
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-/root/llm/results}
mkdir -p "$OUT"
export OUT

step() {
  local name=$1 t0 t1
  echo "################ $name ################"
  t0=$(date +%s)
  bash "$HERE/$name" 2>&1 | tee "$OUT/${name%.sh}.txt"
  t1=$(date +%s)
  echo "RES|step_time|${name%.sh}|seconds=$((t1-t0))"
  echo
}

# 0. 环境快照：所有数字都必须在同一台机器、同一份二进制上才能横向比较
{
  echo "################ 00_env ################"
  echo "RES|env|cpu|$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//')"
  echo "RES|env|vcpu|$(nproc)"
  echo "RES|env|isa|$(grep -o -m1 -E 'avx512f|avx2|fma|f16c' /proc/cpuinfo | sort -u | tr '\n' '+')"
  echo "RES|env|mem_mb|$(free -m | awk 'NR==2{print $2}')"
  echo "RES|env|os|$(. /etc/os-release; echo "$PRETTY_NAME")"
  echo "RES|env|kernel|$(uname -r)"
  echo "RES|env|gcc|$(gcc -dumpfullversion 2>/dev/null || echo '?')"
  # 源码是 tarball 上传的，没有 .git，取不到 commit；退一步记二进制自报的版本号
  echo "RES|env|llama|$(/root/llm/llama.cpp/build/bin/llama-cli --version 2>&1 | head -1 | sed 's/^version: *//')"
} | tee "$OUT/00_env.txt"

WANT="${*:-}"
for s in 05_corpus.sh 10_load.sh 20_pp_tg.sh 30_quant.sh 40_ctx.sh 50_real_gen.sh; do
  if [ -n "$WANT" ]; then
    for w in $WANT; do
      case "$s" in "$w"*) step "$s"; break;; esac
    done
  else
    step "$s"
  fi
done

echo "################ 汇总 ################"
# 只收各步骤的 txt（gen/ 与 *.log 不参与），去重后按行排序
grep -h "^RES|" "$OUT"/[0-9][0-9]_*.txt 2>/dev/null | sort -u > "$OUT/llm_summary.txt"
wc -l < "$OUT/llm_summary.txt" | xargs echo "汇总行数:"
cat "$OUT/llm_summary.txt"
