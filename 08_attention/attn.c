/* 08 · Transformer 与 Attention 在 CPU 上如何执行
 * 对应文章：第 08 篇《Transformer 与 Attention》
 *
 * 验证重点：Attention 的代价是 seq² 增长的。
 * 序列长度翻倍，计算量翻四倍——这就是为什么长上下文在 CPU 上很贵。
 * 同时对比"先算完整分数矩阵"和"分块流式计算"两种写法。
 */
#include "common.h"

/* 完整算：先把 seq×seq 的分数矩阵全部写进内存，再 softmax，再乘 V */
static double attn_full(const float *Q, const float *K, const float *V,
                        int seq, int d) {
    float *S = (float *)xmalloc((size_t)seq * seq * sizeof(float));
    float *O = (float *)xmalloc((size_t)seq * d * sizeof(float));

    double t0 = now_ns();

    for (int i = 0; i < seq; i++) {
        for (int j = 0; j < seq; j++) {
            float s = 0;
            const float *qi = Q + (size_t)i * d;
            const float *kj = K + (size_t)j * d;
            for (int k = 0; k < d; k++) s += qi[k] * kj[k];
            S[(size_t)i * seq + j] = s / sqrtf((float)d);
        }
    }
    for (int i = 0; i < seq; i++) {
        float *row = S + (size_t)i * seq;
        float mx = row[0];
        for (int j = 1; j < seq; j++) if (row[j] > mx) mx = row[j];
        float sum = 0;
        for (int j = 0; j < seq; j++) { row[j] = expf(row[j] - mx); sum += row[j]; }
        float inv = 1.0f / sum;
        for (int j = 0; j < seq; j++) row[j] *= inv;
    }
    for (int i = 0; i < seq; i++)
        for (int k = 0; k < d; k++) {
            float s = 0;
            const float *row = S + (size_t)i * seq;
            for (int j = 0; j < seq; j++) s += row[j] * V[(size_t)j * d + k];
            O[(size_t)i * d + k] = s;
        }

    double dt = now_ns() - t0;
    free(S); free(O);
    return dt;
}

/* 流式算：不落盘中间矩阵，每个 query 一行一行处理，内存只需 O(seq) */
static double attn_stream(const float *Q, const float *K, const float *V,
                          int seq, int d) {
    float *row = (float *)xmalloc((size_t)seq * sizeof(float));
    float *out = (float *)xmalloc((size_t)d * sizeof(float));

    double t0 = now_ns();
    for (int i = 0; i < seq; i++) {
        const float *qi = Q + (size_t)i * d;
        float mx = -1e30f;
        for (int j = 0; j < seq; j++) {
            const float *kj = K + (size_t)j * d;
            float s = 0;
            for (int k = 0; k < d; k++) s += qi[k] * kj[k];
            s /= sqrtf((float)d);
            row[j] = s;
            if (s > mx) mx = s;
        }
        float sum = 0;
        for (int j = 0; j < seq; j++) { row[j] = expf(row[j] - mx); sum += row[j]; }
        for (int k = 0; k < d; k++) out[k] = 0;
        for (int j = 0; j < seq; j++) {
            float w = row[j] / sum;
            const float *vj = V + (size_t)j * d;
            for (int k = 0; k < d; k++) out[k] += w * vj[k];
        }
    }
    double dt = now_ns() - t0;
    free(row); free(out);
    return dt;
}

int main(void) {
    const int d = 64;
    int seqs[] = {32, 64, 128, 256, 512, 1024};
    int n = (int)(sizeof(seqs) / sizeof(int));

    seed_rng(20261008u);

    printf("# seq     完整矩阵(ms)   流式(ms)   分数矩阵大小(MB)   QK乘加量(M)\n");
    double prev_ms = 0; int prev_seq = 0;

    for (int s = 0; s < n; s++) {
        int seq = seqs[s];
        float *Q = (float *)xmalloc((size_t)seq * d * sizeof(float));
        float *K = (float *)xmalloc((size_t)seq * d * sizeof(float));
        float *V = (float *)xmalloc((size_t)seq * d * sizeof(float));
        for (int i = 0; i < seq * d; i++) { Q[i] = frand() - 0.5f; K[i] = frand() - 0.5f; V[i] = frand() - 0.5f; }

        double t_full = attn_full(Q, K, V, seq, d) / 1e6;
        double t_stream = attn_stream(Q, K, V, seq, d) / 1e6;
        double mb = (double)seq * seq * 4.0 / (1024.0 * 1024.0);
        double mmac = (double)seq * seq * d / 1e6;

        printf("ATT|%5d  %12.3f  %9.3f  %13.2f  %12.1f\n", seq, t_full, t_stream, mb, mmac);
        RES("attn_seq%d_full=%.3f|ms|stream=%.3f|ms|score_mb=%.2f", seq, t_full, t_stream, mb);

        if (prev_seq > 0) {
            double grow_seq = (double)seq / prev_seq;
            double grow_t = t_full / prev_ms;
            if (s == n - 1) RES("attn_scaling=seq*%.0f_time*%.1f|quadratic_check", grow_seq, grow_t);
        }
        prev_ms = t_full; prev_seq = seq;

        free(Q); free(K); free(V);
    }

    /* 两种写法数学等价，只是中间矩阵占的内存不同 */
    RES("attn_note=full_vs_stream_same_math_different_memory|1");
    return 0;
}
