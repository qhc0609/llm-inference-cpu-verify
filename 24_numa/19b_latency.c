/* 19b_latency.c - how long does one dependent load take?
 *
 * A random ring is built over a buffer far larger than any cache. Following the
 * pointers makes every load depend on the previous one, so what is measured is
 * the full round trip: core -> cache hierarchy -> memory controller -> DRAM.
 *
 * The buffer is mmap'ed and marked MADV_HUGEPAGE on purpose. Without huge pages
 * 64 MiB is 16384 tiny pages, the TLB cannot hold them, and every single hop
 * pays a page walk; that inflates 100 ns into 200-300 ns and has nothing to do
 * with NUMA. The number of transparent huge pages actually granted is printed
 * with the result, so a bad run is visible instead of silently believed.
 *
 * Usage: ./19b_latency <size_MB> <label>
 * Normally wrapped by numactl so the pages and the cpu belong to chosen nodes:
 *   numactl --membind=1 --cpunodebind=0 ./19b_latency 64 lat_m1c0
 */
#include "common.h"
#include "numa_util.h"
#include <sys/mman.h>

/* AnonHugePages (kB) this process actually got, straight from the kernel */
static long anon_huge_kb(void)
{
    FILE *f = fopen("/proc/self/smaps_rollup", "r");
    if (!f) return -1;
    char line[256];
    long kb = -1;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "AnonHugePages: %ld kB", &kb) == 1) break;
    fclose(f);
    return kb;
}

int main(int argc, char **argv)
{
    size_t mb = (argc > 1) ? (size_t)strtoul(argv[1], NULL, 10) : 64;
    const char *label = (argc > 2) ? argv[2] : "na";
    size_t n = (mb << 20) / sizeof(uint64_t);
    if (n < 1024) n = 1024;
    size_t bytes = n * sizeof(uint64_t);

    uint64_t *a = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (a == MAP_FAILED) { perror("mmap"); return 1; }
    madvise(a, bytes, MADV_HUGEPAGE);

    uint64_t *perm = malloc(n * sizeof(uint64_t));
    if (!perm) return 1;
    for (size_t i = 0; i < n; i++) perm[i] = i;

    unsigned seed = 1234567u;
    for (size_t i = n - 1; i > 0; i--) {
        size_t j = (size_t)(rand_r(&seed) % (unsigned)(i + 1));
        uint64_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
    }
    for (size_t i = 0; i < n; i++)
        a[perm[i]] = (uint64_t)(uintptr_t)&a[perm[(i + 1) % n]];
    free(perm);

    /* warm-up: one full lap, so every page is faulted in before timing */
    uint64_t p = (uint64_t)(uintptr_t)&a[0];
    for (size_t i = 0; i < n; i++) p = *(uint64_t *)p;

    long huge = anon_huge_kb();

    uint64_t v[5];
    for (int r = 0; r < 5; r++) {
        uint64_t t0 = now_ns(), acc = 0;
        for (size_t i = 0; i < n; i++) { p = *(uint64_t *)p; acc += p; }
        uint64_t t1 = now_ns();
        keep(acc);
        v[r] = t1 - t0;
    }
    /* a dependent-load chain is a lower bound: the fastest lap is the honest
     * one, every slower lap was interrupted by something else on the box */
    uint64_t t = v[0];
    for (int r = 1; r < 5; r++) if (v[r] < t) t = v[r];

    int pages[8];
    numa_pages(pages, 8);

    double ns = (double)t / (double)n;
    double gbps = (double)n * 64.0 / (double)t; /* each hop pulls one 64 B line */
    printf("RES|lat|%s|%zu|%.1f|%.3f|%d|%d|%ld\n",
           label, mb, ns, gbps, pages[0], pages[1], huge);
    return 0;
}
