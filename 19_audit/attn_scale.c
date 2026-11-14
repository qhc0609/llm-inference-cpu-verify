/* 19 · Attention 规模复测：原来 seq 翻倍、时间涨 6.1 倍
 *
 * O(n²) 的算法，seq 翻倍时间应该涨 4 倍。原实验测到 6.1 倍，
 * 我当时当成"缓存装不下，超额增长"写进了文章。复核要判明：
 * 这是真的超额增长，还是小尺寸那头的固定开销把比例抬高了。
 * 做法：从 64 一路测到 2048，算每一档相对上一档的倍率，
 * 再看 n² 归一化后的时间是否平稳（平稳=纯二次，抬升=访存开始拖后腿）。
 */
#include "common.h"

/* 单线程 attention：scores = Q·Kᵀ / sqrt(d)，softmax 每行，再乘 V
 * 不做任何分块优化 —— 这一节要看的正是"朴素实现随规模怎么退化" */
static double attn(int n, int d, int reps, double *gflops_out) {
    float *Q = (float *)xmalloc((size_t)n * d * sizeof(float));
    float *K = (float *)xmalloc((size_t)n * d * sizeof(float));
    float *V = (float *)xmalloc((size_t)n * d * sizeof(float));
    float *S = (float *)xmalloc((size_t)n * n * sizeof(float));
    float *O = (float *)xmalloc((size_t)n * d * sizeof(float));
    for (int i = 0; i < n * d; i++) { Q[i] = frand() - 0.5f; K[i] = frand() - 0.5f; V[i] = frand() - 0.5f; }

    double best = 1e18;
    for (int r = 0; r < reps; r++) {
        double t0 = now_ns();
        /* ① S = Q·Kᵀ，n²d 次乘加 */
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++) {
                float s = 0;
                for (int k = 0; k < d; k++) s += Q[i * d + k] * K[j * d + k];
                S[i * n + j] = s * 0.125f;
            }
        /* ② 每行 softmax，n² 次 exp */
        for (int i = 0; i < n; i++) {
            float m = -1e30f;
            for (int j = 0; j < n; j++) if (S[i * n + j] > m) m = S[i * n + j];
            float sum = 0;
            for (int j = 0; j < n; j++) { S[i * n + j] = expf(S[i * n + j] - m); sum += S[i * n + j]; }
            float inv = 1.0f / sum;
            for (int j = 0; j < n; j++) S[i * n + j] *= inv;
        }
        /* ③ O = S·V，n²d 次乘加 */
        for (int i = 0; i < n; i++)
            for (int k = 0; k < d; k++) {
                float s = 0;
                for (int j = 0; j < n; j++) s += S[i * n + j] * V[j * d + k];
                O[i * d + k] = s;
            }
        double dt = now_ns() - t0;
        if (O[0] == 12345.0f) fprintf(stderr, "impossible\n");
        if (dt < best) best = dt;
    }
    free(Q); free(K); free(V); free(S); free(O);
    if (gflops_out) *gflops_out = (4.0 * n * n * d + 2.0 * n * n) / best;  /* 2 次矩阵乘各 2n²d */
    return best / 1e6;   /* ms */
}

int main(void) {
    seed_rng(20261114);
    int seqs[] = {64, 128, 256, 512, 1024, 2048};
    double prev = 0, prev_n2 = 0;
    for (unsigned i = 0; i < sizeof(seqs) / sizeof(seqs[0]); i++) {
        int n = seqs[i];
        int reps = n <= 512 ? 3 : 2;
        double gf = 0;
        double ms = attn(n, 64, reps, &gf);
        double n2 = ms / ((double)n * n);            /* 每 n² 一项花多少 ms */
        if (prev > 0)
            RES("attn|seq=%d|ms=%.3f|gflops=%.2f|times_vs_prev=%.2fx|n2_cost_ratio=%.2f",
                n, ms, gf, ms / prev, n2 / prev_n2);
        else
            RES("attn|seq=%d|ms=%.3f|gflops=%.2f|times_vs_prev=1.00|n2_cost_ratio=1.00",
                n, ms, gf);
        prev = ms; prev_n2 = n2;
    }
    return 0;
}
