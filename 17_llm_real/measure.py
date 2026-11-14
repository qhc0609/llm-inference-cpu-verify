#!/usr/bin/env python3
"""measure.py —— 跑一条命令，报告墙钟时间与"分类型"的峰值内存

为什么不用 /usr/bin/time：openEuler 最小安装里没有它，旧版脚本因此把 RSS 全写成 0.0。

为什么不能只看总 RSS：进程退出那一刻 VSZ/RSS 已经归零，而我们要的是"峰值"；
更重要的是，总 RSS 分不清哪些页是"映射的文件"、哪些是"堆上自己 malloc 的"。
这两类页的回收代价完全不同：
    RssFile —— 文件映射页。内核可以直接丢弃，需要时再从磁盘读回。
    RssAnon —— 匿名页。没有磁盘副本，只能写 swap 或者 OOM。

所以每 20 ms 读一次 /proc/<pid>/status（它的 RssAnon/RssFile 是权威值，
注意 smaps_rollup 里没有这两个字段，只有 Anonymous 和 Pss_Anon/Pss_File，别搞错）。

用法:
    python3 measure.py <stdout文件> <stderr文件> -- cmd args...
输出:
    MEASURE|rc=0|wall_s=1.234|rss_kb=峰值总RSS|rssanon_kb=..|rssfile_kb=..
"""
import subprocess
import sys
import time

WANT = {"VmRSS": "rss", "RssAnon": "anon", "RssFile": "file", "RssShmem": "shmem"}


def read_status(pid):
    """返回 {rss, anon, file, shmem}，单位 KB；进程已退出返回 None"""
    try:
        out = {}
        with open("/proc/%d/status" % pid) as f:
            for line in f:
                k, _, v = line.partition(":")
                if k in WANT:
                    out[WANT[k]] = int(v.split()[0])
        return out or None
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        return None


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        return 2
    out_path, err_path = sys.argv[1], sys.argv[2]
    cmd = sys.argv[3:]
    if cmd and cmd[0] == "--":
        cmd = cmd[1:]

    t0 = time.time()
    with open(out_path, "wb") as fo, open(err_path, "wb") as fe:
        p = subprocess.Popen(cmd, stdout=fo, stderr=fe)
        peak = {"rss": 0, "anon": 0, "file": 0, "shmem": 0}
        while p.poll() is None:
            s = read_status(p.pid)
            if s:
                for k in peak:
                    if s.get(k, 0) > peak[k]:
                        peak[k] = s[k]
            time.sleep(0.02)
        rc = p.wait()
    wall = time.time() - t0

    print("MEASURE|rc=%d|wall_s=%.3f|rss_kb=%d|rssanon_kb=%d|rssfile_kb=%d"
          % (rc, wall, peak["rss"], peak["anon"], peak["file"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
