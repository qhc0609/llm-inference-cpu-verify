#!/bin/bash
# run_numa.sh - the whole NUMA experiment set, in the same discipline as the
# other labs: container paused, one benchmark at a time, warm-up + 3 rounds.
#
#   bash run_numa.sh results_numa
#
# Every number it prints is a RES| line, parsed later by med_numa.py.
set -u
cd "$(dirname "$0")"
OUT=${1:-results_numa}
mkdir -p "$OUT"
CC=${CC:-gcc}
CFLAGS=${CFLAGS:--O3 -march=native -std=gnu11}

restore() { docker unpause ohos-compile-env >/dev/null 2>&1; }
trap restore EXIT INT TERM
docker pause ohos-compile-env >/dev/null 2>&1 && sleep 2 || echo "(no compile container)"

k() { awk -v key="$1" '$1==key{print $2}' /proc/vmstat; }
expand() {
  python3 - "$1" <<'PY'
import sys, itertools
out = []
for part in sys.argv[1].strip().split(','):
    a, _, b = part.partition('-')
    out.append(range(int(a), int(b or a) + 1))
print(' '.join(str(x) for x in itertools.chain(*out)))
PY
}

echo "== topology =="
bash 19a_topo.sh | tee "$OUT/topo.txt" | grep '^RES|'

NN=$(ls -d /sys/devices/system/node/node[0-9]* 2>/dev/null | wc -l)
C0=$(expand "$(cat /sys/devices/system/node/node0/cpulist)")
echo "== nodes=$NN  node0 cpus: $C0 =="

echo "== build =="
for src in 19b_latency 19c_firsttouch 19d_migrate 19e_share 19f_decode 13_streambw; do
  # -ffast-math only matters for 19f: without it gcc refuses to vectorise the
  # float reduction and the GEMV turns compute-bound on 4 scalar accumulators
  $CC $CFLAGS -ffast-math "$src.c" -o "$src" -lpthread -lm || exit 1
done

run_vm() {  # run_vm <tag> <cmd...> : wrap a run and diff the kernel's numa_* counters
  local tag=$1; shift
  local h0=$(k numa_hit) m0=$(k numa_miss) f0=$(k numa_foreign)
  local i0=$(k numa_interleave) l0=$(k numa_local) o0=$(k numa_other)
  "$@" | tee -a "$OUT/vm_$tag.txt"
  local h1=$(k numa_hit) m1=$(k numa_miss) f1=$(k numa_foreign)
  local i1=$(k numa_interleave) l1=$(k numa_local) o1=$(k numa_other)
  printf "RES|vmstat|%s|hit|%d|miss|%d|foreign|%d|interleave|%d|local|%d|other|%d\n" \
    "$tag" $((h1-h0)) $((m1-m0)) $((f1-f0)) $((i1-i0)) $((l1-l0)) $((o1-o0))
}

if [ "$NN" -lt 2 ]; then
  echo "RES|nodes|$NN|NOTE|only one node: local/remote comparison impossible"
  ./19c_firsttouch | tee "$OUT/firsttouch.txt"
  ./13_streambw        | tee "$OUT/bw_default.txt"
  exit 0
fi

N1=$(expand "$(cat /sys/devices/system/node/node1/cpulist)")
A0=$(echo $C0 | awk '{print $1}'); A1=$(echo $C0 | awk '{print $2}')
B0=$(echo $N1 | awk '{print $1}'); B1=$(echo $N1 | awk '{print $2}')
echo "== cpus: node0=$A0,$A1 node1=$B0,$B1 =="

echo "== 1. latency: memory node x cpu node =="
for m in 0 1; do
  for c in 0 1; do
    run_vm "lat_m${m}c${c}" numactl --membind=$m --cpunodebind=$c ./19b_latency 64 "lat_m${m}c${c}"
  done
done
run_vm "lat_inter" numactl --interleave=all --cpunodebind=0 ./19b_latency 64 "lat_inter"

echo "== 2. bandwidth: same matrix, STREAM read =="
# one cpu node = 2 cpus here, so cap the sweep at 2 threads: a 4-thread line
# under --cpunodebind would be 4 threads fighting over 2 cores
for m in 0 1; do
  for c in 0 1; do
    echo "RES|comb|membind=$m|cpunode=$c"
    BW_MAXT=2 run_vm "bw_m${m}c${c}" numactl --membind=$m --cpunodebind=$c ./13_streambw
  done
done
echo "RES|comb|interleave"
BW_MAXT=2 run_vm "bw_inter" numactl --interleave=all --cpunodebind=0 ./13_streambw

# and the case that actually uses the whole machine: 4 cpus, memory either all
# on one node or spread by the interleave policy
echo "RES|comb|4cpu_membind0"
run_vm "bw_4cpu_m0" numactl --membind=0 --physcpubind=$A0,$A1,$B0,$B1 ./13_streambw
echo "RES|comb|4cpu_interleave"
run_vm "bw_4cpu_inter" numactl --interleave=all --physcpubind=$A0,$A1,$B0,$B1 ./13_streambw

echo "== 3. first touch: own / cross / single / cold =="
./19c_firsttouch | tee "$OUT/firsttouch.txt"
echo "RES|comb|firsttouch_interleave"
numactl --interleave=all ./19c_firsttouch | tee "$OUT/firsttouch_inter.txt"

echo "== 4. migration 0 -> 1 =="
run_vm "mig01" numactl --membind=0 ./19d_migrate 1 256
run_vm "mig10" numactl --membind=1 ./19d_migrate 0 256

echo "== 5. false sharing: same node vs cross node =="
./19e_share "$A0" "$A1" | sed 's/|packed|/|packed_same_node|/; s/|padded|/|padded_same_node|/' | tee "$OUT/share_same.txt"
./19e_share "$A0" "$B0" | sed 's/|packed|/|packed_cross_node|/; s/|padded|/|padded_cross_node|/' | tee "$OUT/share_cross.txt"

echo "== 6. decode: weights on node0, workers local vs split =="
# weights are 256 MiB and read once per token; a "node" here has 2 cpus, so
# "all local" means mem node0 + the 2 cpus of node0, and "split" means one
# worker from each node reading the same node0-resident weights
run_vm "dec_2local"  numactl --membind=0 --physcpubind=$A0,$A1 ./19f_decode 2
run_vm "dec_2split"  numactl --membind=0 --physcpubind=$A0,$B0 ./19f_decode 2
run_vm "dec_2remote" numactl --membind=1 --physcpubind=$A0,$A1 ./19f_decode 2
run_vm "dec_2inter"  numactl --interleave=all --physcpubind=$A0,$A1 ./19f_decode 2
run_vm "dec_4split"  numactl --membind=0 --physcpubind=$A0,$A1,$B0,$B1 ./19f_decode 4
run_vm "dec_4inter"  numactl --interleave=all --physcpubind=$A0,$A1,$B0,$B1 ./19f_decode 4

echo "== done =="
