// 22gemm.c - 第 22 篇：CPU 算子优化实验台
// C[MxN] = A[MxK] * B[KxN]，A/B 行主序，float32
// 输出 CSV：R|kernel|M|N|K|T|rep|ms|gflops|gbps|maxerr
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#include <immintrin.h>

#define BS 32
#define ALIGN 64

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void *xmalloc(size_t n)
{
    void *p = NULL;
    if (posix_memalign(&p, ALIGN, n)) { fprintf(stderr, "oom\n"); exit(1); }
    memset(p, 0, n);
    return p;
}

/* ---------------------------------------------------------------- 参考实现 */
static void ref_gemm(int M, int N, int K, const float *A, const float *B, double *C)
{
    for (int i = 0; i < M; i++)
        for (int k = 0; k < K; k++) {
            double a = A[(size_t)i * K + k];
            for (int j = 0; j < N; j++)
                C[(size_t)i * N + j] += a * B[(size_t)k * N + j];
        }
}

static double max_err(int M, int N, const float *C, const double *R)
{
    double e = 0;
    for (size_t t = 0; t < (size_t)M * N; t++) {
        double d = fabs((double)C[t] - R[t]);
        if (d > e) e = d;
    }
    return e;
}

static void fill_mat(float *p, size_t n, unsigned seed)
{
    for (size_t t = 0; t < n; t++)
        p[t] = (float)((t * 1103515245u + seed) % 1000) / 1000.f - 0.5f;
}

/* ---------------------------------------------------------------- GELU（向量版与标量版同式） */
static inline float gelu_s(float x)
{
    double c = 0.7978845608028654;
    double inner = c * (x + 0.044715 * x * x * x);
    double e = exp(2.0 * inner);
    double th = (e - 1.0) / (e + 1.0);
    return (float)(0.5 * x * (1.0 + th));
}

/* ---------------------------------------------------------------- 1. 教科书三重循环 */
static void k_naive(int M, int N, int K, const float *A, const float *B, float *C)
{
    for (int i = 0; i < M; i++)
        for (int j = 0; j < N; j++) {
            float s = 0.f;
            for (int k = 0; k < K; k++)
                s += A[(size_t)i * K + k] * B[(size_t)k * N + j];
            C[(size_t)i * N + j] = s;
        }
}

/* ---------------------------------------------------------------- 2. 换循环顺序 ikj */
static void k_ikj(int M, int N, int K, const float *A, const float *B, float *C)
{
    for (int i = 0; i < M; i++) {
        float *cr = C + (size_t)i * N;
        for (int k = 0; k < K; k++) {
            float a = A[(size_t)i * K + k];
            const float *br = B + (size_t)k * N;
            for (int j = 0; j < N; j++)
                cr[j] += a * br[j];
        }
    }
}

/* ---------------------------------------------------------------- 3. AVX2 单累加器 */
static void k_simd1(int M, int N, int K, const float *A, const float *B, float *C)
{
    int jn = N & ~7;
    for (int i = 0; i < M; i++) {
        float *cr = C + (size_t)i * N;
        for (int k = 0; k < K; k++) {
            __m256 a = _mm256_set1_ps(A[(size_t)i * K + k]);
            const float *br = B + (size_t)k * N;
            for (int j = 0; j < jn; j += 8)
                _mm256_storeu_ps(cr + j, _mm256_fmadd_ps(a, _mm256_loadu_ps(br + j), _mm256_loadu_ps(cr + j)));
            for (int j = jn; j < N; j++) cr[j] += A[(size_t)i * K + k] * br[j];
        }
    }
}

/* ---------------------------------------------------------------- 4. 寄存器分块 R x 8 */
#define GEN_TILE(NAME, R)                                                           \
static void NAME(int M, int N, int K, const float *A, const float *B, float *C)     \
{                                                                                   \
    int jn = N & ~7, i = 0;                                                         \
    for (; i + (R) <= M; i += (R)) {                                                \
        for (int j = 0; j < jn; j += 8) {                                           \
            __m256 acc[(R)];                                                        \
            for (int r = 0; r < (R); r++) acc[r] = _mm256_setzero_ps();             \
            for (int k = 0; k < K; k++) {                                           \
                __m256 b = _mm256_loadu_ps(B + (size_t)k * N + j);                  \
                for (int r = 0; r < (R); r++)                                       \
                    acc[r] = _mm256_fmadd_ps(_mm256_set1_ps(A[(size_t)(i + r) * K + k]), b, acc[r]); \
            }                                                                       \
            for (int r = 0; r < (R); r++)                                           \
                _mm256_storeu_ps(C + (size_t)(i + r) * N + j,                       \
                    _mm256_add_ps(_mm256_loadu_ps(C + (size_t)(i + r) * N + j), acc[r])); \
        }                                                                           \
    }                                                                               \
    for (; i < M; i++)                                                              \
        for (int j = 0; j < N; j++) {                                               \
            float s = 0;                                                            \
            for (int k = 0; k < K; k++) s += A[(size_t)i * K + k] * B[(size_t)k * N + j]; \
            C[(size_t)i * N + j] += s;                                              \
        }                                                                           \
}

GEN_TILE(k_t1, 1)
GEN_TILE(k_t2, 2)
GEN_TILE(k_t4, 4)
GEN_TILE(k_t6, 6)
GEN_TILE(k_t8, 8)

/* ---------------------------------------------------------------- 5. k 循环展开 */
static void k_t4u2(int M, int N, int K, const float *A, const float *B, float *C)
{
    int jn = N & ~7, i = 0;
    for (; i + 4 <= M; i += 4) {
        for (int j = 0; j < jn; j += 8) {
            __m256 c0 = _mm256_setzero_ps(), c1 = c0, c2 = c0, c3 = c0;
            int k = 0;
            for (; k + 2 <= K; k += 2) {
                __m256 b0 = _mm256_loadu_ps(B + (size_t)k * N + j);
                __m256 b1 = _mm256_loadu_ps(B + (size_t)(k + 1) * N + j);
                c0 = _mm256_fmadd_ps(_mm256_set1_ps(A[(size_t)i * K + k]), b0, c0);
                c1 = _mm256_fmadd_ps(_mm256_set1_ps(A[(size_t)(i + 1) * K + k]), b0, c1);
                c2 = _mm256_fmadd_ps(_mm256_set1_ps(A[(size_t)(i + 2) * K + k]), b0, c2);
                c3 = _mm256_fmadd_ps(_mm256_set1_ps(A[(size_t)(i + 3) * K + k]), b0, c3);
                c0 = _mm256_fmadd_ps(_mm256_set1_ps(A[(size_t)i * K + k + 1]), b1, c0);
                c1 = _mm256_fmadd_ps(_mm256_set1_ps(A[(size_t)(i + 1) * K + k + 1]), b1, c1);
                c2 = _mm256_fmadd_ps(_mm256_set1_ps(A[(size_t)(i + 2) * K + k + 1]), b1, c2);
                c3 = _mm256_fmadd_ps(_mm256_set1_ps(A[(size_t)(i + 3) * K + k + 1]), b1, c3);
            }
            for (; k < K; k++) {
                __m256 b0 = _mm256_loadu_ps(B + (size_t)k * N + j);
                c0 = _mm256_fmadd_ps(_mm256_set1_ps(A[(size_t)i * K + k]), b0, c0);
                c1 = _mm256_fmadd_ps(_mm256_set1_ps(A[(size_t)(i + 1) * K + k]), b0, c1);
                c2 = _mm256_fmadd_ps(_mm256_set1_ps(A[(size_t)(i + 2) * K + k]), b0, c2);
                c3 = _mm256_fmadd_ps(_mm256_set1_ps(A[(size_t)(i + 3) * K + k]), b0, c3);
            }
            _mm256_storeu_ps(C + (size_t)i * N + j, _mm256_add_ps(_mm256_loadu_ps(C + (size_t)i * N + j), c0));
            _mm256_storeu_ps(C + (size_t)(i + 1) * N + j, _mm256_add_ps(_mm256_loadu_ps(C + (size_t)(i + 1) * N + j), c1));
            _mm256_storeu_ps(C + (size_t)(i + 2) * N + j, _mm256_add_ps(_mm256_loadu_ps(C + (size_t)(i + 2) * N + j), c2));
            _mm256_storeu_ps(C + (size_t)(i + 3) * N + j, _mm256_add_ps(_mm256_loadu_ps(C + (size_t)(i + 3) * N + j), c3));
        }
    }
    for (; i < M; i++)
        for (int j = 0; j < N; j++) {
            float s = 0;
            for (int k = 0; k < K; k++) s += A[(size_t)i * K + k] * B[(size_t)k * N + j];
            C[(size_t)i * N + j] += s;
        }
}

/* ---------------------------------------------------------------- 6. 手写汇编 vfmadd231ps */
static void k_asm(int M, int N, int K, const float *A, const float *B, float *C)
{
    int jn = N & ~7, i = 0;
    for (; i + 4 <= M; i += 4) {
        for (int j = 0; j < jn; j += 8) {
            __m256 c0 = _mm256_setzero_ps(), c1 = c0, c2 = c0, c3 = c0;
            for (int k = 0; k < K; k++) {
                __m256 a0 = _mm256_set1_ps(A[(size_t)i * K + k]);
                __m256 a1 = _mm256_set1_ps(A[(size_t)(i + 1) * K + k]);
                __m256 a2 = _mm256_set1_ps(A[(size_t)(i + 2) * K + k]);
                __m256 a3 = _mm256_set1_ps(A[(size_t)(i + 3) * K + k]);
                const float *bp = B + (size_t)k * N + j;
                __asm__ volatile(
                    "vmovups (%[bp]), %%ymm9          \n\t"
                    "vfmadd231ps %%ymm9, %[a0], %[c0] \n\t"
                    "vfmadd231ps %%ymm9, %[a1], %[c1] \n\t"
                    "vfmadd231ps %%ymm9, %[a2], %[c2] \n\t"
                    "vfmadd231ps %%ymm9, %[a3], %[c3] \n\t"
                    : [c0] "+x"(c0), [c1] "+x"(c1), [c2] "+x"(c2), [c3] "+x"(c3)
                    : [bp] "r"(bp), [a0] "x"(a0), [a1] "x"(a1), [a2] "x"(a2), [a3] "x"(a3)
                    : "ymm9", "memory");
            }
            _mm256_storeu_ps(C + (size_t)i * N + j, _mm256_add_ps(_mm256_loadu_ps(C + (size_t)i * N + j), c0));
            _mm256_storeu_ps(C + (size_t)(i + 1) * N + j, _mm256_add_ps(_mm256_loadu_ps(C + (size_t)(i + 1) * N + j), c1));
            _mm256_storeu_ps(C + (size_t)(i + 2) * N + j, _mm256_add_ps(_mm256_loadu_ps(C + (size_t)(i + 2) * N + j), c2));
            _mm256_storeu_ps(C + (size_t)(i + 3) * N + j, _mm256_add_ps(_mm256_loadu_ps(C + (size_t)(i + 3) * N + j), c3));
        }
    }
    for (; i < M; i++)
        for (int j = 0; j < N; j++) {
            float s = 0;
            for (int k = 0; k < K; k++) s += A[(size_t)i * K + k] * B[(size_t)k * N + j];
            C[(size_t)i * N + j] += s;
        }
}

/* ---------------------------------------------------------------- 7. 分块版 */
static int MC_G = 64, NC_G = 256, KC_G = 256;

/* 处理 [i0,i1) 行，含列尾与行尾兜底 */
static void tiled_range(int i0, int i1, int N, int K, const float *A, const float *B, float *C)
{
    int jn = N & ~7;
    for (int ii = i0; ii < i1; ii += MC_G) {
        int imax = ii + MC_G < i1 ? ii + MC_G : i1;
        int i4 = ii + ((imax - ii) / 4) * 4;
        for (int jj = 0; jj < jn; jj += NC_G) {
            int jmax = jj + NC_G < jn ? jj + NC_G : jn;
            for (int kk = 0; kk < K; kk += KC_G) {
                int kmax = kk + KC_G < K ? kk + KC_G : K;
                for (int i = ii; i < i4; i += 4) {
                    for (int j = jj; j < jmax; j += 8) {
                        __m256 c0 = _mm256_setzero_ps(), c1 = c0, c2 = c0, c3 = c0;
                        for (int k = kk; k < kmax; k++) {
                            __m256 b = _mm256_loadu_ps(B + (size_t)k * N + j);
                            c0 = _mm256_fmadd_ps(_mm256_set1_ps(A[(size_t)i * K + k]), b, c0);
                            c1 = _mm256_fmadd_ps(_mm256_set1_ps(A[(size_t)(i + 1) * K + k]), b, c1);
                            c2 = _mm256_fmadd_ps(_mm256_set1_ps(A[(size_t)(i + 2) * K + k]), b, c2);
                            c3 = _mm256_fmadd_ps(_mm256_set1_ps(A[(size_t)(i + 3) * K + k]), b, c3);
                        }
                        _mm256_storeu_ps(C + (size_t)i * N + j, _mm256_add_ps(_mm256_loadu_ps(C + (size_t)i * N + j), c0));
                        _mm256_storeu_ps(C + (size_t)(i + 1) * N + j, _mm256_add_ps(_mm256_loadu_ps(C + (size_t)(i + 1) * N + j), c1));
                        _mm256_storeu_ps(C + (size_t)(i + 2) * N + j, _mm256_add_ps(_mm256_loadu_ps(C + (size_t)(i + 2) * N + j), c2));
                        _mm256_storeu_ps(C + (size_t)(i + 3) * N + j, _mm256_add_ps(_mm256_loadu_ps(C + (size_t)(i + 3) * N + j), c3));
                    }
                }
                /* MC 块内不满 4 行的尾巴 */
                for (int i = i4; i < imax; i++)
                    for (int j = jj; j < jmax; j++) {
                        for (int k = kk; k < kmax; k++)
                            C[(size_t)i * N + j] += A[(size_t)i * K + k] * B[(size_t)k * N + j];
                    }
            }
        }
    }
    /* 列尾 jn..N-1（最多 7 列） */
    for (int i = i0; i < i1; i++)
        for (int j = jn; j < N; j++) {
            float s = 0;
            for (int k = 0; k < K; k++) s += A[(size_t)i * K + k] * B[(size_t)k * N + j];
            C[(size_t)i * N + j] += s;
        }
}

static void k_block(int M, int N, int K, const float *A, const float *B, float *C)
{
    tiled_range(0, M, N, K, A, B, C);
}

/* ---------------------------------------------------------------- 8. 多线程 */
typedef struct { int i0, i1, N, K; const float *A, *B; float *C; } job_t;

static void *worker(void *arg)
{
    job_t *j = (job_t *)arg;
    tiled_range(j->i0, j->i1, j->N, j->K, j->A, j->B, j->C);
    return NULL;
}

static void k_mt(int T, int M, int N, int K, const float *A, const float *B, float *C)
{
    if (T <= 1) { tiled_range(0, M, N, K, A, B, C); return; }
    pthread_t th[64];
    job_t jobs[64];
    int chunk = (M + T - 1) / T;
    chunk = ((chunk + 3) / 4) * 4;
    for (int t = 0; t < T; t++) {
        int a = t * chunk, b = (t + 1) * chunk;
        if (a > M) a = M;
        if (b > M) b = M;
        jobs[t].i0 = a; jobs[t].i1 = b;
        jobs[t].N = N; jobs[t].K = K; jobs[t].A = A; jobs[t].B = B; jobs[t].C = C;
        pthread_create(&th[t], NULL, worker, &jobs[t]);
    }
    for (int t = 0; t < T; t++) pthread_join(th[t], NULL);
}

/* ---------------------------------------------------------------- 9. 融合 epilogue */
static inline __m256 gelu_v(__m256 x)
{
    const __m256 c = _mm256_set1_ps(0.79788456f);
    __m256 x3 = _mm256_mul_ps(_mm256_mul_ps(x, x), x);
    __m256 inner = _mm256_mul_ps(c, _mm256_add_ps(x, _mm256_mul_ps(_mm256_set1_ps(0.044715f), x3)));
    /* tanh(z) 的 Padé 近似，再夹到 [-1,1]，避免调 libm */
    __m256 z2 = _mm256_mul_ps(inner, inner);
    __m256 num = _mm256_mul_ps(inner, _mm256_add_ps(_mm256_set1_ps(27.f), z2));
    __m256 den = _mm256_add_ps(_mm256_set1_ps(27.f), _mm256_mul_ps(_mm256_set1_ps(9.f), z2));
    __m256 th = _mm256_div_ps(num, den);
    th = _mm256_min_ps(_mm256_max_ps(th, _mm256_set1_ps(-1.f)), _mm256_set1_ps(1.f));
    return _mm256_mul_ps(_mm256_set1_ps(0.5f), _mm256_mul_ps(x, _mm256_add_ps(_mm256_set1_ps(1.f), th)));
}

static void gelu_pass(int M, int N, float *C)
{
    int jn = N & ~7;
    for (int i = 0; i < M; i++) {
        float *r = C + (size_t)i * N;
        int j = 0;
        for (; j < jn; j += 8) _mm256_storeu_ps(r + j, gelu_v(_mm256_loadu_ps(r + j)));
        for (; j < N; j++) r[j] = gelu_s(r[j]);
    }
}

static void k_sep(int M, int N, int K, const float *A, const float *B, const float *bias, float *C)
{
    tiled_range(0, M, N, K, A, B, C);
    int jn = N & ~7;
    for (int i = 0; i < M; i++) {
        float *r = C + (size_t)i * N;
        int j = 0;
        for (; j < jn; j += 8)
            _mm256_storeu_ps(r + j, _mm256_add_ps(_mm256_loadu_ps(r + j), _mm256_loadu_ps(bias + j)));
        for (; j < N; j++) r[j] += bias[j];
    }
    gelu_pass(M, N, C);
}

static void k_fused(int M, int N, int K, const float *A, const float *B, const float *bias, float *C)
{
    int jn = N & ~7;
    for (int ii = 0; ii < M; ii += MC_G) {
        int imax = ii + MC_G < M ? ii + MC_G : M;
        for (int jj = 0; jj < jn; jj += NC_G) {
            int jmax = jj + NC_G < jn ? jj + NC_G : jn;
            int i4 = ii + ((imax - ii) / 4) * 4;
            for (int i = ii; i < i4; i += 4) {
                for (int j = jj; j < jmax; j += 8) {
                    __m256 c0 = _mm256_setzero_ps(), c1 = c0, c2 = c0, c3 = c0;
                    /* k 分块放在 tile 内部：整条 K 累加完才写回，不会覆盖前一块 */
                    for (int kk = 0; kk < K; kk += KC_G) {
                        int kmax = kk + KC_G < K ? kk + KC_G : K;
                        for (int k = kk; k < kmax; k++) {
                            __m256 b = _mm256_loadu_ps(B + (size_t)k * N + j);
                            c0 = _mm256_fmadd_ps(_mm256_set1_ps(A[(size_t)i * K + k]), b, c0);
                            c1 = _mm256_fmadd_ps(_mm256_set1_ps(A[(size_t)(i + 1) * K + k]), b, c1);
                            c2 = _mm256_fmadd_ps(_mm256_set1_ps(A[(size_t)(i + 2) * K + k]), b, c2);
                            c3 = _mm256_fmadd_ps(_mm256_set1_ps(A[(size_t)(i + 3) * K + k]), b, c3);
                        }
                    }
                    __m256 bv = _mm256_loadu_ps(bias + j);
                    c0 = gelu_v(_mm256_add_ps(c0, bv));
                    c1 = gelu_v(_mm256_add_ps(c1, bv));
                    c2 = gelu_v(_mm256_add_ps(c2, bv));
                    c3 = gelu_v(_mm256_add_ps(c3, bv));
                    _mm256_storeu_ps(C + (size_t)i * N + j, c0);
                    _mm256_storeu_ps(C + (size_t)(i + 1) * N + j, c1);
                    _mm256_storeu_ps(C + (size_t)(i + 2) * N + j, c2);
                    _mm256_storeu_ps(C + (size_t)(i + 3) * N + j, c3);
                }
            }
            for (int i = i4; i < imax; i++)
                for (int j = jj; j < jmax; j++) {
                    float s = 0;
                    for (int k = 0; k < K; k++)
                        s += A[(size_t)i * K + k] * B[(size_t)k * N + j];
                    C[(size_t)i * N + j] = gelu_s(s + bias[j]);
                }
        }
    }
    for (int i = 0; i < M; i++)
        for (int j = jn; j < N; j++) {
            float s = 0;
            for (int k = 0; k < K; k++) s += A[(size_t)i * K + k] * B[(size_t)k * N + j];
            C[(size_t)i * N + j] = gelu_s(s + bias[j]);
        }
}

/* ---------------------------------------------------------------- 10. Q8_0 量化 GEMM */
static float *quant_q8(int rows, int cols, const float *X, signed char *Q)
{
    int nb = cols / BS;
    float *d = (float *)xmalloc(sizeof(float) * (size_t)rows * nb);
    for (int r = 0; r < rows; r++)
        for (int b = 0; b < nb; b++) {
            const float *x = X + (size_t)r * cols + b * BS;
            float amax = 0;
            for (int t = 0; t < BS; t++) { float v = fabsf(x[t]); if (v > amax) amax = v; }
            float scale = amax / 127.f;
            d[(size_t)r * nb + b] = scale;
            signed char *q = Q + (size_t)r * cols + b * BS;
            for (int t = 0; t < BS; t++)
                q[t] = (signed char)lrintf(x[t] / (scale > 1e-12f ? scale : 1e-12f));
        }
    return d;
}

static inline float dot_q8(const signed char *a, const signed char *b, float sa, float sb)
{
    __m256i va = _mm256_loadu_si256((const __m256i *)a);
    __m256i vb = _mm256_loadu_si256((const __m256i *)b);
    __m256i ax = _mm256_sign_epi8(va, va);
    __m256i sy = _mm256_sign_epi8(vb, va);
    __m256i p16 = _mm256_maddubs_epi16(ax, sy);   /* 第一参数按无符号解释 */
    __m256i p32 = _mm256_madd_epi16(p16, _mm256_set1_epi16(1));
    __m256 f = _mm256_cvtepi32_ps(p32);
    __m128 lo = _mm256_castps256_ps128(f);
    __m128 hi = _mm256_extractf128_ps(f, 1);
    lo = _mm_add_ps(lo, hi);
    lo = _mm_hadd_ps(lo, lo);
    lo = _mm_hadd_ps(lo, lo);
    return _mm_cvtss_f32(lo) * sa * sb;
}

/* B 需要转置成 [N][K]，这样每条权重的 32 个字节是连续的 */
static void int8_gemm(int M, int N, int K, const signed char *qa, const float *da,
                      const signed char *qbT, const float *dbT, float *C)
{
    int nb = K / BS;
    for (int i = 0; i < M; i++) {
        const signed char *ai = qa + (size_t)i * K;
        const float *di = da + (size_t)i * nb;
        for (int j = 0; j < N; j++) {
            const signed char *bj = qbT + (size_t)j * K;
            const float *dj = dbT + (size_t)j * nb;
            float acc = 0;
            for (int b = 0; b < nb; b++)
                acc += dot_q8(ai + b * BS, bj + b * BS, di[b], dj[b]);
            C[(size_t)i * N + j] = acc;
        }
    }
}

/* ---------------------------------------------------------------- 峰值 FMA */
static double peak_gflops(int iters)
{
    volatile float sink = 0;
    __m256 a0 = _mm256_set1_ps(1.000001f), a1 = _mm256_set1_ps(1.000002f);
    __m256 c0 = _mm256_set1_ps(0.f), c1 = c0, c2 = c0, c3 = c0, c4 = c0;
    __m256 d0 = c0, d1 = c0, d2 = c0, d3 = c0, d4 = c0;
    double t0 = now_ms();
    for (int i = 0; i < iters; i++) {
        c0 = _mm256_fmadd_ps(a0, a1, c0); d0 = _mm256_fmadd_ps(a1, a0, d0);
        c1 = _mm256_fmadd_ps(a0, a1, c1); d1 = _mm256_fmadd_ps(a1, a0, d1);
        c2 = _mm256_fmadd_ps(a0, a1, c2); d2 = _mm256_fmadd_ps(a1, a0, d2);
        c3 = _mm256_fmadd_ps(a0, a1, c3); d3 = _mm256_fmadd_ps(a1, a0, d3);
        c4 = _mm256_fmadd_ps(a0, a1, c4); d4 = _mm256_fmadd_ps(a1, a0, d4);
    }
    double t1 = now_ms();
    c0 = _mm256_add_ps(_mm256_add_ps(_mm256_add_ps(c0, c1), _mm256_add_ps(c2, c3)), c4);
    d0 = _mm256_add_ps(_mm256_add_ps(_mm256_add_ps(d0, d1), _mm256_add_ps(d2, d3)), d4);
    c0 = _mm256_add_ps(c0, d0);
    float tmp[8];
    _mm256_storeu_ps(tmp, c0);
    for (int i = 0; i < 8; i++) sink += tmp[i];
    if (sink == 12345.678f) printf("#");
    return (double)iters * 10.0 * 16.0 / (t1 - t0) / 1e6;
}

/* ---------------------------------------------------------------- 驱动 */
typedef void (*kern_t)(int, int, int, const float *, const float *, float *);

static void emit(const char *kn, int M, int N, int K, int T, int r, double ms,
                 const float *C, const double *R, int pad)
{
    double gf = 2.0 * M * N * K / 1e9 / (ms / 1000.0);
    double gb = 4.0 * ((double)M * K + (double)K * N + (double)M * N) / 1e9 / (ms / 1000.0);
    printf("R|%s|%d|%d|%d|%d|%d|%.3f|%.2f|%.2f|%.3e\n", kn, M, N, K, T, r, ms, gf, gb,
           R ? max_err(M, N, C, R) : 0.0);
    (void)pad;
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <mode>\n", argv[0]); return 2; }
    const char *mode = argv[1];
    if (getenv("MC")) MC_G = atoi(getenv("MC"));
    if (getenv("KC")) KC_G = atoi(getenv("KC"));
    if (getenv("NC")) NC_G = atoi(getenv("NC"));

    if (!strcmp(mode, "peak")) {
        printf("PEAK|%.2f\n", peak_gflops(argc > 2 ? atoi(argv[2]) : 20000000));
        return 0;
    }

    if (!strcmp(mode, "bench")) {
        const char *kn = argv[2];
        int M = atoi(argv[3]), N = atoi(argv[4]), K = atoi(argv[5]);
        int T = argc > 6 ? atoi(argv[6]) : 1;
        int reps = argc > 7 ? atoi(argv[7]) : 5;
        int pad = ((M + 7) / 8) * 8;
        float *A = (float *)xmalloc(sizeof(float) * (size_t)pad * K);
        float *B = (float *)xmalloc(sizeof(float) * (size_t)K * N);
        float *C = (float *)xmalloc(sizeof(float) * (size_t)pad * N);
        double *R = (double *)xmalloc(sizeof(double) * (size_t)pad * N);
        fill_mat(A, (size_t)M * K, 12345u);
        fill_mat(B, (size_t)K * N, 67890u);
        ref_gemm(M, N, K, A, B, R);

        static const struct { const char *n; kern_t f; } tbl[] = {
            {"naive", k_naive}, {"ikj", k_ikj}, {"simd1", k_simd1},
            {"t1", k_t1}, {"t2", k_t2}, {"t4", k_t4}, {"t6", k_t6}, {"t8", k_t8},
            {"t4u2", k_t4u2}, {"asm", k_asm}, {"block", k_block}, {NULL, NULL}
        };
        for (int e = 0; tbl[e].n; e++) {
            if (strcmp(tbl[e].n, kn)) continue;
            for (int r = 0; r < reps; r++) {
                memset(C, 0, sizeof(float) * (size_t)pad * N);
                double t0 = now_ms();
                tbl[e].f(M, N, K, A, B, C);
                emit(kn, M, N, K, T, r, now_ms() - t0, C, R, pad);
            }
            return 0;
        }
        if (!strcmp(kn, "mt")) {
            for (int r = 0; r < reps; r++) {
                memset(C, 0, sizeof(float) * (size_t)pad * N);
                double t0 = now_ms();
                k_mt(T, M, N, K, A, B, C);
                emit("mt", M, N, K, T, r, now_ms() - t0, C, R, pad);
            }
            return 0;
        }
        fprintf(stderr, "no such kernel: %s\n", kn);
        return 2;
    }

    if (!strcmp(mode, "block")) {
        int M = atoi(argv[2]), N = atoi(argv[3]), K = atoi(argv[4]), reps = atoi(argv[5]);
        int kcs[] = {64, 128, 256, 512, 1024};
        int ncs[] = {64, 128, 256, 512, 1024};
        int pad = ((M + 7) / 8) * 8;
        float *A = (float *)xmalloc(sizeof(float) * (size_t)pad * K);
        float *B = (float *)xmalloc(sizeof(float) * (size_t)K * N);
        float *C = (float *)xmalloc(sizeof(float) * (size_t)pad * N);
        double *R = (double *)xmalloc(sizeof(double) * (size_t)pad * N);
        fill_mat(A, (size_t)M * K, 12345u);
        fill_mat(B, (size_t)K * N, 67890u);
        ref_gemm(M, N, K, A, B, R);
        for (int mc = 32; mc <= 128; mc *= 2)
            for (int a = 0; a < 5; a++)
                for (int b = 0; b < 5; b++) {
                    MC_G = mc; KC_G = kcs[a]; NC_G = ncs[b];
                    double best = 1e30;
                    for (int r = 0; r < reps; r++) {
                        memset(C, 0, sizeof(float) * (size_t)pad * N);
                        double t0 = now_ms();
                        k_block(M, N, K, A, B, C);
                        double d = now_ms() - t0;
                        if (d < best) best = d;
                    }
                    double gf = 2.0 * M * N * K / 1e9 / (best / 1000.0);
                    printf("B|%d|%d|%d|%.3f|%.2f|%.3e\n", MC_G, KC_G, NC_G, best, gf, max_err(M, N, C, R));
                    fflush(stdout);
                }
        return 0;
    }

    if (!strcmp(mode, "fusion")) {
        int M = atoi(argv[2]), N = atoi(argv[3]), K = atoi(argv[4]), reps = atoi(argv[5]);
        int pad = ((M + 7) / 8) * 8;
        float *A = (float *)xmalloc(sizeof(float) * (size_t)pad * K);
        float *B = (float *)xmalloc(sizeof(float) * (size_t)K * N);
        float *C = (float *)xmalloc(sizeof(float) * (size_t)pad * N);
        float *C2 = (float *)xmalloc(sizeof(float) * (size_t)pad * N);
        float *bias = (float *)xmalloc(sizeof(float) * N);
        fill_mat(A, (size_t)M * K, 12345u);
        fill_mat(B, (size_t)K * N, 67890u);
        for (int j = 0; j < N; j++) bias[j] = 0.002f * j - 0.5f;
        double ts = 1e30, tf = 1e30;
        for (int r = 0; r < reps; r++) {
            memset(C, 0, sizeof(float) * (size_t)pad * N);
            double t0 = now_ms();
            k_sep(M, N, K, A, B, bias, C);
            double d = now_ms() - t0;
            if (d < ts) ts = d;
        }
        for (int r = 0; r < reps; r++) {
            memset(C2, 0, sizeof(float) * (size_t)pad * N);
            double t0 = now_ms();
            k_fused(M, N, K, A, B, bias, C2);
            double d = now_ms() - t0;
            if (d < tf) tf = d;
        }
        double err = 0;
        for (size_t t = 0; t < (size_t)M * N; t++) {
            double d = fabs((double)C[t] - C2[t]);
            if (d > err) err = d;
        }
        printf("F|%d|%d|%d|%.3f|%.3f|%.2f|%.2f|%.3e\n", M, N, K, ts, tf,
               2.0 * M * N * K / 1e9 / (ts / 1000.0), 2.0 * M * N * K / 1e9 / (tf / 1000.0), err);
        return 0;
    }

    if (!strcmp(mode, "int8")) {
        int M = atoi(argv[2]), N = atoi(argv[3]), K = atoi(argv[4]), reps = atoi(argv[5]);
        int pad = ((M + 7) / 8) * 8;
        float *A = (float *)xmalloc(sizeof(float) * (size_t)pad * K);
        float *B = (float *)xmalloc(sizeof(float) * (size_t)K * N);
        float *BT = (float *)xmalloc(sizeof(float) * (size_t)N * K);
        float *C = (float *)xmalloc(sizeof(float) * (size_t)pad * N);
        double *R = (double *)xmalloc(sizeof(double) * (size_t)pad * N);
        fill_mat(A, (size_t)M * K, 12345u);
        fill_mat(B, (size_t)K * N, 67890u);
        for (int k = 0; k < K; k++)
            for (int j = 0; j < N; j++) BT[(size_t)j * K + k] = B[(size_t)k * N + j];
        ref_gemm(M, N, K, A, B, R);
        signed char *qa = (signed char *)xmalloc((size_t)M * K);
        signed char *qb = (signed char *)xmalloc((size_t)N * K);
        float *da = quant_q8(M, K, A, qa);
        float *db = quant_q8(N, K, BT, qb);
        for (int r = 0; r < reps; r++) {
            memset(C, 0, sizeof(float) * (size_t)pad * N);
            double t0 = now_ms();
            int8_gemm(M, N, K, qa, da, qb, db, C);
            double ms = now_ms() - t0;
            /* 相对误差：按 |C| 的 RMS 归一 */
            double se = 0, sr = 0;
            for (size_t t = 0; t < (size_t)M * N; t++) {
                double d = (double)C[t] - R[t];
                se += d * d; sr += R[t] * R[t];
            }
            double rel = sqrt(se / (sr > 0 ? sr : 1));
            printf("Q|%d|%d|%d|%d|%.3f|%.2f|%.3e|%.4f\n", M, N, K, r, ms,
                   2.0 * M * N * K / 1e9 / (ms / 1000.0), max_err(M, N, C, R), rel);
            fflush(stdout);
            (void)pad;
        }
        return 0;
    }

    fprintf(stderr, "unknown mode %s\n", mode);
    return 2;
}
