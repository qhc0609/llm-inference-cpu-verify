/* 10_missrate.c - count Cache misses in software, the way perf would.
 *
 * The guest has no vPMU: `perf stat` reports <not supported> for every hardware
 * event (cycles, instructions, cache-references, cache-misses). So instead of
 * reading counters we reproduce them: generate exactly the address stream each
 * access pattern produces, feed it through an LRU set-associative model of this
 * machine's L1d / L2 / L3, and count misses.
 *
 * 3C split ("full-associative equivalent", the usual textbook method):
 *   compulsory = distinct lines touched
 *   capacity   = fully associative misses - compulsory
 *   conflict   = real misses - fully associative misses
 *
 * Line size 64 B, geometries read from this machine:
 *   L1d 32 KB  8-way (64 sets)
 *   L2 256 KB  4-way (1024 sets)
 *   L3   6 MB 12-way (8192 sets)
 */
#include "common.h"

typedef struct {
    const char *name;
    int sets, ways;
    int nlines;
    int *tag;
    int *prev, *next;
    int *head, *tail;
    int *count;
    int *ht;
    int htsz;
    int live, tomb;      /* probe-table occupancy, triggers a rebuild */
} cache_t;

static inline uint32_t mix32(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return (uint32_t)x;
}

static void cache_init(cache_t *c, const char *name, int sets, int ways)
{
    c->name = name;
    c->sets = sets;
    c->ways = ways;
    c->nlines = sets * ways;
    c->tag = calloc((size_t)c->nlines, sizeof(int));
    c->prev = malloc((size_t)c->nlines * sizeof(int));
    c->next = malloc((size_t)c->nlines * sizeof(int));
    c->head = malloc((size_t)sets * sizeof(int));
    c->tail = malloc((size_t)sets * sizeof(int));
    c->count = calloc((size_t)sets, sizeof(int));
    for (int s = 0; s < sets; s++) { c->head[s] = -1; c->tail[s] = -1; }
    for (int i = 0; i < c->nlines; i++) { c->prev[i] = -1; c->next[i] = -1; }
    c->htsz = 1;
    while (c->htsz < c->nlines * 2) c->htsz <<= 1;
    c->ht = calloc((size_t)c->htsz, sizeof(int));
    c->live = c->tomb = 0;
}

/* Rebuild the probe table once tombstones pile up: with linear probing a table
 * that is all tombstones makes every lookup scan to the end, which turns an
 * 8-million-access trace into minutes. */
static void ht_rebuild(cache_t *c)
{
    memset(c->ht, 0, (size_t)c->htsz * sizeof(int));
    c->live = c->tomb = 0;
    for (int s = 0; s < c->sets; s++)
        for (int k = 0; k < c->count[s]; k++) {
            int line = s * c->ways + k;
            uint32_t h = mix32(((uint64_t)(uint32_t)s << 32) | (uint32_t)c->tag[line]) & (uint32_t)(c->htsz - 1);
            while (c->ht[h] > 0) h = (h + 1) & (uint32_t)(c->htsz - 1);
            c->ht[h] = line + 1;
            c->live++;
        }
}

static void cache_free(cache_t *c)
{
    free(c->tag); free(c->prev); free(c->next);
    free(c->head); free(c->tail); free(c->count); free(c->ht);
}

static inline int ht_find(cache_t *c, int s, int tag)
{
    uint32_t h = mix32(((uint64_t)(uint32_t)s << 32) | (uint32_t)tag) & (uint32_t)(c->htsz - 1);
    int guard = 0;
    while (c->ht[h] && guard++ < c->htsz) {
        int e = c->ht[h];
        if (e > 0) {
            int line = e - 1;
            if (line / c->ways == s && c->tag[line] == tag) return line;
        }
        h = (h + 1) & (uint32_t)(c->htsz - 1);
    }
    return -1;
}

static inline void ht_put(cache_t *c, int s, int tag, int line)
{
    if ((c->live + c->tomb) * 3 > c->htsz * 2) ht_rebuild(c);
    uint32_t h = mix32(((uint64_t)(uint32_t)s << 32) | (uint32_t)tag) & (uint32_t)(c->htsz - 1);
    while (c->ht[h] > 0) h = (h + 1) & (uint32_t)(c->htsz - 1);
    c->ht[h] = line + 1;
    c->live++;
}

static inline void lru_unlink(cache_t *c, int s, int line)
{
    int p = c->prev[line], n = c->next[line];
    if (p >= 0) c->next[p] = n; else c->head[s] = n;
    if (n >= 0) c->prev[n] = p; else c->tail[s] = p;
    c->prev[line] = c->next[line] = -1;
}

static inline void lru_append(cache_t *c, int s, int line)
{
    c->prev[line] = c->tail[s];
    c->next[line] = -1;
    if (c->tail[s] >= 0) c->next[c->tail[s]] = line; else c->head[s] = line;
    c->tail[s] = line;
}

/* returns 1 on miss, 0 on hit */
static inline int cache_access(cache_t *c, uint64_t addr)
{
    int s = (int)((addr >> 6) % (uint32_t)c->sets);
    int tag = (int)((addr >> 6) / (uint32_t)c->sets);
    int line = ht_find(c, s, tag);
    if (line >= 0) {
        lru_unlink(c, s, line);
        lru_append(c, s, line);
        return 0;
    }
    if (c->count[s] < c->ways) {
        line = s * c->ways + c->count[s]++;
    } else {
        line = c->head[s];
        int oldtag = c->tag[line];
        uint32_t h = mix32(((uint64_t)(uint32_t)s << 32) | (uint32_t)oldtag) & (uint32_t)(c->htsz - 1);
        int guard = 0;
        while (c->ht[h] && guard++ < c->htsz) {
            if (c->ht[h] - 1 == line) { c->ht[h] = -1; c->live--; c->tomb++; break; }
            h = (h + 1) & (uint32_t)(c->htsz - 1);
        }
        lru_unlink(c, s, line);
    }
    c->tag[line] = tag;
    ht_put(c, s, tag, line);
    lru_append(c, s, line);
    return 1;
}

typedef struct {
    cache_t l1, l2, l3, l1_fa;
    uint64_t acc, d1miss, l2miss, l3miss, fa_miss;
    int *seen;
    int seensz;
    uint64_t distinct;
} hier_t;

static void hier_init(hier_t *h)
{
    cache_init(&h->l1, "L1d", 64, 8);
    cache_init(&h->l2, "L2", 1024, 4);
    cache_init(&h->l3, "L3", 8192, 12);
    cache_init(&h->l1_fa, "L1d-fa", 1, 512);   /* same 32 KB, fully associative */
    h->acc = h->d1miss = h->l2miss = h->l3miss = h->fa_miss = 0;
    h->seensz = 1 << 22;
    h->seen = calloc((size_t)h->seensz, sizeof(int));
    h->distinct = 0;
}

static void hier_free(hier_t *h)
{
    cache_free(&h->l1); cache_free(&h->l2); cache_free(&h->l3);
    cache_free(&h->l1_fa); free(h->seen);
}

static inline void note_line(hier_t *h, uint64_t addr)
{
    uint64_t line = addr >> 6;
    uint32_t p = mix32(line * 2 + 1) & (uint32_t)(h->seensz - 1);
    while (h->seen[p] && (uint64_t)(h->seen[p] - 1) != line)
        p = (p + 1) & (uint32_t)(h->seensz - 1);
    if (!h->seen[p]) { h->seen[p] = (int)line + 1; h->distinct++; }
}

static inline void feed(hier_t *h, uint64_t addr)
{
    h->acc++;
    note_line(h, addr);
    h->fa_miss += (uint64_t)cache_access(&h->l1_fa, addr);
    if (!cache_access(&h->l1, addr)) return;
    h->d1miss++;
    if (!cache_access(&h->l2, addr)) return;
    h->l2miss++;
    if (!cache_access(&h->l3, addr)) return;
    h->l3miss++;
}

static void case_seq(hier_t *h, size_t bytes, int passes)
{
    for (int p = 0; p < passes; p++)
        for (size_t off = 0; off < bytes; off += 4)
            feed(h, off);
}

static void case_stride(hier_t *h, size_t bytes, size_t step, uint64_t n)
{
    size_t slots = bytes / step;
    for (uint64_t i = 0; i < n; i++)
        feed(h, (i % slots) * step);
}

static void case_aos(hier_t *h, uint64_t n)      /* 16 B records, key only */
{
    for (uint64_t i = 0; i < n; i++) feed(h, i * 16);
}

static void case_soa(hier_t *h, uint64_t n)      /* key column on its own */
{
    for (uint64_t i = 0; i < n; i++) feed(h, i * 4);
}

static void case_gemm_ijk(hier_t *h, int n)      /* B walked down a column */
{
    size_t row = (size_t)n * 4;
    size_t mat = row * (size_t)n;                /* A and B are separate arrays */
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++)
            for (int k = 0; k < n; k++) {
                feed(h, (size_t)i * row + (size_t)k * 4);
                feed(h, mat + (size_t)k * row + (size_t)j * 4);
            }
}

static void case_gemm_tiled(hier_t *h, int n, int bs)
{
    size_t row = (size_t)n * 4;
    size_t mat = row * (size_t)n;
    for (int ii = 0; ii < n; ii += bs)
        for (int kk = 0; kk < n; kk += bs)
            for (int jj = 0; jj < n; jj += bs)
                for (int i = ii; i < ii + bs && i < n; i++)
                    for (int k = kk; k < kk + bs && k < n; k++) {
                        feed(h, (size_t)i * row + (size_t)k * 4);
                        for (int j = jj; j < jj + bs && j < n; j++)
                            feed(h, mat + (size_t)k * row + (size_t)j * 4);
                    }
}

static void run_case(const char *name, int kind, size_t a, int b)
{
    hier_t h;
    hier_init(&h);
    switch (kind) {
    case 0: case_seq(&h, a, b); break;
    case 1: case_stride(&h, a, 64, 8000000ull); break;
    case 2: case_aos(&h, (uint64_t)a); break;
    case 3: case_soa(&h, (uint64_t)a); break;
    case 4: case_gemm_ijk(&h, (int)a); break;
    case 5: case_gemm_tiled(&h, (int)a, b); break;
    }
    double d1 = 100.0 * (double)h.d1miss / (double)h.acc;
    double l2 = h.d1miss ? 100.0 * (double)h.l2miss / (double)h.d1miss : 0.0;
    double l3 = h.l2miss ? 100.0 * (double)h.l3miss / (double)h.l2miss : 0.0;
    double dram = (double)h.l3miss * 64.0 / 1048576.0;
    printf("RES|missrate|%s|%llu|%.2f|%.2f|%.2f|%.1f\n",
           name, (unsigned long long)h.acc, d1, l2, l3, dram);
    uint64_t comp = h.distinct;
    uint64_t cap = h.fa_miss > comp ? h.fa_miss - comp : 0;
    uint64_t conf = h.d1miss > h.fa_miss ? h.d1miss - h.fa_miss : 0;
    printf("RES|miss3c|%s|%llu|%llu|%llu\n",
           name, (unsigned long long)comp,
           (unsigned long long)cap, (unsigned long long)conf);
    hier_free(&h);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("RES|missrate_meta|line=64|l1=32K/8way|L2=256K/4way|L3=6M/12way\n");
    int only = argc > 1 ? atoi(argv[1]) : -1;
    if (only < 0 || only == 0) run_case("seq_1MiB_x8",    0, 1u << 20,  8);
    if (only < 0 || only == 1) run_case("seq_64MiB_x1",   0, 64u << 20, 1);
    if (only < 0 || only == 2) run_case("stride_64B",     1, 64u << 20, 0);
    if (only < 0 || only == 3) run_case("aos_key_8M",     2, 8000000,   0);
    if (only < 0 || only == 4) run_case("soa_key_8M",     3, 8000000,   0);
    if (only < 0 || only == 5) run_case("gemm_ijk_256",   4, 256,       0);
    if (only < 0 || only == 6) run_case("gemm_tiled_256", 5, 256,      64);
    return 0;
}
