/* 19 · 页缓存冷热 + 大页策略：这两个是"操作系统真能影响多少"的量尺
 *
 * 两件事：
 *   1) 同一份 256MB 文件，页缓存热着读 vs 丢掉页缓存再读，差多少倍；
 *   2) 同样 256MB 内存搬运，开透明大页 / 关透明大页，差多少。
 * 第 2 条尤其重要：它是内核策略能造成的真实差距的量级。
 * 如果内核策略这一档只能造出几十个百分点，那 3 倍、65% 那种差距
 * 就不可能来自"Windows 换成了 openEuler"。
 */
#include "common.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

static double read_file(const char *path, int drop) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open"); return -1; }
    struct stat st; fstat(fd, &st);
    size_t sz = (size_t)st.st_size;
    if (drop) posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);

    size_t buf_sz = 8u << 20;
    char *buf = (char *)xmalloc(buf_sz);
    double t0 = now_ns();
    size_t got = 0;
    while (got < sz) {
        ssize_t r = read(fd, buf, buf_sz);
        if (r <= 0) break;
        got += (size_t)r;
    }
    double dt = now_ns() - t0;
    free(buf); close(fd);
    return dt / 1e9;   /* 秒 */
}

/* 纯内存搬运：dst[i] = src[i]，按 8 字节走，gcc 会自动向量化 */
static double memcpy_bw(size_t bytes, int huge, int reps) {
    char *src = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char *dst = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (src == MAP_FAILED || dst == MAP_FAILED) { perror("mmap"); exit(1); }
    madvise(src, bytes, huge ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);
    madvise(dst, bytes, huge ? MADV_HUGEPAGE : MADV_NOHUGEPAGE);
    memset(src, 1, bytes);

    double best = 1e18;
    for (int r = 0; r < reps; r++) {
        double t0 = now_ns();
        memcpy(dst, src, bytes);
        double dt = now_ns() - t0;
        if (dt < best) best = dt;
    }
    if (dst[0] == 2) fprintf(stderr, "impossible\n");
    munmap(src, bytes); munmap(dst, bytes);
    /* 带宽按 读 + 写 两份算 */
    return 2.0 * bytes / best;
}

int main(void) {
    const char *path = "/root/audit/big.bin";
    const size_t SZ = 256u << 20;

    /* 造文件 */
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { perror("create"); return 1; }
    char *chunk = (char *)xmalloc(1u << 20);
    for (size_t done = 0; done < SZ; done += (1u << 20)) {
        if (write(fd, chunk, 1u << 20) != (1u << 20)) { perror("write"); return 1; }
    }
    fsync(fd); close(fd);

    double warm_best = 1e18, cold_best = 1e18;
    for (int r = 0; r < 5; r++) {
        double w = read_file(path, 0);
        if (w < warm_best) warm_best = w;
        double c = read_file(path, 1);
        if (c < cold_best) cold_best = c;
        RES("pagecache|round=%d|warm_ms=%.1f|cold_ms=%.1f|warm_gbps=%.2f|cold_gbps=%.2f",
            r, w * 1e3, c * 1e3, SZ / w / 1e9, SZ / c / 1e9);
    }
    RES("pagecache_result|warm_gbps=%.2f|cold_gbps=%.2f|ratio=%.2fx",
        SZ / warm_best / 1e9, SZ / cold_best / 1e9,
        (SZ / cold_best) / (SZ / warm_best));

    double thp_on = 0, thp_off = 0;
    for (int r = 0; r < 3; r++) {
        double a = memcpy_bw(SZ, 1, 5);
        double b = memcpy_bw(SZ, 0, 5);
        if (a > thp_on) thp_on = a;
        if (b > thp_off) thp_off = b;
        RES("thp_memcpy|round=%d|thp_on_gbps=%.2f|thp_off_gbps=%.2f", r, a, b);
    }
    RES("thp_memcpy_result|thp_on_gbps=%.2f|thp_off_gbps=%.2f|on_vs_off=%.2fx",
        thp_on, thp_off, thp_on / thp_off);

    unlink(path);
    return 0;
}
