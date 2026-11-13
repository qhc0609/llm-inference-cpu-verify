/* 15 · CPU 并行计算（OpenMP / 锁 / 线程池）
 * 对应文章：第 15 篇《CPU 并行计算》
 *
 * 三个实验，第三个是最容易看走眼的：
 *   A. 计算密集型：工作集很小（塞得进 L2），反复算，加线程应该接近线性
 *   B. 内存密集型：同样的代码，工作集 960MB，加线程基本没用
 *      ——因为瓶颈不在核，在内存带宽，而带宽是所有核共享的
 *   C. 假共享：4 个线程各加各的计数器，代码一样，只是内存布局差 64 字节，
 *      速度差好几倍，而且这个原因在源码里完全看不见
 */
#include "common.h"
#include <omp.h>
#include <pthread.h>

#define NTH 4

/* ---- 假共享版：计数器紧挨着 ---- */
static long counters_adjacent[NTH];
static pthread_barrier_t bar1;

static void *bump_adjacent(void *arg) {
    long i = (long)arg;
    pthread_barrier_wait(&bar1);
    for (long k = 0; k < 15000000L / NTH; k++) {
        counters_adjacent[i]++;
        /* 这个空 asm 是关键：没有它，编译器会把整个循环折叠成
         * 一条 "v += 3750000"，测出来的就不是假共享了 */
        __asm__ __volatile__("" ::: "memory");
    }
    return NULL;
}

/* ---- 消除假共享版：每个计数器独占一条缓存行 ---- */
typedef struct { long v; char pad[64 - sizeof(long)]; } PaddedCounter;
static PaddedCounter counters_padded[NTH];

static void *bump_padded(void *arg) {
    long i = (long)arg;
    pthread_barrier_wait(&bar1);
    for (long k = 0; k < 15000000L / NTH; k++) {
        counters_padded[i].v++;
        __asm__ __volatile__("" ::: "memory");
    }
    return NULL;
}

int main(void) {
    int ncpu = omp_get_num_procs();
    RES("omp_procs=%d", ncpu);

    /* ================= A. 计算密集型 =================
     * 数组 64K 个 double = 512KB，两组合计 1MB，正好压在 L2(256K) 外沿、
     * 但远小于 L3(6MB)，所以数据基本不落到内存，瓶颈在运算不在带宽。 */
    const long SMALL = 65536;
    double *sa = (double *)xmalloc((size_t)SMALL * sizeof(double));
    double *sb = (double *)xmalloc((size_t)SMALL * sizeof(double));
    for (long i = 0; i < SMALL; i++) { sa[i] = 1.0; sb[i] = 2.0; }
    const int SMALL_REPEAT = 400;

    double t1c = 0;
    for (int nt = 1; nt <= ncpu && nt <= 8; nt++) {
        omp_set_num_threads(nt);
        double best = 1e30;
        for (int r = 0; r < 3; r++) {
            double t0 = now_ns();
            double s = 0;
            #pragma omp parallel for reduction(+:s) schedule(static)
            for (int rep = 0; rep < SMALL_REPEAT; rep++)
                for (long i = 0; i < SMALL; i++) {
                    sa[i] = sa[i] * 1.0000001 + sb[i];
                    s += sa[i];
                }
            double dt = now_ns() - t0;
            if (dt < best) best = dt;
            if (s < 0) printf("x");
        }
        double ms = best / 1e6;
        if (nt == 1) t1c = ms;
        printf("OMP|compute  threads=%d  %8.2f ms  speedup=%.2fx  eff=%.0f%%\n",
               nt, ms, t1c / ms, (t1c / ms) / nt * 100);
        RES("omp_compute_threads%d=%.2f|ms|speedup=%.2f|eff_pct=%.0f",
            nt, ms, t1c / ms, (t1c / ms) / nt * 100);
    }

    /* ================= B. 内存密集型 =================
     * 数组 4000 万 double = 320MB × 2 = 640MB，每次迭代读写近 1GB。
     * 这个负载的瓶颈是内存带宽，不是核数。 */
    const long BIG = 40000000L;
    double *ba = (double *)xmalloc((size_t)BIG * sizeof(double));
    double *bb = (double *)xmalloc((size_t)BIG * sizeof(double));
    for (long i = 0; i < BIG; i++) { ba[i] = 1.0; bb[i] = 2.0; }

    double t1m = 0;
    for (int nt = 1; nt <= ncpu && nt <= 8; nt++) {
        omp_set_num_threads(nt);
        double best = 1e30;
        for (int r = 0; r < 2; r++) {
            double t0 = now_ns();
            double s = 0;
            #pragma omp parallel for reduction(+:s) schedule(static)
            for (long i = 0; i < BIG; i++) {
                ba[i] = ba[i] * 1.0000001 + bb[i];
                s += ba[i];
            }
            double dt = now_ns() - t0;
            if (dt < best) best = dt;
            if (s < 0) printf("x");
        }
        double ms = best / 1e6;
        if (nt == 1) t1m = ms;
        double gbs = (double)BIG * 24.0 / (best);   /* 读2写1，每个元素 24 字节 */
        printf("OMP|memcpy   threads=%d  %8.2f ms  speedup=%.2fx  bw=%.1f GB/s\n",
               nt, ms, t1m / ms, gbs);
        RES("omp_memory_threads%d=%.2f|ms|speedup=%.2f|bw_gbs=%.1f",
            nt, ms, t1m / ms, gbs);
    }
    RES("omp_contrast=compute_vs_memory|并行只对算得多的有效|对搬得多的无效");

    /* ================= C. 假共享 ================= */
    pthread_t th[NTH];
    pthread_barrier_init(&bar1, NULL, NTH);

    double t0 = now_ns();
    for (long i = 0; i < NTH; i++) pthread_create(&th[i], NULL, bump_adjacent, (void *)i);
    for (int i = 0; i < NTH; i++) pthread_join(th[i], NULL);
    double ms_adj = (now_ns() - t0) / 1e6;

    t0 = now_ns();
    for (long i = 0; i < NTH; i++) pthread_create(&th[i], NULL, bump_padded, (void *)i);
    for (int i = 0; i < NTH; i++) pthread_join(th[i], NULL);
    double ms_pad = (now_ns() - t0) / 1e6;

    long total_adj = 0, total_pad = 0;
    for (int i = 0; i < NTH; i++) { total_adj += counters_adjacent[i]; total_pad += counters_padded[i].v; }

    printf("FS|adjacent   %8.2f ms\n", ms_adj);
    printf("FS|padded     %8.2f ms\n", ms_pad);
    RES("false_sharing_adjacent=%.2f|ms", ms_adj);
    RES("false_sharing_padded=%.2f|ms", ms_pad);
    RES("false_sharing_ratio=%.2f|x|padded_faster", ms_adj / ms_pad);
    RES("fs_check=adj:%ld|pad:%ld|should_equal", total_adj, total_pad);

    return 0;
}
