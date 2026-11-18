/* 12_reuse.c - temporal locality, measured as a bandwidth difference.
 *
 * Same number of requested bytes (32 MiB) every time. The only variable is
 * whether the block fits in a given level and is therefore read again from
 * there, or whether every repeat has to go back to DRAM.
 *
 *   tile  x  repeats = 32 MiB requested
 *
 *   32 KiB  x 1024   fits L1
 *  256 KiB  x  128   fits L2
 *    4 MiB  x    8   fits L3
 *   32 MiB  x    1   straight from DRAM
 */
#include "common.h"

#define TOTAL (32.0 * 1024 * 1024)

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

static void case_reuse(const char *name, const uint32_t *a, size_t tile_bytes,
                       int passes)
{
    size_t words = tile_bytes / 4;
    uint64_t t[5];
    keep(scan_u32(a, words));
    for (int r = 0; r < 5; r++) {
        uint64_t t0 = now_ns();
        for (int p = 0; p < passes; p++) keep(scan_u32(a, words));
        t[r] = now_ns() - t0;
    }
    uint64_t med = median3(t[0], t[1], t[2]);
    if (t[3] < t[0] && t[3] < t[4]) med = t[3];
    double ms = (double)med / 1e6;
    printf("RES|reuse|%s|%.0f|%.2f|%.2f\n", name, TOTAL / 1048576.0, ms,
           TOTAL / (ms / 1000.0) / 1e9);
}

int main(void)
{
    uint32_t *a = xalloc((size_t)TOTAL);
    for (size_t i = 0; i < (size_t)TOTAL / 4; i++)
        a[i] = (uint32_t)(i * 2654435761u);

    printf("RES|reuse_meta|requested=32MiB|rounds=5|median\n");
    case_reuse("32KiB_x1024",  a, 32u << 10,        1024);
    case_reuse("256KiB_x128",  a, 256u << 10,       128);
    case_reuse("4MiB_x8",      a, 4u << 20,         8);
    case_reuse("32MiB_x1",     a, 32u << 20,        1);
    return 0;
}
