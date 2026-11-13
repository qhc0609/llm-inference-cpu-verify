/* 16 · 大模型权重加载（GGUF / mmap / 缺页）
 * 对应文章：第 16 篇《大模型权重加载》
 *
 * 验证：
 *   1) mmap 一个 512MB 权重文件，实际上并不立刻占内存
 *      （RSS 不会涨，直到你真正读到那一页）
 *   2) 第一次读：触发缺页，慢；第二次读：页已在 page cache，快
 *   3) 顺序读 vs 随机读的差距（预读机制在起作用）
 *   4) madvise(MADV_WILLNEED) 提前把页拉进来，能削掉多少首次延迟
 *
 * 这就是 llm 启动时"第一个 token 特别慢"的物理原因。
 */
#include "common.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/resource.h>

#define FNAME "/tmp/verify_weights.bin"
#define SIZE  (512L * 1024 * 1024)      /* 512MB，模拟一个小模型的权重 */
#define PAGE  4096

static void make_file(void) {
    int fd = open(FNAME, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("open"); exit(1); }
    /* 用 1MB 缓冲循环写，别一次 malloc 512MB */
    char *buf = (char *)xmalloc(1 << 20);
    for (size_t i = 0; i < (1u << 20); i++) buf[i] = (char)(i * 7 + 3);
    for (long off = 0; off < SIZE; off += (1 << 20)) {
        if (write(fd, buf, 1 << 20) != (1 << 20)) { perror("write"); exit(1); }
    }
    fsync(fd);
    close(fd);
    free(buf);
}

static long faults_now(int which) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return which ? ru.ru_majflt : ru.ru_minflt;
}

int main(void) {
    make_file();

    struct stat st;
    stat(FNAME, &st);
    RES("weights_file_size=%.0f|MB", (double)st.st_size / 1048576.0);

    int fd = open(FNAME, O_RDONLY);
    if (fd < 0) { perror("open"); return 1; }

    long rss0 = vm_rss_kb();
    double t0 = now_ns();
    char *p = (char *)mmap(NULL, SIZE, PROT_READ, MAP_PRIVATE, fd, 0);
    double t_map = now_ns() - t0;
    if (p == MAP_FAILED) { perror("mmap"); return 1; }
    long rss1 = vm_rss_kb();

    RES("mmap_time=%.3f|ms", t_map / 1e6);
    RES("rss_before_mmap=%.1f|MB", rss0 / 1024.0);
    RES("rss_after_mmap=%.1f|MB|虚拟映射%.0fMB", rss1 / 1024.0, (double)SIZE / 1048576.0);

    /* ---------- 第一次读：缺页 ---------- */
    long mf0 = faults_now(0), jf0 = faults_now(1);
    volatile long sum = 0;
    t0 = now_ns();
    for (long off = 0; off < SIZE; off += PAGE) sum += p[off];
    double t_cold = now_ns() - t0;
    long mf1 = faults_now(0), jf1 = faults_now(1);

    RES("cold_read=%.1f|ms|effective_bw=%.2fGB/s",
        t_cold / 1e6, (double)SIZE / t_cold);
    RES("page_faults_minor=%ld", mf1 - mf0);
    RES("page_faults_major=%ld", jf1 - jf0);

    /* ---------- 第二次读：页已在 page cache ---------- */
    t0 = now_ns();
    for (long off = 0; off < SIZE; off += PAGE) sum += p[off];
    double t_warm = now_ns() - t0;

    RES("warm_read=%.1f|ms|effective_bw=%.2fGB/s", t_warm / 1e6, (double)SIZE / t_warm);
    RES("cold_vs_warm=%.1f|x", t_cold / t_warm);

    /* ---------- 随机读：破坏预读 ---------- */
    srand48(20261008u);
    t0 = now_ns();
    for (int i = 0; i < 100000; i++) {
        long off = (long)(drand48() * (double)(SIZE - PAGE)) & ~((long)PAGE - 1);
        sum += p[off];
    }
    double t_rand = now_ns() - t0;
    double seq_per = t_warm / (double)(SIZE / PAGE);
    double rand_per = t_rand / 100000.0;
    RES("random_access=%.1f|us_per_page", rand_per / 1000.0);
    RES("sequential_access=%.2f|us_per_page", seq_per / 1000.0);
    RES("rand_vs_seq=%.0f|x_slower", rand_per / seq_per);

    munmap(p, SIZE);
    close(fd);

    /* ---------- 阶段 2：MADV_WILLNEED 预取 ---------- */
    fd = open(FNAME, O_RDONLY);
    p = (char *)mmap(NULL, SIZE, PROT_READ, MAP_PRIVATE, fd, 0);
    t0 = now_ns();
    madvise(p, SIZE, MADV_WILLNEED);
    double t_advise = now_ns() - t0;
    t0 = now_ns();
    for (long off = 0; off < SIZE; off += PAGE) sum += p[off];
    double t_after_advise = now_ns() - t0;
    RES("madvise_willneed=%.1f|ms|cost", t_advise / 1e6);
    RES("read_after_madvise=%.1f|ms|vs_cold_%.1f", t_after_advise / 1e6, t_cold / 1e6);

    munmap(p, SIZE);
    close(fd);
    printf("# sink=%ld\n", sum);
    return 0;
}
