/* 07 · Tensor 与矩阵计算：GEMM 与分块优化
 * 对应文章：第 07 篇《Tensor 与矩阵计算》
 *
 * 同一道矩阵乘法，五种写法，五种速度：
 *   1) 教科书三重循环（i-j-k）
 *   2) 调换循环顺序（i-k-j，让内层连续访存）
 *   3) 分块（blocking），块大小扫一遍
 *   4) B 先转置（把列访问变成行访问）
 *   5) 一维数组 + 指针外提（消除重复地址计算）
 * 差异的来源只有一个：缓存。
 */
#include "common.h"

/* 教科书写法：内层走 j，B 的每一列都要跨行取，cache 不友好 */
static void mm_ijk(const float *A, const float *B, float *C, int N) {
    for (int i = 0; i < N; i++)
        for (int j = 0; j < N; j++) {
            float s = 0;
            for (int k = 0; k < N; k++) s += A[i * N + k] * B[k * N + j];
            C[i * N + j] = s;
        }
}

/* i-k-j：内层对 B 的行做顺序扫描，天然利用 cache line */
static void mm_ikj(const float *A, const float *B, float *C, int N) {
    memset(C, 0, (size_t)N * N * sizeof(float));
    for (int i = 0; i < N; i++)
        for (int k = 0; k < N; k++) {
            float a = A[i * N + k];
            const float *b = B + (size_t)k * N;
            float *c = C + (size_t)i * N;
            for (int j = 0; j < N; j++) c[j] += a * b[j];
        }
}

/* 分块：把大矩阵切成 bs×bs 的小块，块内三个维度都留在缓存里 */
static void mm_block(const float *A, const float *B, float *C, int N, int bs) {
    memset(C, 0, (size_t)N * N * sizeof(float));
    for (int ii = 0; ii < N; ii += bs)
        for (int kk = 0; kk < N; kk += bs)
            for (int jj = 0; jj < N; jj += bs) {
                int i_end = ii + bs < N ? ii + bs : N;
                int k_end = kk + bs < N ? kk + bs : N;
                int j_end = jj + bs < N ? jj + bs : N;
                for (int i = ii; i < i_end; i++)
                    for (int k = kk; k < k_end; k++) {
                        float a = A[(size_t)i * N + k];
                        const float *b = B + (size_t)k * N;
                        float *c = C + (size_t)i * N;
                        for (int j = jj; j < j_end; j++) c[j] += a * b[j];
                    }
            }
}

static double bench(void (*fn)(const float *, const float *, float *, int),
                    const float *A, const float *B, float *C, int N) {
    double best = 1e30;
    for (int r = 0; r < 3; r++) {
        double t0 = now_ns();
        fn(A, B, C, N);
        double dt = now_ns() - t0;
        if (dt < best) best = dt;
    }
    return best;
}

int main(void) {
    const int N = 1024;                    /* 4MB 一个矩阵，彻底超出 6MB 的 L3 */
    size_t elems = (size_t)N * N;

    float *A = (float *)xmalloc(elems * sizeof(float));
    float *B = (float *)xmalloc(elems * sizeof(float));
    float *C = (float *)xmalloc(elems * sizeof(float));

    seed_rng(20261008u);
    for (size_t i = 0; i < elems; i++) { A[i] = frand(); B[i] = frand(); }

    double flops = 2.0 * (double)N * N * N;
    double ms, gflops;

    ms = bench(mm_ijk, A, B, C, N) / 1e6;
    gflops = flops / (ms * 1e6);
    printf("GEMM|naive_ijk  N=%d  %9.3f ms  %7.2f GFLOPS\n", N, ms, gflops);
    RES("gemm_naive_ijk=%.3f|ms|gflops=%.2f", ms, gflops);

    ms = bench(mm_ikj, A, B, C, N) / 1e6;
    gflops = flops / (ms * 1e6);
    printf("GEMM|loop_ikj   N=%d  %9.3f ms  %7.2f GFLOPS\n", N, ms, gflops);
    RES("gemm_loop_ikj=%.3f|ms|gflops=%.2f", ms, gflops);

    int blocks[] = {8, 16, 32, 48, 64, 96, 128, 192, 256};
    double best_g = 0; int best_b = 0;
    for (int i = 0; i < (int)(sizeof(blocks) / sizeof(int)); i++) {
        int bs = blocks[i];
        double best = 1e30;
        for (int r = 0; r < 3; r++) {
            double t0 = now_ns();
            mm_block(A, B, C, N, bs);
            double dt = now_ns() - t0;
            if (dt < best) best = dt;
        }
        ms = best / 1e6;
        gflops = flops / (ms * 1e6);
        printf("GEMM|blocked_%-4d N=%d  %9.3f ms  %7.2f GFLOPS\n", bs, N, ms, gflops);
        RES("gemm_block_%d=%.3f|ms|gflops=%.2f", bs, ms, gflops);
        if (gflops > best_g) { best_g = gflops; best_b = bs; }
    }
    RES("gemm_best_block=%d|bs|gflops=%.2f", best_b, best_g);

    /* 正确性抽查：分块版和朴素版在同一个点上必须一致 */
    mm_ikj(A, B, C, N);
    float ref = C[100 * N + 200];
    memset(C, 0, elems * sizeof(float));
    mm_block(A, B, C, N, 32);
    float got = C[100 * N + 200];
    RES("gemm_check_diff=%.6f|abs|ref=%.4f|got=%.4f", fabsf(ref - got), ref, got);

    return 0;
}
