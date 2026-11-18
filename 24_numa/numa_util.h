/* numa_util.h - read NUMA facts from the kernel, never from guesses.
 *
 *   node_count()      : how many /sys/devices/system/node/nodeN exist
 *   node_cpus()       : cpu list of one node (parses "0-1,4")
 *   numa_pages()      : pages this process owns per node (parses /proc/self/numa_maps)
 *   pin_cpu()         : hard-pin the calling thread to one logical cpu
 */
#ifndef NUMA_UTIL_H
#define NUMA_UTIL_H

#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int node_count(void)
{
    int n = 0;
    char p[128];
    for (int i = 0; i < 64; i++) {
        snprintf(p, sizeof p, "/sys/devices/system/node/node%d", i);
        if (access(p, F_OK) == 0) n++;
    }
    return n;
}

/* "0-1,4" -> {0,1,4}; returns how many were written */
static int node_cpus(int node, int *out, int max)
{
    char p[128], buf[512];
    snprintf(p, sizeof p, "/sys/devices/system/node/node%d/cpulist", node);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    if (!fgets(buf, sizeof buf, f)) { fclose(f); return 0; }
    fclose(f);
    int n = 0;
    char *s = buf;
    while (*s && n < max) {
        while (*s && !isdigit((unsigned char)*s)) s++;
        if (!*s) break;
        int a = (int)strtol(s, &s, 10), b = a;
        if (*s == '-') { s++; b = (int)strtol(s, &s, 10); }
        for (int c = a; c <= b && n < max; c++) out[n++] = c;
    }
    return n;
}

/* total pages this process has on each node; returns the number of nodes seen */
static int numa_pages(int *out, int maxn)
{
    for (int i = 0; i < maxn; i++) out[i] = 0;
    FILE *f = fopen("/proc/self/numa_maps", "r");
    if (!f) return 0;
    char line[8192];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while ((p = strchr(p, 'N')) != NULL) {
            if (!isdigit((unsigned char)p[1])) { p++; continue; }
            int id = (int)strtol(p + 1, NULL, 10);
            char *eq = strchr(p, '=');
            if (!eq) break;
            long v = strtol(eq + 1, NULL, 10);
            if (id >= 0 && id < maxn) out[id] += (int)v;
            p = eq + 1;
        }
    }
    fclose(f);
    return maxn;
}

static void pin_cpu(int cpu)
{
    cpu_set_t s;
    CPU_ZERO(&s);
    CPU_SET(cpu, &s);
    sched_setaffinity(0, sizeof s, &s);
}

#endif /* NUMA_UTIL_H */
