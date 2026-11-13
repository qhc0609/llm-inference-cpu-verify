/* 10 · Prefill 与 Decode 两个计算阶段
 * 对应文章：第 10 篇《Prefill 与 Decode》
 *
 * 同一份权重，同样的矩阵乘，只是"一次处理几个 token"不同，
 * 结果就从"算力受限"变成"带宽受限"。
 *
 * 这里用固定 M×K 的权重矩阵，分别跑 M_batch = 1 / 8 / 32 / 128 / 512，
 * 观察每 token 耗时、算力利用率、有效带宽三条曲线的方向。
 */
#include "common.h"

#define M 2048
#define K 2048

/* y[batch][M] = x[batch][K] * W^T   (W 为 M×K)
 *
 * ★ 关键点：先把 X 转置成 Xt[K][batch]，
 *   然后按"外积"的方式算：一次读一个权重 w，就把 batch 里所有 token 一起更新。
 *   权重 W[i][k] 只从内存读一次，却被 batch 个 token 复用。
 *   这就是 prefill 比 decode 划算的物理原因：
 *   同一个权重字节，喂 1 个 token 还是 512 个，都只读一次。 */
static void gemm(const float *W, const float *X, float *Y, int batch,
                 float *Xt, float *Yt) {
    /* X[batch][K] -> Xt[K][batch]，让内层循环在 batch 方向上连续 */
    for (int b = 0; b < batch; b++)
        for (int k = 0; k < K; k++)
            Xt[(size_t)k * batch + b] = X[(size_t)b * K + k];

    for (int i = 0; i < M; i++) {
        const float *wrow = W + (size_t)i * K;
        float *yrow = Yt + (size_t)i * batch;
        for (int b = 0; b < batch; b++) yrow[b] = 0.0f;
        for (int k = 0; k < K; k++) {
            float w = wrow[k];
            const float *xk = Xt + (size_t)k * batch;
            for (int b = 0; b < batch; b++) yrow[b] += w * xk[b];
        }
    }
    /* Yt[M][batch] -> Y[batch][M] */
    for (int i = 0; i < M; i++)
        for (int b = 0; b < batch; b++)
            Y[(size_t)b * M + i] = Yt[(size_t)i * batch + b];
}

int main(void) {
    float *W = (float *)xmalloc((size_t)M * K * sizeof(float));
    int batches[] = {1, 8, 32, 128, 512};
    int n = (int)(sizeof(batches) / sizeof(int));
    int maxb = 512;

    float *X = (float *)xmalloc((size_t)maxb * K * sizeof(float));
    float *Y = (float *)xmalloc((size_t)maxb * M * sizeof(float));
    float *Xt = (float *)xmalloc((size_t)maxb * K * sizeof(float));
    float *Yt = (float *)xmalloc((size_t)maxb * M * sizeof(float));

    seed_rng(20261008u);
    for (size_t i = 0; i < (size_t)M * K; i++) W[i] = (frand() - 0.5f) * 0.1f;
    for (size_t i = 0; i < (size_t)maxb * K; i++) X[i] = frand() - 0.5f;

    double wbytes = (double)M * K * 4.0;   /* 权重必须完整读一遍 */

    printf("# phase      batch   耗时(ms)   每token(us)  算力(GFLOPS)  权重带宽(GB/s)\n");

    for (int s = 0; s < n; s++) {
        int b = batches[s];
        double best = 1e30;
        for (int r = 0; r < 3; r++) {
            double t0 = now_ns();
            gemm(W, X, Y, b, Xt, Yt);
            double dt = now_ns() - t0;
            if (dt < best) best = dt;
        }
        double ms = best / 1e6;
        double flops = 2.0 * (double)b * M * K;
        double gflops = flops / (ms * 1e6);
        double per_tok_us = best / 1000.0 / b;
        double bw = wbytes / best;          /* bytes/ns == GB/s */

        const char *phase = (b == 1) ? "decode" : (b >= 128 ? "prefill" : "mixed");
        printf("PD|%-9s %5d  %9.3f  %10.1f  %12.2f  %14.1f\n",
               phase, b, ms, per_tok_us, gflops, bw);
        RES("pd_batch%d_ms=%.3f|ms|per_token_us=%.1f|gflops=%.2f|wbandwidth_gbs=%.1f|phase=%s",
            b, ms, per_tok_us, gflops, bw, phase);
    }

    /* 关键对比：decode 一个 token 的时间，和 prefill 平均每个 token 的时间 */
    double best1 = 1e30, best512 = 1e30;
    for (int r = 0; r < 3; r++) {
        double t0 = now_ns(); gemm(W, X, Y, 1, Xt, Yt);   double d = now_ns() - t0; if (d < best1) best1 = d;
    }
    for (int r = 0; r < 3; r++) {
        double t0 = now_ns(); gemm(W, X, Y, 512, Xt, Yt); double d = now_ns() - t0; if (d < best512) best512 = d;
    }
    RES("pd_decode_per_token_us=%.1f|us", best1 / 1000.0);
    RES("pd_prefill_per_token_us=%.1f|us", best512 / 1000.0 / 512);
    RES("pd_ratio=%.1f|x|decode_slower_than_prefill_per_token", (best1 / 1.0) / (best512 / 512.0));

    return 0;
}
