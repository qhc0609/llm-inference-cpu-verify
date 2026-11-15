/* 07_layout.c - AoS vs SoA over 8,000,000 records, only the key field is summed.
 *
 * AoS: {int key; float x, y, z;} = 16 B per record, 128 MB touched
 * SoA: key row only = 4 B per record, 32 MB touched
 *
 * output: RES|layout|aos|ms|GB/s ; RES|layout|soa|ms|GB/s
 */
#include "common.h"

#define RECORDS 8000000

typedef struct {
    int key;
    float x, y, z;
} Rec;

int main(void)
{
    size_t aos_bytes = (size_t)RECORDS * sizeof(Rec);
    Rec *recs = (Rec *)xalloc(aos_bytes);
    for (int i = 0; i < RECORDS; i++) {
        recs[i].key = i & 0xffff;
        recs[i].x = (float)i;
        recs[i].y = (float)(i * 2);
        recs[i].z = (float)(i * 3);
    }

    size_t soa_bytes = (size_t)RECORDS * sizeof(int);
    int *keys = (int *)xalloc(soa_bytes);
    for (int i = 0; i < RECORDS; i++)
        keys[i] = recs[i].key;

    uint64_t best[3];

    for (int r = 0; r < 4; r++) {
        uint64_t sum = 0;
        uint64_t t0 = now_ns();
        for (int i = 0; i < RECORDS; i++)
            sum += (uint64_t)recs[i].key;
        uint64_t dt = now_ns() - t0;
        keep(sum);
        if (r > 0)
            best[r - 1] = dt;
    }
    double ms = (double)median3(best[0], best[1], best[2]) / 1e6;
    printf("RES|layout|aos|%zu|%.2f|%.2f\n", aos_bytes, ms,
           (double)aos_bytes / (ms / 1e3) / 1e9);

    for (int r = 0; r < 4; r++) {
        uint64_t sum = 0;
        uint64_t t0 = now_ns();
        for (int i = 0; i < RECORDS; i++)
            sum += (uint64_t)keys[i];
        uint64_t dt = now_ns() - t0;
        keep(sum);
        if (r > 0)
            best[r - 1] = dt;
    }
    ms = (double)median3(best[0], best[1], best[2]) / 1e6;
    printf("RES|layout|soa|%zu|%.2f|%.2f\n", soa_bytes, ms,
           (double)soa_bytes / (ms / 1e3) / 1e9);

    fflush(stdout);
    free(recs);
    free(keys);
    return 0;
}
