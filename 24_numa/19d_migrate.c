/* 19d_migrate.c - what does it cost the kernel to move pages between nodes?
 *
 * Allocates 256 MiB under whatever policy numactl gave us, touches it so every
 * page really exists, then asks move_pages() to push it to <to_node> and times
 * the whole migration. Verifies with /proc/self/numa_maps afterwards.
 *
 * Usage: numactl --membind=0 ./19d_migrate <to_node> [MB]
 */
#include "common.h"
#include "numa_util.h"
#include <sys/mman.h>
#include <sys/syscall.h>

#ifndef MPOL_MF_MOVE
#define MPOL_MF_MOVE 2
#endif

static long do_move_pages(pid_t pid, unsigned long n, void **pages,
                          const int *nodes, int *status, int flags)
{
    return syscall(SYS_move_pages, pid, n, pages, nodes, status, flags);
}

int main(int argc, char **argv)
{
    int to = (argc > 1) ? atoi(argv[1]) : 1;
    size_t mb = (argc > 2) ? (size_t)strtoul(argv[2], NULL, 10) : 256;
    size_t bytes = mb << 20;
    size_t npg = bytes / 4096;

    uint64_t *buf = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (buf == MAP_FAILED) { perror("mmap"); return 1; }
    /* ask for huge pages: 256 MiB is 32 huge pages or 65536 small ones, and
     * move_pages() walks every one of them, so the page size decides the price */
    madvise(buf, bytes, MADV_HUGEPAGE);
    for (size_t i = 0; i < bytes / 8; i++) buf[i] = i;   /* fault them in */

    void **pages = malloc(npg * sizeof(void *));
    int *nodes = malloc(npg * sizeof(int));
    int *status = malloc(npg * sizeof(int));
    if (!pages || !nodes || !status) return 1;
    for (size_t i = 0; i < npg; i++) { pages[i] = (char *)buf + i * 4096; nodes[i] = to; }

    int before[8];
    numa_pages(before, 8);

    /* Every timed move must really move: a second move_pages() to the node the
     * pages already sit on is a no-op and would time as ~0. So each round is
     * "reset to the far node untimed, then move to <to> and time that". */
    int far = 1 - to;
    for (size_t i = 0; i < npg; i++) nodes[i] = far;
    do_move_pages(0, npg, pages, nodes, status, MPOL_MF_MOVE);
    for (size_t i = 0; i < npg; i++) nodes[i] = to;

    uint64_t v[3];
    long moved = 0;
    for (int r = 0; r < 3; r++) {
        uint64_t t0 = now_ns();
        moved = do_move_pages(0, npg, pages, nodes, status, MPOL_MF_MOVE);
        uint64_t t1 = now_ns();
        v[r] = t1 - t0;
        if (r < 2) {                      /* put them back on the far node */
            for (size_t i = 0; i < npg; i++) nodes[i] = far;
            do_move_pages(0, npg, pages, nodes, status, MPOL_MF_MOVE);
            for (size_t i = 0; i < npg; i++) nodes[i] = to;
        }
    }
    /* fastest of three: slower rounds are host interference, not migration */
    uint64_t t = v[0];
    for (int r = 1; r < 3; r++) if (v[r] < t) t = v[r];

    int after[8];
    numa_pages(after, 8);
    long huge = -1;
    FILE *sm = fopen("/proc/self/smaps_rollup", "r");
    if (sm) {
        char line[256];
        while (fgets(line, sizeof line, sm))
            if (sscanf(line, "AnonHugePages: %ld kB", &huge) == 1) break;
        fclose(sm);
    }

    double ms = (double)t / 1e6;
    double gbps = (double)bytes / (double)t;
    printf("RES|mig|%zu|to|%d|ms|%.1f|gbps|%.2f|before|%d|%d|after|%d|%d|huge_kb|%ld|ret|%ld\n",
           mb, to, ms, gbps, before[0], before[1], after[0], after[1], huge, moved);
    return 0;
}
