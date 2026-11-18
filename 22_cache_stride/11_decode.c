/* 11_decode.c - the memory bill of a decode step.
 *
 * A single-token decode pass reads every weight once and does almost no math in
 * between, so its speed is set by bytes, not FLOPs. This measures that: stream a
 * weight-sized buffer per "token" and see how many tokens per second the memory
 * system allows.
 *
 * Sizes: Qwen2.5-0.5B weights = 638.7 MiB. int8 = 1/4 of the bytes, int4 = 1/8,
 * so the same pass over a quantised copy reads proportionally less.
 * A 4 MiB model is included as the control: it fits in L3, so it stops being a
 * memory problem at all.
 */
#include "common.h"

#define MIB (1024.0 * 1024.0)

static double gb_per_s(size_t bytes, uint64_t ns)
{
    return (double)bytes / ((double)ns / 1e9) / 1e9;
}

static uint64_t scan_u32(const uint32_t *a, size_t words)
{
    uint64_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    size_t i = 0;
    for (; i + 4 <= words; i += 4) {
        s0 += a[i];
        s1 += a[i + 1];
        s2 += a[i + 2];
        s3 += a[i + 3];
    }
    for (; i < words; i++) s0 += a[i];
    return s0 + s1 + s2 + s3;
}

static void case_weights(const char *name, const uint32_t *w, double mib,
                         int tokens)
{
    size_t words = (size_t)(mib * MIB) / 4;
    uint64_t t[9];
    keep(scan_u32(w, words));                       /* warm up */
    for (int k = 0; k < tokens; k++) {
        uint64_t t0 = now_ns();
        keep(scan_u32(w, words));
        t[k] = now_ns() - t0;
    }
    for (int i = 1; i < tokens; i++)                /* insertion sort, small n */
        for (int j = i; j > 0 && t[j - 1] > t[j]; j--) {
            uint64_t tmp = t[j]; t[j] = t[j - 1]; t[j - 1] = tmp;
        }
    uint64_t med = t[tokens / 2];
    double ms = (double)med / 1e6;
    double bytes = (double)words * 4.0;
    printf("RES|decode|%s|%.1f|%.2f|%.1f|%.2f\n", name, bytes / MIB, ms,
           1000.0 / ms, gb_per_s((size_t)bytes, med));
}

int main(void)
{
    const double BIG = 638.7;                       /* MiB, fp32 weights */
    uint32_t *w = xalloc((size_t)(BIG * MIB));
    for (size_t i = 0; i < (size_t)(BIG * MIB) / 4; i++)
        w[i] = (uint32_t)(i * 2654435761u);

    printf("RES|decode_meta|budget=638.7MiB|rounds=5|median\n");
    case_weights("fp32_full", w, BIG, 5);
    case_weights("int8", w, BIG / 4.0, 5);
    case_weights("int4", w, BIG / 8.0, 5);
    case_weights("fits_in_L3", w, 4.0, 5);
    return 0;
}
