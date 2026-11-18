/* 20d_pin.c - what a DMA map costs before a single byte moves.
 *
 * Every streaming DMA mapping has to (a) find the user pages, (b) pin them so
 * the kernel cannot move or reclaim them, (c) program the IOMMU/device with the
 * resulting addresses. On a machine without a device we can still price the
 * part the CPU pays for: mlock() is exactly the pin step (GUP + get_page).   */
#include "common.h"
#include <sys/mman.h>
#include <sys/resource.h>

#define MB 256

static uint64_t time_mlock(void *p, size_t n, int *pages_out) {
    uint64_t best = UINT64_MAX;
    for (int r = 0; r < 3; r++) {
        munlock(p, n);
        uint64_t t0 = now_ns();
        if (mlock(p, n)) { perror("mlock"); exit(1); }
        uint64_t dt = now_ns() - t0;
        if (dt < best) best = dt;
    }
    if (pages_out) {
        FILE *f = fopen("/proc/self/status", "r");
        char line[256]; long kb = 0;
        while (f && fgets(line, sizeof line, f))
            if (sscanf(line, "VmLck: %ld kB", &kb) == 1) break;
        if (f) fclose(f);
        *pages_out = (int)(kb / 4);
    }
    return best;
}

int main(int argc, char **argv) {
    pin_cpu(argc > 1 ? atoi(argv[1]) : 0);
    struct rlimit rl = { RLIM_INFINITY, RLIM_INFINITY };
    setrlimit(RLIMIT_MEMLOCK, &rl);

    size_t n = (size_t)MB << 20;
    long ps = sysconf(_SC_PAGESIZE);
    int npages = 0;

    /* --- 4 KiB pages: the default for an anonymous malloc buffer --------- */
    void *a = xalloc(n);
    int touched = 0;
    for (size_t i = 0; i < n; i += ps) { ((char *)a)[i] = 1; touched++; }
    uint64_t t4k = time_mlock(a, n, &npages);
    RES("pin|4k|mlock|%d|%.2f|%.1f|%d", MB, (double)t4k / 1e6,
        (double)t4k / (double)touched, npages);

    /* --- 2 MiB pages: same bytes, 512x fewer descriptors ------------------ */
    void *b = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (b == MAP_FAILED) { perror("mmap"); return 1; }
    madvise(b, n, MADV_HUGEPAGE);
    for (size_t i = 0; i < n; i += ps) ((char *)b)[i] = 1;
    uint64_t t2m = time_mlock(b, n, &npages);
    int huge = npages;
    RES("pin|2m|mlock|%d|%.2f|%.1f|%d", MB, (double)t2m / 1e6,
        (double)t2m / (double)(n >> 21), huge);

    /* --- per-transfer mapping: 64 KiB at a time, as a naive driver would -- */
    /* This is the pattern that kills throughput: map, kick the device, wait,
     * unmap, repeat. Each round pays the pin again.                         */
    {
        const size_t chunk = 64u << 10;
        int rounds = (int)(n / chunk);
        uint64_t t0 = now_ns();
        for (int i = 0; i < rounds; i++) {
            void *c = (char *)a + (size_t)i * chunk;
            if (mlock(c, chunk)) { perror("mlock chunk"); break; }
            munlock(c, chunk);
        }
        uint64_t dt = now_ns() - t0;
        RES("pin|chunk64k|mappairs|%d|%.1f|%.1f", rounds, (double)dt / 1e6,
            (double)dt / (double)rounds);
    }

    /* --- the same chunking with 2 MiB blocks ------------------------------ */
    {
        const size_t chunk = 2u << 20;
        int rounds = (int)(n / chunk);
        uint64_t t0 = now_ns();
        for (int i = 0; i < rounds; i++) {
            void *c = (char *)b + (size_t)i * chunk;
            if (mlock(c, chunk)) { perror("mlock chunk2m"); break; }
            munlock(c, chunk);
        }
        uint64_t dt = now_ns() - t0;
        RES("pin|chunk2m|mappairs|%d|%.1f|%.1f", rounds, (double)dt / 1e6,
            (double)dt / (double)rounds);
    }
    munlock(a, n); munlock(b, n);
    free(a); munmap(b, n);
    return 0;
}
