/* 21f_gemv.c - 复刻 llama.cpp Decode 的计算图，用于 perf 采样与火焰图
 *
 * 为什么不是直接跑 llama.cpp：实验机没有模型文件也没有网络。
 * 这里按 Qwen2.5-0.5B 的真实张量形状（n_embd 896 / n_ff 4864 / 14 头 / 2 组 KV）
 * 和 llama.cpp 的算子顺序（rms_norm -> qkv -> rope -> attn -> o_proj -> ffn gate/up/silu/down）
 * 逐层实现，权重用 Q8_0（int8 + 每 32 个一组 scale）存放，
 * 使每 token 的访存量、算术强度与真实推理同量级。
 * 调用层次刻意与 ggml 对齐，便于 perf --call-graph 展开成可读的栈。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <immintrin.h>

#define N_LAYER 24
#define N_EMBD  896
#define N_HEAD  14
#define N_KV    2
#define HD      64
#define N_FF    4864
#define VOCAB   151936
#define KV_DIM  (N_KV * HD)

typedef struct { int8_t *q; float *s; int rows, cols; } qt;

static double now_ns(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

static qt *qt_new(int rows, int cols) {
    qt *t = calloc(1, sizeof(qt));
    t->rows = rows; t->cols = cols;
    t->q = aligned_alloc(64, (size_t)rows * cols);
    t->s = malloc(sizeof(float) * ((size_t)rows * cols / 32 + 1));
    /* 用确定性的小值填充：均值 0、幅度 1，避免全零让分支预测失真 */
    uint32_t seed = 0x9e3779b9u;
    for (size_t i = 0; i < (size_t)rows * cols; i++) {
        seed = seed * 1664525u + 1013904223u;
        t->q[i] = (int8_t)((seed >> 24) - 128);
    }
    for (size_t i = 0; i < (size_t)rows * cols / 32; i++) t->s[i] = 0.001f;
    return t;
}

/* Q8_0 GEMV：y[r] = sum_c x[c] * q[r][c] * scale
 * 一次算 4 行：4 条权重流并行，把 DRAM 延迟藏起来（单行逐条算只能跑出 2 GB/s）。 */
__attribute__((noinline)) static void gemv_q8_0(const qt *W, const float *x, float *y) {
    const int cols = W->cols;
    const int rows = W->rows;
    int r = 0;
    for (; r + 4 <= rows; r += 4) {
        const int8_t *w0 = W->q + (size_t)(r + 0) * cols;
        const int8_t *w1 = W->q + (size_t)(r + 1) * cols;
        const int8_t *w2 = W->q + (size_t)(r + 2) * cols;
        const int8_t *w3 = W->q + (size_t)(r + 3) * cols;
        __m256 a0 = _mm256_setzero_ps(), a1 = a0, a2 = a0, a3 = a0;
        for (int c = 0; c < cols; c += 32) {
            __m256 x0 = _mm256_loadu_ps(x + c);
            __m256 x1 = _mm256_loadu_ps(x + c + 8);
            __m256 x2 = _mm256_loadu_ps(x + c + 16);
            __m256 x3 = _mm256_loadu_ps(x + c + 24);
#define ROW(WP, ACC) do {                                                   \
            __m128i p0 = _mm_loadu_si128((const __m128i *)(WP));            \
            __m128i p1 = _mm_loadu_si128((const __m128i *)((WP) + 16));     \
            ACC = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(p0)), x0, ACC); \
            ACC = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(_mm_srli_si128(p0, 8))), x1, ACC); \
            ACC = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(p1)), x2, ACC); \
            ACC = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(_mm_srli_si128(p1, 8))), x3, ACC); \
        } while (0)
            ROW(w0 + c, a0);
            ROW(w1 + c, a1);
            ROW(w2 + c, a2);
            ROW(w3 + c, a3);
#undef ROW
        }
        float t[8];
#define EMIT(ACC, RR) do {                                                   \
            _mm256_storeu_ps(t, ACC);                                        \
            y[RR] = ((t[0]+t[1])+(t[2]+t[3]) + ((t[4]+t[5])+(t[6]+t[7])))   \
                    * W->s[(size_t)(RR) * (cols / 32)];                      \
        } while (0)
        EMIT(a0, r + 0); EMIT(a1, r + 1); EMIT(a2, r + 2); EMIT(a3, r + 3);
#undef EMIT
    }
    for (; r < rows; r++) {
        const int8_t *w = W->q + (size_t)r * cols;
        __m256 acc = _mm256_setzero_ps();
        for (int c = 0; c < cols; c += 32) {
            __m128i p0 = _mm_loadu_si128((const __m128i *)(w + c));
            __m128i p1 = _mm_loadu_si128((const __m128i *)(w + c + 16));
            acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(p0)),
                                  _mm256_loadu_ps(x + c), acc);
            acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(_mm_srli_si128(p0, 8))),
                                  _mm256_loadu_ps(x + c + 8), acc);
            acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(p1)),
                                  _mm256_loadu_ps(x + c + 16), acc);
            acc = _mm256_fmadd_ps(_mm256_cvtepi32_ps(_mm256_cvtepi8_epi32(_mm_srli_si128(p1, 8))),
                                  _mm256_loadu_ps(x + c + 24), acc);
        }
        float t[8]; _mm256_storeu_ps(t, acc);
        y[r] = ((t[0]+t[1])+(t[2]+t[3]) + ((t[4]+t[5])+(t[6]+t[7]))) * W->s[(size_t)r * (cols / 32)];
    }
}

__attribute__((noinline)) static void rms_norm(const float *x, const float *g, float *o, int n) {
    float ss = 0;
    for (int i = 0; i < n; i++) ss += x[i] * x[i];
    float inv = 1.0f / sqrtf(ss / n + 1e-6f);
    for (int i = 0; i < n; i++) o[i] = x[i] * inv * g[i];
}

__attribute__((noinline)) static void rope(float *v, int pos, int n_head, int hd) {
    for (int h = 0; h < n_head; h++) {
        float *p = v + h * hd;
        for (int i = 0; i < hd; i += 2) {
            float theta = powf(10000.0f, -(float)i / hd) * pos;
            float c = cosf(theta), s = sinf(theta);
            float a = p[i], b = p[i + 1];
            p[i]     = a * c - b * s;
            p[i + 1] = a * s + b * c;
        }
    }
}

/* 单头 softmax(QK^T/sqrt(d))V，KV 长度取 64（Decode 时的历史长度） */
__attribute__((noinline)) static void attention(const float *q, const float *k, const float *v,
                                                float *out, int n_head, int n_kv, int hd, int klen) {
    int group = n_head / n_kv;
    for (int h = 0; h < n_head; h++) {
        int kv = h / group;
        const float *qh = q + h * hd;
        const float *kh = k + kv * hd * klen;
        const float *vh = v + kv * hd * klen;
        float sc[128]; float mx = -1e30f;
        for (int t = 0; t < klen; t++) {
            float d = 0;
            const float *kt = kh + t * hd;
            for (int i = 0; i < hd; i++) d += qh[i] * kt[i];
            d *= 1.0f / sqrtf((float)hd);
            sc[t] = d; if (d > mx) mx = d;
        }
        float sum = 0;
        for (int t = 0; t < klen; t++) { sc[t] = expf(sc[t] - mx); sum += sc[t]; }
        float *oh = out + h * hd;
        for (int i = 0; i < hd; i++) oh[i] = 0;
        for (int t = 0; t < klen; t++) {
            float w = sc[t] / sum;
            const float *vt = vh + t * hd;
            for (int i = 0; i < hd; i++) oh[i] += w * vt[i];
        }
    }
}

__attribute__((noinline)) static void silu_mul(float *g, const float *u, int n) {
    for (int i = 0; i < n; i++) g[i] = (g[i] / (1.0f + expf(-g[i]))) * u[i];
}

/* ---- 权重（全局，避免每 token 重新分配） ---- */
static qt *Wq, *Wk, *Wv, *Wo, *Wgate, *Wup, *Wdown, *Wemb, *Whead;
static float *fn_g, *fn_b;
static float *kv_k, *kv_v;

__attribute__((noinline)) static void llama_build_graph(float *x, int pos) {
    float q[N_EMBD], k[KV_DIM], v[KV_DIM], att[KV_DIM], tmp[N_EMBD], h[N_FF], h2[N_FF];

    rms_norm(x, fn_g, tmp, N_EMBD);
    gemv_q8_0(Wq, tmp, q);
    gemv_q8_0(Wk, tmp, k);
    gemv_q8_0(Wv, tmp, v);
    rope(q, pos, N_HEAD, HD);
    rope(k, pos, N_KV, HD);

    /* 追加到 KV cache 的第 pos 个位置（环形，简化：直接放 pos % 64） */
    int slot = pos & 63;
    for (int i = 0; i < KV_DIM; i++) { kv_k[slot * KV_DIM + i] = k[i]; kv_v[slot * KV_DIM + i] = v[i]; }

    attention(q, kv_k, kv_v, att, N_HEAD, N_KV, HD, 64);
    gemv_q8_0(Wo, att, tmp);
    for (int i = 0; i < N_EMBD; i++) x[i] += tmp[i];

    rms_norm(x, fn_g, tmp, N_EMBD);
    gemv_q8_0(Wgate, tmp, h);
    gemv_q8_0(Wup, tmp, h2);
    silu_mul(h, h2, N_FF);
    gemv_q8_0(Wdown, h, tmp);
    for (int i = 0; i < N_EMBD; i++) x[i] += tmp[i];
}

__attribute__((noinline)) static void graph_compute(float *x, int pos) {
    for (int l = 0; l < N_LAYER; l++) llama_build_graph(x, pos);
}

__attribute__((noinline)) static void llama_decode(float *x, int pos) {
    graph_compute(x, pos);
}

__attribute__((noinline)) static void lm_head(const float *x, float *logits) {
    gemv_q8_0(Whead, x, logits);
}

int main(int argc, char **argv) {
    int ntok = argc > 1 ? atoi(argv[1]) : 24;

    Wq    = qt_new(N_EMBD, N_EMBD);
    Wk    = qt_new(KV_DIM, N_EMBD);
    Wv    = qt_new(KV_DIM, N_EMBD);
    Wo    = qt_new(N_EMBD, N_EMBD);
    Wgate = qt_new(N_FF,   N_EMBD);
    Wup   = qt_new(N_FF,   N_EMBD);
    Wdown = qt_new(N_EMBD, N_FF);
    Wemb  = qt_new(N_EMBD, N_EMBD);      /* 只取一个 896x896 的块代表词嵌入流 */
    Whead = qt_new(VOCAB,  N_EMBD);
    fn_g  = malloc(sizeof(float) * N_EMBD);
    for (int i = 0; i < N_EMBD; i++) fn_g[i] = 1.0f;
    kv_k = calloc((size_t)64 * KV_DIM, sizeof(float));
    kv_v = calloc((size_t)64 * KV_DIM, sizeof(float));

    /* 统计每 token 真正要流过的权重字节：24 层的权重 + 输出头 */
    size_t per_layer = 0;
    qt *lw[] = {Wq, Wk, Wv, Wo, Wgate, Wup, Wdown};
    for (int i = 0; i < 7; i++) per_layer += (size_t)lw[i]->rows * lw[i]->cols;
    size_t wbytes = per_layer * N_LAYER + (size_t)Whead->rows * Whead->cols;

    float *x = malloc(sizeof(float) * N_EMBD);
    float *logits = malloc(sizeof(float) * VOCAB);
    for (int i = 0; i < N_EMBD; i++) x[i] = 0.02f * (float)(i % 13);

    /* 预热 1 个 token */
    llama_decode(x, 0);
    lm_head(x, logits);

    double t0 = now_ns();
    for (int t = 0; t < ntok; t++) {
        llama_decode(x, 8 + t);
        lm_head(x, logits);
    }
    double t1 = now_ns();

    float checksum = 0;
    for (int i = 0; i < VOCAB; i += 997) checksum += logits[i];
    printf("TOKENS=%d\nLAYERS=%d\nWEIGHT_MB=%.1f\nNS=%.0f\nMS_PER_TOKEN=%.2f\nTOK_PER_SEC=%.2f\nBANDWIDTH_GBPS=%.2f\nCHECKSUM=%.3f\n",
           ntok, N_LAYER, wbytes / 1048576.0, t1 - t0, (t1 - t0) / 1e6 / ntok,
           ntok / ((t1 - t0) / 1e9), wbytes * (double)ntok / ((t1 - t0) / 1e9) / 1e9, checksum);

    for (int i = 0; i < 7; i++) { free(lw[i]->q); free(lw[i]->s); free(lw[i]); }
    free(Whead->q); free(Whead->s); free(Whead);
    free(fn_g); free(kv_k); free(kv_v); free(x); free(logits);
    return 0;
}
