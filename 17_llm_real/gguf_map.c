/* gguf_map.c —— 对着真实模型文件看 mmap 到底做了什么
 *
 * 对应文章 16。llama.cpp 默认用 mmap 打开 gguf，但它的日志只告诉你
 * "load time"，不告诉你内核这边发生了什么。这个程序补上那一层：
 *
 *   1. 只 mmap、不碰数据      -> 几乎不占 RSS，也不缺页
 *   2. 顺序读完整个文件       -> 页错误次数 ≈ 文件大小 / 页大小
 *   3. 随机 4KB 读 10 万次    -> 页错误少，但每次都要查页表、跳地址
 *
 * 关键点：mmap 只是建立"虚拟地址到文件"的映射，物理页要等第一次访问
 * 才分配（缺页）。所以"模型加载完了"和"模型进内存了"是两件事。
 *
 * 编译：gcc -O2 -o gguf_map gguf_map.c
 * 用法：./gguf_map /root/llm/models/qwen2.5-0.5b-instruct-q8_0.gguf
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/resource.h>

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static long faults(long *minf, long *majf)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    long tot = ru.ru_minflt + ru.ru_majflt;
    if (minf) *minf = ru.ru_minflt;
    if (majf) *majf = ru.ru_majflt;
    return tot;
}

static long rss_kb(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    long size = 0, resident = 0;
    if (f) { if (fscanf(f, "%ld %ld", &size, &resident) != 2) resident = 0; fclose(f); }
    return resident * (sysconf(_SC_PAGESIZE) / 1024);
}

#define RES(fmt, ...) printf("RES|" fmt "\n", ##__VA_ARGS__)

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "用法: %s <model.gguf>\n", argv[0]); return 1; }
    const char *path = argv[1];

    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }
    struct stat st;
    fstat(fd, &st);
    size_t sz = (size_t)st.st_size;
    long pg = sysconf(_SC_PAGESIZE);

    printf("文件: %s\n", path);
    printf("大小: %.1f MB   页大小: %ld B   理论页数: %zu\n", sz / 1048576.0, pg, sz / pg);
    RES("file|size_mb|%.1f|pages|%zu|page_size|%ld", sz / 1048576.0, sz / pg, pg);

    /* ---------- 1. 只建立映射，不访问 ---------- */
    long m0, j0;
    faults(&m0, &j0);
    double t0 = now_ms();
    unsigned char *p = mmap(NULL, sz, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    double mmap_ms = now_ms() - t0;
    long m1, j1;
    faults(&m1, &j1);

    RES("mmap_only|time_ms|%.3f|rss_mb|%.1f|new_faults|%ld",
        mmap_ms, rss_kb() / 1024.0, (m1 + j1) - (m0 + j0));

    /* ---------- 2. 顺序读完 ---------- */
    faults(&m0, &j0);
    long rss_before = rss_kb();
    t0 = now_ms();
    volatile uint64_t sum = 0;
    for (size_t i = 0; i < sz; i += 4096) sum += p[i];
    double seq_ms = now_ms() - t0;
    faults(&m1, &j1);
    long seq_faults = (m1 + j1) - (m0 + j0);
    double seq_gbs = (sz / 1073741824.0) / (seq_ms / 1000.0);

    RES("seq_read|time_ms|%.1f|throughput_gbs|%.2f|page_faults|%ld|rss_mb|%.1f|rss_growth_mb|%.1f",
        seq_ms, seq_gbs, seq_faults, rss_kb() / 1024.0, (rss_kb() - rss_before) / 1024.0);

    /* ---------- 3. 随机 4KB 读 ---------- */
    const int N = 100000;
    size_t nblk = sz / 4096;
    faults(&m0, &j0);
    t0 = now_ms();
    uint64_t s2 = 0;
    unsigned int seed = 12345;
    for (int i = 0; i < N; i++) {
        seed = seed * 1103515245u + 12345u;
        size_t blk = (seed >> 8) % nblk;
        s2 += p[blk * 4096];
    }
    double rnd_ms = now_ms() - t0;
    faults(&m1, &j1);

    RES("rand_read|n|%d|total_ms|%.1f|per_us|%.2f|page_faults|%ld|seq_ratio|%.2f",
        N, rnd_ms, rnd_ms * 1000.0 / N, (m1 + j1) - (m0 + j0),
        (rnd_ms / N) / (seq_ms / (double)(sz / 4096)));

    /* ---------- 4. munmap 之后内存还回去了吗 ---------- */
    long rss_mapped = rss_kb();
    munmap(p, sz);
    close(fd);
    RES("after_munmap|rss_before_mb|%.1f|rss_after_mb|%.1f|freed_mb|%.1f",
        rss_mapped / 1024.0, rss_kb() / 1024.0, (rss_mapped - rss_kb()) / 1024.0);

    printf("(sink=%llu %llu，防止编译器把读操作优化掉)\n",
           (unsigned long long)sum, (unsigned long long)s2);
    return 0;
}
