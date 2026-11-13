/* 06 · 大模型推理执行流程：CPU 究竟在做什么
 * 对应文章：第 06 篇《大模型推理执行流程》
 *
 * 一个能真跑的最小 Transformer 前向，权重随机但结构真实：
 *   词嵌入 -> QKV 投影 -> 多头注意力 -> 输出投影 -> FFN -> LM Head
 * 每一段单独计时，看时间到底花在哪。
 *
 * 这里故意不用任何库（没有 BLAS、没有 OpenMP、没有 SIMD），
 * 目的不是跑得快，而是让每一纳秒都能对上文章里的那张流程图。
 */
#include "common.h"

typedef struct { int d, nh, hd, seq, ffn, vocab, layers; } Cfg;

/* 最朴素的矩阵乘：C[M,N] = A[M,K] * B[K,N]，B 按行主序 */
static void matmul(const float *A, const float *B, float *C, int M, int K, int N) {
    for (int i = 0; i < M; i++) {
        for (int k = 0; k < K; k++) {
            float a = A[i * K + k];
            const float *brow = B + k * N;
            float *crow = C + i * N;
            for (int j = 0; j < N; j++) crow[j] += a * brow[j];
        }
    }
}

static void layernorm(float *x, int n) {
    double s = 0;
    for (int i = 0; i < n; i++) s += x[i];
    float mean = (float)(s / n);
    double v = 0;
    for (int i = 0; i < n; i++) { float d = x[i] - mean; v += d * d; }
    float inv = 1.0f / sqrtf((float)(v / n) + 1e-5f);
    for (int i = 0; i < n; i++) x[i] = (x[i] - mean) * inv;
}

static void softmax(float *x, int n) {
    float mx = x[0];
    for (int i = 1; i < n; i++) if (x[i] > mx) mx = x[i];
    float s = 0;
    for (int i = 0; i < n; i++) { x[i] = expf(x[i] - mx); s += x[i]; }
    float inv = 1.0f / s;
    for (int i = 0; i < n; i++) x[i] *= inv;
}

int main(int argc, char **argv) {
    Cfg c;
    c.d = 256; c.nh = 8; c.seq = 64; c.ffn = 1024; c.vocab = 4096; c.layers = 1;
    c.hd = c.d / c.nh;
    if (argc > 1) c.seq = atoi(argv[1]);

    int d = c.d, seq = c.seq;

    printf("# 配置: d_model=%d heads=%d head_dim=%d seq=%d ffn=%d vocab=%d layers=%d\n",
           c.d, c.nh, c.hd, c.seq, c.ffn, c.vocab, c.layers);

    /* 权重（随机，只关心计算量不关心数值） */
    float *Wqkv = (float *)xmalloc((size_t)d * 3 * d * sizeof(float));
    float *Wo   = (float *)xmalloc((size_t)d * d * sizeof(float));
    float *W1   = (float *)xmalloc((size_t)d * c.ffn * sizeof(float));
    float *W2   = (float *)xmalloc((size_t)c.ffn * d * sizeof(float));
    float *Wlm  = (float *)xmalloc((size_t)d * c.vocab * sizeof(float));
    float *Emb  = (float *)xmalloc((size_t)c.vocab * d * sizeof(float));

    seed_rng(20261008u);
    float *ws[] = {Wqkv, Wo, W1, W2, Wlm, Emb};
    size_t wn[] = {(size_t)d*3*d, (size_t)d*d, (size_t)d*c.ffn, (size_t)c.ffn*d, (size_t)d*c.vocab, (size_t)c.vocab*d};
    for (int k = 0; k < 6; k++)
        for (size_t i = 0; i < wn[k]; i++) ws[k][i] = (frand() - 0.5f) * 0.08f;

    /* 激活值 */
    float *X  = (float *)xmalloc((size_t)seq * d * sizeof(float));
    float *QKV= (float *)xmalloc((size_t)seq * 3 * d * sizeof(float));
    float *Att= (float *)xmalloc((size_t)seq * d * sizeof(float));
    float *O  = (float *)xmalloc((size_t)seq * d * sizeof(float));
    float *H  = (float *)xmalloc((size_t)seq * c.ffn * sizeof(float));
    float *Y  = (float *)xmalloc((size_t)seq * d * sizeof(float));
    float *Log= (float *)xmalloc((size_t)seq * c.vocab * sizeof(float));
    float *Scr= (float *)xmalloc((size_t)seq * seq * sizeof(float));

    int tokens[64];
    for (int i = 0; i < seq; i++) tokens[i] = (int)(drand48() * c.vocab) % c.vocab;

    double t_emb = 0, t_qkv = 0, t_attn = 0, t_o = 0, t_ffn = 0, t_lm = 0;

    const int REP = 3;
    for (int rep = 0; rep < REP; rep++) {
        double t0, t1;

        /* --- 1. 词嵌入：一次查表，几乎零成本 --- */
        t0 = now_ns();
        for (int i = 0; i < seq; i++)
            memcpy(X + (size_t)i * d, Emb + (size_t)tokens[i] * d, (size_t)d * sizeof(float));
        t1 = now_ns(); t_emb = t1 - t0;

        /* --- 2. QKV 投影：seq×d 乘 d×3d --- */
        t0 = now_ns();
        matmul(X, Wqkv, QKV, seq, d, 3 * d);
        t1 = now_ns(); t_qkv = t1 - t0;

        /* --- 3. 多头注意力 ---
         * 为了缩短代码，这里把 8 个头合成一次全维度计算；
         * 计算量与真正的分头实现完全相同（都是 seq×d×seq 级别的乘加）。 */
        t0 = now_ns();
        /* QK^T */
        for (int i = 0; i < seq; i++)
            for (int j = 0; j < seq; j++) {
                float s = 0;
                const float *qi = QKV + (size_t)i * 3 * d;
                const float *kj = QKV + (size_t)j * 3 * d + d;
                for (int k = 0; k < d; k++) s += qi[k] * kj[k];
                Scr[(size_t)i * seq + j] = s / sqrtf((float)c.hd);
            }
        for (int i = 0; i < seq; i++) softmax(Scr + (size_t)i * seq, seq);
        /* 乘 V */
        for (int i = 0; i < seq; i++)
            for (int k = 0; k < d; k++) {
                float s = 0;
                for (int j = 0; j < seq; j++)
                    s += Scr[(size_t)i * seq + j] * QKV[(size_t)j * 3 * d + 2 * d + k];
                Att[(size_t)i * d + k] = s;
            }
        t1 = now_ns(); t_attn = t1 - t0;

        /* --- 4. 输出投影 --- */
        t0 = now_ns();
        matmul(Att, Wo, O, seq, d, d);
        t1 = now_ns(); t_o = t1 - t0;

        /* --- 5. FFN：两层，中间放大 4 倍 --- */
        t0 = now_ns();
        matmul(O, W1, H, seq, d, c.ffn);
        for (size_t i = 0; i < (size_t)seq * c.ffn; i++) H[i] = H[i] > 0 ? H[i] : 0; /* ReLU */
        matmul(H, W2, Y, seq, c.ffn, d);
        layernorm(Y, seq * d);
        t1 = now_ns(); t_ffn = t1 - t0;

        /* --- 6. LM Head：只算最后一个 token 的 logits --- */
        t0 = now_ns();
        matmul(Y + (size_t)(seq - 1) * d, Wlm, Log + (size_t)(seq - 1) * c.vocab, 1, d, c.vocab);
        t1 = now_ns(); t_lm = t1 - t0;
    }

    double total = t_emb + t_qkv + t_attn + t_o + t_ffn + t_lm;
    printf("#\n# 阶段           耗时(ms)   占比\n");
    printf("STAGE|embedding   %8.3f  %5.1f%%\n", t_emb / 1e6,  t_emb / total * 100);
    printf("STAGE|qkv_proj    %8.3f  %5.1f%%\n", t_qkv / 1e6,  t_qkv / total * 100);
    printf("STAGE|attention   %8.3f  %5.1f%%\n", t_attn / 1e6, t_attn / total * 100);
    printf("STAGE|out_proj    %8.3f  %5.1f%%\n", t_o / 1e6,    t_o / total * 100);
    printf("STAGE|ffn         %8.3f  %5.1f%%\n", t_ffn / 1e6,  t_ffn / total * 100);
    printf("STAGE|lm_head     %8.3f  %5.1f%%\n", t_lm / 1e6,   t_lm / total * 100);
    printf("# 合计 %.3f ms / %d token = %.2f ms/token\n", total / 1e6, seq, total / 1e6 / seq);

    RES("stage_embedding_ms=%.3f|ms", t_emb / 1e6);
    RES("stage_qkv_ms=%.3f|ms", t_qkv / 1e6);
    RES("stage_attention_ms=%.3f|ms", t_attn / 1e6);
    RES("stage_outproj_ms=%.3f|ms", t_o / 1e6);
    RES("stage_ffn_ms=%.3f|ms", t_ffn / 1e6);
    RES("stage_lmhead_ms=%.3f|ms", t_lm / 1e6);
    RES("stage_total_ms=%.3f|ms", total / 1e6);
    RES("per_token_ms=%.3f|ms/token|seq=%d", total / 1e6 / seq, seq);

    return 0;
}
