/* memcpy_recheck.c —— 带宽到底是不是单调递减
 *
 * 原 03_cache 的扫描里出现 256MB 比 32MB 还快，当时当成"反直觉结论"写了。
 * 更可能的解释是：每个尺寸的重复次数不同（iter 14 次 vs 92 次），
 * 再加上 256MB 触发了透明大页，两者混在一起。
 * 本程序固定"每次运行搬运的总字节数"，让每个尺寸的重复次数在同一量级，
 * 每个尺寸跑 5 次报最小值与中位数。
 */
#include "../common.h"

#define REP 5
#define TARGET_BYTES (2ULL * 1024 * 1024 * 1024)   /* 每个尺寸总搬运 2 GiB */

static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y);
}

int main(void) {
    size_t sizes[] = {32u << 10, 256u << 10, 2u << 20, 8u << 20,
                      32u << 20, 128u << 20, 256u << 20};
    int n = (int)(sizeof sizes / sizeof sizes[0]);

    RES("recheck|what=memcpy_bw|reps=%d|target_bytes_per_size=%llu",
        REP, (unsigned long long)TARGET_BYTES);

    double prev_bw = 0;
    for (int i = 0; i < n; i++) {
        size_t sz = sizes[i];
        long iters = (long)(TARGET_BYTES / sz);
        if (iters < 4) iters = 4;

        unsigned char *a = xmalloc(sz), *b = xmalloc(sz);
        /* 先摸一遍，排除首次缺页成本 */
        memcpy(b, a, sz);

        double t[REP];
        for (int r = 0; r < REP; r++) {
            double t0 = now_ns();
            for (long k = 0; k < iters; k++) memcpy(b, a, sz);
            t[r] = (now_ns() - t0) / 1e9;     /* 秒 */
        }
        qsort(t, REP, sizeof(double), cmp_d);

        /* 带宽按"读+写"算，即 2 倍字节数 */
        double bw_min = (double)sz * iters * 2.0 / t[0] / 1e9;
        double bw_med = (double)sz * iters * 2.0 / t[REP / 2] / 1e9;

        RES("bw|size_kb=%zu|iters=%ld|bw_min=%.2f|bw_med=%.2f|GBps|"
            "vs_prev_min_pct=%.1f",
            sz / 1024, iters, bw_min, bw_med,
            prev_bw > 0 ? (bw_min - prev_bw) / prev_bw * 100.0 : 0.0);
        prev_bw = bw_min;
        free(a); free(b);
    }
    printf("RES|note=%s\n",
           "size 每翻 4 倍，若带宽仍出现回升，才是真的非单调；否则是测量噪声");
    return 0;
}
