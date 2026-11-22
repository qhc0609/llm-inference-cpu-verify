/* hot25.c —— 第25篇《CPU计算热点与内存瓶颈定位》实验程序
 *
 * 四个模式，覆盖大纲里的「算子优化 / 绑核 / Cache / 综合」四块：
 *   chase  <kb> <reps>                随机指针追逐，测各级 Cache 的访问延迟
 *   stream <mb> read|write|copy       顺序带宽
 *   gemv   <mb> scalar|avx2|avx2x4|mt:<n>   decode 阶段的核心算子
 *   gemm   <n>  naive|block:<bs>|mt:<n>     prefill 阶段的核心算子
 *
 * 所有模式都打印 checksum，用来确认「优化没把结果算错」。
 * 这台机器没有 vPMU（perf cycles 显示 not supported），所以瓶颈判断只能靠
 * 「时间 × 字节数 × FLOP」三条口径互推，代码里不依赖任何硬件计数器。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <immintrin.h>

static volatile float g_touch;

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/* ---------------- 随机指针追逐 ---------------- */
static int run_chase(size_t kb, long total_accesses)
{
    size_t n = kb * 1024 / sizeof(size_t);
    if (n < 2) n = 2;
    size_t *next = aligned_alloc(64, ((n * sizeof(size_t)) + 63) / 64 * 64);
    size_t *perm = malloc(n * sizeof(size_t));
    if (!next || !perm) { fprintf(stderr, "alloc fail\n"); return 1; }

    for (size_t i = 0; i < n; i++) perm[i] = i;
    srand(20261122);
    for (size_t i = n - 1; i > 0; i--) {
        size_t j = (size_t)(rand() % (int)(i + 1));
        size_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
    }
    /* 按打乱顺序串成一条链：访问 i 之后跳到 perm 里的下一个，等于随机跳 */
    for (size_t i = 0; i < n; i++) next[perm[i]] = perm[(i + 1) % n];

    /* 第二个参数改成「总访问次数」，内部按规模折算圈数：
       否则 64 MB 那档会跑到几分钟，而 4 KB 那档只跑几十毫秒。 */
    long reps = total_accesses / (long)n;
    if (reps < 1) reps = 1;

    size_t idx = perm[0];
    long total = reps * (long)n;
    double t0 = now_ms();
    for (long r = 0; r < reps; r++) {
        for (size_t i = 0; i < n; i++) idx = next[idx];
    }
    double dt = now_ms() - t0;
    printf("CHASE kb=%zu n=%zu reps=%ld ns_per_access=%.3f checksum=%zu\n",
           kb, n, reps, dt * 1e6 / (double)total, idx);
    free(next); free(perm);
    return 0;
}

/* ---------------- 顺序带宽 ---------------- */
typedef struct { const float *a; float *b; size_t from, to; double sink; } stask;
static void *stream_thread(void *p)
{
    stask *t = (stask *)p;
    const uint32_t *p32 = (const uint32_t *)t->a;
    uint32_t s = 0;
    for (size_t i = t->from; i < t->to; i++) s += p32[i];
    t->sink = (double)s;
    return NULL;
}

static int run_stream(int mb, const char *mode, int nt)
{
    size_t n = (size_t)mb * 1024 * 1024 / sizeof(float);
    float *a = aligned_alloc(64, n * sizeof(float));
    float *b = aligned_alloc(64, n * sizeof(float));
    if (!a || !b) { fprintf(stderr, "alloc fail\n"); return 1; }
    for (size_t i = 0; i < n; i++) { a[i] = (float)(i % 97) * 0.01f; b[i] = 0.f; }

    int reps = 10;
    double bytes = 0, t0 = now_ms(), sink = 0;
    if (!strcmp(mode, "read") && nt > 1) {
        pthread_t th[64]; stask t[64];
        size_t chunk = n / nt;
        for (int i = 0; i < nt; i++) {
            t[i].a = a; t[i].b = b; t[i].from = (size_t)i * chunk;
            t[i].to = (i == nt - 1) ? n : (size_t)(i + 1) * chunk;
        }
        for (int i = 0; i < nt; i++) pthread_create(&th[i], NULL, stream_thread, &t[i]);
        for (int i = 0; i < nt; i++) { pthread_join(th[i], NULL); sink += t[i].sink; }
        bytes = (double)n * sizeof(float);
        reps = 1;
    } else if (!strcmp(mode, "read")) {
        const uint32_t *p32 = (const uint32_t *)a;
        for (int r = 0; r < reps; r++) {
            uint32_t s = 0;
            for (size_t i = 0; i < n; i++) s += p32[i];
            sink += (double)s;
        }
        bytes = (double)n * sizeof(float) * reps;
    } else if (!strcmp(mode, "write")) {
        for (int r = 0; r < reps; r++)
            for (size_t i = 0; i < n; i++) b[i] = (float)r;
        bytes = (double)n * sizeof(float) * reps;
    } else if (!strcmp(mode, "copy")) {
        for (int r = 0; r < reps; r++)
            for (size_t i = 0; i < n; i++) b[i] = a[i];
        bytes = (double)n * sizeof(float) * 2 * reps;
    } else {
        fprintf(stderr, "bad stream mode\n");
        return 2;
    }
    double dt = now_ms() - t0;
    printf("STREAM mb=%d mode=%s mt=%d ms=%.1f gbs=%.2f checksum=%.1f\n",
           mb, mode, nt, dt, bytes / (dt / 1e3) / 1e9, sink + b[0]);
    free(a); free(b);
    return 0;
}

/* ---------------- GEMV：三种实现 ---------------- */
typedef struct {
    const float *w;
    float       *out;
    const float *x;
    int          K;
    size_t       from, to;
    int          reps;
} gtask;

static float gemv_scalar(size_t n, const float *w, const float *x, int K)
{
    g_touch = (float)n;   /* volatile 写：防止编译器把重复调用合并成一次 */
    const int mask = K - 1;
    float acc = 0.f;
    for (size_t i = 0; i < n; i++) acc += w[i] * x[i & mask];
    return acc;
}

static float gemv_avx2(size_t n, const float *w, const float *x, int K)
{
    g_touch = (float)n;   /* volatile 写：防止编译器把重复调用合并成一次 */
    const int mask = K - 1;
    __m256 acc = _mm256_setzero_ps();
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256 xv = _mm256_loadu_ps(x + (i & mask));
        __m256 wv = _mm256_loadu_ps(w + i);
        acc = _mm256_fmadd_ps(wv, xv, acc);
    }
    float buf[8];
    _mm256_storeu_ps(buf, acc);
    float s = 0.f;
    for (int k = 0; k < 8; k++) s += buf[k];
    for (; i < n; i++) s += w[i] * x[i & mask];
    return s;
}

static float gemv_avx2x4(size_t n, const float *w, const float *x, int K)
{
    g_touch = (float)n;   /* volatile 写：防止编译器把重复调用合并成一次 */
    const int mask = K - 1;
    __m256 a0 = _mm256_setzero_ps(), a1 = _mm256_setzero_ps();
    __m256 a2 = _mm256_setzero_ps(), a3 = _mm256_setzero_ps();
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        a0 = _mm256_fmadd_ps(_mm256_loadu_ps(w + i),      _mm256_loadu_ps(x + (i & mask)),      a0);
        a1 = _mm256_fmadd_ps(_mm256_loadu_ps(w + i + 8),  _mm256_loadu_ps(x + ((i + 8) & mask)),  a1);
        a2 = _mm256_fmadd_ps(_mm256_loadu_ps(w + i + 16), _mm256_loadu_ps(x + ((i + 16) & mask)), a2);
        a3 = _mm256_fmadd_ps(_mm256_loadu_ps(w + i + 24), _mm256_loadu_ps(x + ((i + 24) & mask)), a3);
    }
    a0 = _mm256_add_ps(_mm256_add_ps(a0, a1), _mm256_add_ps(a2, a3));
    float buf[8];
    _mm256_storeu_ps(buf, a0);
    float s = 0.f;
    for (int k = 0; k < 8; k++) s += buf[k];
    for (; i < n; i++) s += w[i] * x[i & mask];
    return s;
}

static void *gemv_thread(void *p)
{
    gtask *t = (gtask *)p;
    size_t n = t->to - t->from;
    float acc = 0.f;
    for (int r = 0; r < t->reps; r++)
        acc += gemv_avx2x4(n, t->w + t->from, t->x, t->K);
    t->out[0] = acc;
    return NULL;
}

static int run_gemv(int mb, const char *variant)
{
    int K = 8192;
    size_t n = (size_t)mb * 1024 * 1024 / sizeof(float);
    n -= n % 32;
    float *w = aligned_alloc(64, n * sizeof(float) + 64);
    float *x = aligned_alloc(64, (K + 16) * sizeof(float));
    if (!w || !x) { fprintf(stderr, "alloc fail\n"); return 1; }
    srand(4242);
    for (size_t i = 0; i < n; i++) w[i] = (float)((rand() % 200) - 100) * 0.001f;
    for (int i = 0; i < K + 16; i++) x[i] = (float)((i % 37) * 0.01);

    int reps = 20;
    double bytes = (double)n * sizeof(float) * reps;
    float r = 0;

    if (!strncmp(variant, "mt:", 3)) {
        int nt = atoi(variant + 3);
        if (nt < 1) nt = 1;
        pthread_t th[64];
        gtask t[64];
        float out[64];
        for (int i = 0; i < nt; i++) out[i] = 0.f;

        /* 线程只创建一次：每轮重建线程会把缓存和调度状态都清掉，测出来的不是并行度 */
        size_t chunk = (n / nt) & ~(size_t)31;
        for (int i = 0; i < nt; i++) {
            t[i].w = w; t[i].x = x; t[i].K = K;
            t[i].from = (size_t)i * chunk;
            t[i].to = (i == nt - 1) ? n : (size_t)(i + 1) * chunk;
            t[i].out = &out[i];
            t[i].reps = reps;
        }
        double t0 = now_ms();
        for (int i = 0; i < nt; i++) pthread_create(&th[i], NULL, gemv_thread, &t[i]);
        for (int i = 0; i < nt; i++) { pthread_join(th[i], NULL); r += out[i]; }
        double dt = now_ms() - t0;
        printf("GEMV variant=mt:%d mb=%d n=%zu reps=%d ms=%.2f gbs=%.2f checksum=%.4f\n",
               nt, mb, n, reps, dt, bytes / (dt / 1e3) / 1e9, r);
    } else {
        float (*fn)(size_t, const float *, const float *, int) = gemv_scalar;
        if (!strcmp(variant, "avx2")) fn = gemv_avx2;
        else if (!strcmp(variant, "avx2x4")) fn = gemv_avx2x4;
        fn(n, w, x, K);
        double t0 = now_ms();
        for (int i = 0; i < reps; i++) r += fn(n, w, x, K);
        double dt = now_ms() - t0;
        printf("GEMV variant=%s mb=%d n=%zu reps=%d ms=%.2f gbs=%.2f checksum=%.4f\n",
               variant, mb, n, reps, dt, bytes / (dt / 1e3) / 1e9, r);
    }
    free(w); free(x);
    return 0;
}

/* ---------------- GEMM：朴素 vs 分块 vs 多线程 ---------------- */
static double *gA, *gB, *gC;

static void gemm_naive(int n)
{
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            double s = 0;
            for (int k = 0; k < n; k++) s += gA[i * n + k] * gB[k * n + j];
            gC[i * n + j] = s;
        }
}

static void gemm_block(int n, int BS)
{
    memset(gC, 0, (size_t)n * n * sizeof(double));
    for (int ii = 0; ii < n; ii += BS)
        for (int kk = 0; kk < n; kk += BS)
            for (int jj = 0; jj < n; jj += BS)
                for (int i = ii; i < ii + BS && i < n; i++)
                    for (int k = kk; k < kk + BS && k < n; k++) {
                        double a = gA[i * n + k];
                        for (int j = jj; j < jj + BS && j < n; j++)
                            gC[i * n + j] += a * gB[k * n + j];
                    }
}

typedef struct { int n, from, to, BS, reps; } btask;
static void *gemm_thread(void *p)
{
    btask *t = (btask *)p;
    int n = t->n, BS = t->BS;
    for (int r = 0; r < t->reps; r++)
    for (int ii = t->from; ii < t->to; ii += BS)
        for (int kk = 0; kk < n; kk += BS)
            for (int jj = 0; jj < n; jj += BS)
                for (int i = ii; i < ii + BS && i < n; i++)
                    for (int k = kk; k < kk + BS && k < n; k++) {
                        double a = gA[i * n + k];
                        for (int j = jj; j < jj + BS && j < n; j++)
                            gC[i * n + j] += a * gB[k * n + j];
                    }
    return NULL;
}

static int run_gemm(int n, const char *variant)
{
    size_t sz = (size_t)n * n;
    gA = malloc(sz * sizeof(double));
    gB = malloc(sz * sizeof(double));
    gC = malloc(sz * sizeof(double));
    if (!gA || !gB || !gC) { fprintf(stderr, "alloc fail\n"); return 1; }
    for (size_t i = 0; i < sz; i++) {
        gA[i] = (double)(i % 19) * 0.01;
        gB[i] = (double)(i % 23) * 0.01;
        gC[i] = 0;
    }
    double t0 = now_ms();
    if (!strncmp(variant, "block:", 6)) {
        gemm_block(n, atoi(variant + 6));
    } else if (!strncmp(variant, "mt:", 3)) {
        int nt = atoi(variant + 3);
        if (nt < 1) nt = 1;
        memset(gC, 0, sz * sizeof(double));
        pthread_t th[64];
        btask t[64];
        int rows = n / nt;
        for (int i = 0; i < nt; i++) {
            t[i].n = n; t[i].BS = 64; t[i].reps = 1;
            t[i].from = i * rows;
            t[i].to = (i == nt - 1) ? n : (i + 1) * rows;
            pthread_create(&th[i], NULL, gemm_thread, &t[i]);
        }
        for (int i = 0; i < nt; i++) pthread_join(th[i], NULL);
    } else {
        gemm_naive(n);
    }
    double dt = now_ms() - t0;
    double sum = 0;
    for (size_t i = 0; i < sz; i++) sum += gC[i];
    printf("GEMM variant=%s n=%d ms=%.1f gflops=%.2f checksum=%.3f\n",
           variant, n, dt, 2.0 * n * n * n / (dt / 1e3) / 1e9, sum);
    free(gA); free(gB); free(gC);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "usage:\n  %s chase <kb> <reps>\n  %s stream <mb> read|write|copy\n"
            "  %s gemv <mb> scalar|avx2|avx2x4|mt:<n>\n  %s gemm <n> naive|block:<bs>|mt:<n>\n",
            argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "chase") && argc >= 4)  return run_chase((size_t)atol(argv[2]), atol(argv[3]));
    if (!strcmp(argv[1], "stream") && argc >= 4) {
        int nt = 1;
        for (int i = 4; i < argc; i++) if (!strncmp(argv[i], "mt:", 3)) nt = atoi(argv[i] + 3);
        return run_stream(atoi(argv[2]), argv[3], nt);
    }
    if (!strcmp(argv[1], "gemv") && argc >= 4)   return run_gemv(atoi(argv[2]), argv[3]);
    if (!strcmp(argv[1], "gemm") && argc >= 3)   return run_gemm(atoi(argv[2]), argc >= 4 ? argv[3] : "naive");
    fprintf(stderr, "bad args\n");
    return 2;
}
