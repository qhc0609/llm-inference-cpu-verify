// gpu02.cu —— 第 02 篇配套微基准：Thread / Warp / Block / Grid 的真实层级关系
// 编译： nvcc -O3 -arch=native -o gpu02 gpu02.cu
// 用法： ./gpu02 <mode>   mode = dev|map|lane|smid|div|ballot|occ|tail|red|launch|ilp
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cuda_runtime.h>
#include <algorithm>
#include <vector>

#define CK(x) do{ cudaError_t e_=(x); if(e_!=cudaSuccess){ \
    printf("CUDA_ERR %s @%d: %s\n", #x, __LINE__, cudaGetErrorString(e_)); exit(1);} }while(0)

__device__ __forceinline__ unsigned get_smid() {
    unsigned r; asm("mov.u32 %0, %%smid;" : "=r"(r)); return r;
}


// ---------- 预热：把 SM 频率拉到稳态，否则计时全是爬坡数据 ----------
__global__ void k_spin(float* o, int iters) {
    float a = threadIdx.x * 1e-6f;
    for (int i = 0; i < iters; ++i) a = fmaf(a, 1.0000001f, 1e-7f);
    if (a == 12345.678f) o[0] = a;
}

static void warm(int ms_target = 400) {
    cudaDeviceProp p; CK(cudaGetDeviceProperties(&p, 0));
    int gd = p.multiProcessorCount * 8, bd = 128;
    float* o; CK(cudaMalloc(&o, sizeof(float)));
    cudaEvent_t a, b; cudaEventCreate(&a); cudaEventCreate(&b);
    cudaEventRecord(a);
    float el = 0;
    int guard = 0;
    while (el < ms_target && guard++ < 4000) {
        k_spin<<<gd, bd>>>(o, 20000);
        cudaEventRecord(b); CK(cudaEventSynchronize(b));
        cudaEventElapsedTime(&el, a, b);
    }
    printf("WARM|ms=%.0f|rounds=%d\n", el, guard);
    CK(cudaFree(o));
}

// ---------- E0 设备属性 ----------
static void mode_dev() {
    int n = 0; CK(cudaGetDeviceCount(&n));
    for (int i = 0; i < n; ++i) {
        cudaDeviceProp p; CK(cudaGetDeviceProperties(&p, i));
        int clk = 0, mclk = 0;
        CK(cudaDeviceGetAttribute(&clk, cudaDevAttrClockRate, i));
        CK(cudaDeviceGetAttribute(&mclk, cudaDevAttrMemoryClockRate, i));
        printf("DEV|idx=%d|name=%s|cc=%d.%d|sm=%d|thr_per_sm=%d|warps_per_sm=%d|"
               "blocks_per_sm=%d|regs_per_sm=%d|smem_per_sm=%zu|shared_per_block=%zu|"
               "warpSize=%d|clk_mhz=%.3f|l2=%d|mem_bus=%d|mem_clk_mhz=%.1f\n",
               i, p.name, p.major, p.minor, p.multiProcessorCount,
               p.maxThreadsPerMultiProcessor, p.maxThreadsPerMultiProcessor / 32,
               p.maxBlocksPerMultiProcessor, p.regsPerMultiprocessor, p.sharedMemPerMultiprocessor,
               p.sharedMemPerBlock, p.warpSize, clk / 1e3, p.l2CacheSize,
               p.memoryBusWidth, mclk / 1e3);
        printf("DEV|idx=%d|maxThreadsPerBlock=%d|maxBlockDimX=%d|regsPerBlock=%d\n",
               i, p.maxThreadsPerBlock, p.maxThreadsDim[0], p.regsPerBlock);
    }
}

// ---------- E1 三层索引 -> 全局线程号 ----------
struct Rec { int bx, by, bz, tx, ty, tz, tpb, gid, wid, lane; };

__global__ void k_map(Rec* out) {
    int tpb = blockDim.x * blockDim.y * blockDim.z;
    int tid = threadIdx.x + threadIdx.y * blockDim.x
            + threadIdx.z * blockDim.x * blockDim.y;
    int bid = blockIdx.x + blockIdx.y * gridDim.x
            + blockIdx.z * gridDim.x * gridDim.y;
    size_t gid = (size_t)bid * tpb + tid;
    Rec r;
    r.bx = blockIdx.x; r.by = blockIdx.y; r.bz = blockIdx.z;
    r.tx = threadIdx.x; r.ty = threadIdx.y; r.tz = threadIdx.z;
    r.tpb = tpb; r.gid = (int)gid; r.wid = tid / 32; r.lane = tid & 31;
    out[gid] = r;
}

static void mode_map() {
    dim3 blk(16, 16, 1), grd(2, 2, 1);
    int total = blk.x * blk.y * blk.z * grd.x * grd.y * grd.z;
    Rec* d = nullptr; CK(cudaMalloc(&d, sizeof(Rec) * total));
    k_map<<<grd, blk>>>(d); CK(cudaDeviceSynchronize());
    std::vector<Rec> h(total); CK(cudaMemcpy(h.data(), d, sizeof(Rec) * total, cudaMemcpyDeviceToHost));
    printf("MAP|blockDim=(%d,%d,%d)|gridDim=(%d,%d,%d)|threadsPerBlock=%d|totalThreads=%d|"
           "warpsPerBlock=%d|totalWarps=%d\n",
           blk.x, blk.y, blk.z, grd.x, grd.y, grd.z, blk.x * blk.y * blk.z, total,
           (blk.x * blk.y * blk.z + 31) / 32, total / 32);
    // 打印每个 block 的首尾线程，展示 gid = bid*tpb + tid
    for (int b = 0; b < grd.x * grd.y * grd.z; ++b) {
        Rec f = h[b * (blk.x * blk.y * blk.z)];
        Rec l = h[b * (blk.x * blk.y * blk.z) + blk.x * blk.y * blk.z - 1];
        printf("BLK|bid=(%d,%d,%d)|first_gid=%d|last_gid=%d|first_tid=(%d,%d,%d)|last_tid=(%d,%d,%d)\n",
               f.bx, f.by, f.bz, f.gid, l.gid, f.tx, f.ty, f.tz, l.tx, l.ty, l.tz);
    }
    for (int i = 0; i < total && i < 40; ++i) {
        Rec r = h[i];
        printf("THR|gid=%d|block=(%d,%d,%d)|thread=(%d,%d,%d)|warp=%d|lane=%2d\n",
               r.gid, r.bx, r.by, r.bz, r.tx, r.ty, r.tz, r.wid, r.lane);
    }
    CK(cudaFree(d));
}

// ---------- E2 warp/lane 归属：lane 由 threadIdx.x & 31 决定 ----------
__global__ void k_lane(int* wid_out, int* lane_out) {
    int tid = blockIdx.x * blockDim.x + threadIdx.x;
    wid_out[tid] = threadIdx.x / 32;      // block 内 warp 号
    lane_out[tid] = threadIdx.x & 31;     // warp 内 lane 号
}

static void mode_lane() {
    int bd = 256, gd = 3;
    int total = bd * gd;
    int *dw, *dl;
    CK(cudaMalloc(&dw, sizeof(int) * total)); CK(cudaMalloc(&dl, sizeof(int) * total));
    k_lane<<<gd, bd>>>(dw, dl); CK(cudaDeviceSynchronize());
    std::vector<int> w(total), l(total);
    CK(cudaMemcpy(w.data(), dw, sizeof(int) * total, cudaMemcpyDeviceToHost));
    CK(cudaMemcpy(l.data(), dl, sizeof(int) * total, cudaMemcpyDeviceToHost));
    printf("LANE|blockDim=%d|gridDim=%d|warpsPerBlock=%d|totalWarps=%d\n", bd, gd, bd / 32, total / 32);
    for (int b = 0; b < gd; ++b) {
        printf("BLK_WARP|bid=%d|warps=%d|tid_range=[%d,%d]|gtid_range=[%d,%d]\n",
               b, bd / 32, 0, bd - 1, b * bd, b * bd + bd - 1);
    }
    // 每个 warp 的 lane 序列（证明每 32 个 threadIdx 一个 warp，且跨 block 从 0 重数）
    for (int i = 0; i < total; i += 32) {
        printf("WARP|gtid=%d..%d|bid=%d|internal_warp=%d|lane0..31=", i, i + 31, i / bd, w[i]);
        for (int j = 0; j < 32; ++j) printf("%d%s", l[i + j], j == 31 ? "\n" : ",");
    }
    CK(cudaFree(dw)); CK(cudaFree(dl));
}

// ---------- E3 block -> SM 映射 ----------
__global__ void k_smid(int* cnt, int nsm, int* rec, int ngrid) {
    unsigned s = get_smid();
    if (threadIdx.x == 0) {
        atomicAdd(&cnt[s], 1);
        if (blockIdx.x < ngrid) rec[blockIdx.x] = (int)s;
    }
}

static void mode_smid() {
    cudaDeviceProp p; CK(cudaGetDeviceProperties(&p, 0));
    int nsm = p.multiProcessorCount;
    int grids[] = {nsm, nsm * 2, nsm * 10, nsm * 100};
    int bd = 256;
    for (int gi = 0; gi < 4; ++gi) {
        int gd = grids[gi];
        int* dc; int* dr;
        CK(cudaMalloc(&dc, sizeof(int) * nsm)); CK(cudaMemset(dc, 0, sizeof(int) * nsm));
        CK(cudaMalloc(&dr, sizeof(int) * gd));
        k_smid<<<gd, bd>>>(dc, nsm, dr, gd); CK(cudaDeviceSynchronize());
        std::vector<int> c(nsm, 0), r(gd, 0);
        CK(cudaMemcpy(c.data(), dc, sizeof(int) * nsm, cudaMemcpyDeviceToHost));
        CK(cudaMemcpy(r.data(), dr, sizeof(int) * gd, cudaMemcpyDeviceToHost));
        int mx = 0, mn = 1 << 30, used = 0;
        for (int i = 0; i < nsm; ++i) { mx = std::max(mx, c[i]); if (c[i]) { ++used; mn = std::min(mn, c[i]); } }
        printf("SMID|grid=%d|blockDim=%d|nsm=%d|sm_used=%d|blocks_per_sm_min=%d|blocks_per_sm_max=%d|total=%d\n",
               gd, bd, nsm, used, used ? mn : 0, mx, gd);
        printf("HIST|grid=%d|", gd);
        for (int i = 0; i < nsm; ++i) if (i < 16 || c[i] != c[0]) printf("sm%d=%d,", i, c[i]);
        printf("\n");
        if (gd <= nsm * 2) {
            printf("BLK2SM|grid=%d|", gd);
            for (int i = 0; i < gd; ++i) printf("%d->%d,", i, r[i]);
            printf("\n");
        }
        CK(cudaFree(dc)); CK(cudaFree(dr));
    }
}

// ---------- E4 warp 分歧（trip count 不同） ----------
__global__ void k_div(float* out, int iters, int mode, long long* cyc, unsigned* mask) {
    int lane = threadIdx.x & 31;
    int n;
    if (mode == 0)      n = iters;                         // 全 warp 一致
    else if (mode == 1) n = (lane < 16) ? iters : iters * 2; // 半 warp 发散
    else if (mode == 2) n = iters + lane;                  // 按 lane 微发散
    else                n = iters * (1 + (lane & 3));      // 4 组，最大 4 倍
    long long t0 = clock64();
    float acc = 1.0f;
    for (int i = 0; i < n; ++i) acc = fmaf(acc, 1.0000001f, 1e-7f);
    long long t1 = clock64();
    out[blockIdx.x * blockDim.x + threadIdx.x] = acc;
    if (threadIdx.x == 0) { cyc[blockIdx.x] = t1 - t0; mask[blockIdx.x] = __activemask(); }
}

static void mode_div() {
    warm();
    int bd = 32, gd = 128, iters = 2000;
    float* o; long long* c; unsigned* m;
    CK(cudaMalloc(&o, sizeof(float) * bd * gd));
    CK(cudaMalloc(&c, sizeof(long long) * gd));
    CK(cudaMalloc(&m, sizeof(unsigned) * gd));
    const char* names[] = {"uniform", "half_warp_2x", "per_lane_small", "four_groups_4x"};
    double base = 0;
    for (int mode = 0; mode < 4; ++mode) {
        // 预热
        k_div<<<gd, bd>>>(o, iters, mode, c, m); CK(cudaDeviceSynchronize());
        float ms = 0;
        const int rep = 30;
        cudaEvent_t a, b; cudaEventCreate(&a); cudaEventCreate(&b);
        cudaEventRecord(a);
        for (int r = 0; r < rep; ++r) k_div<<<gd, bd>>>(o, iters, mode, c, m);
        cudaEventRecord(b); CK(cudaEventSynchronize(b)); cudaEventElapsedTime(&ms, a, b);
        std::vector<long long> hc(gd);
        CK(cudaMemcpy(hc.data(), c, sizeof(long long) * gd, cudaMemcpyDeviceToHost));
        std::vector<long long> s = hc; std::sort(s.begin(), s.end());
        double avg_cyc = 0; for (long long v : hc) avg_cyc += (double)v; avg_cyc /= gd;
        if (mode == 0) base = avg_cyc;
        printf("DIV|mode=%s|iters=%d|warp_cycles_median=%lld|warp_cycles_min=%lld|warp_cycles_max=%lld|"
               "warp_cycles_avg=%.0f|ms_per_rep=%.4f|ratio_vs_base=%.2f\n",
               names[mode], iters, s[gd / 2], s[0], s[gd - 1], avg_cyc, ms / rep, avg_cyc / base);
    }
    CK(cudaFree(o)); CK(cudaFree(c)); CK(cudaFree(m));
}

// ---------- E5 分歧点上的 active mask ----------
__global__ void k_ballot(unsigned* out) {
    int lane = threadIdx.x & 31;
    unsigned mfull = __activemask();
    unsigned mLow = 0, mHigh = 0;
    if (lane < 16) mLow = __activemask();
    else           mHigh = __activemask();
    unsigned b = __ballot_sync(0xffffffff, lane < 16);
    if (threadIdx.x == 0)  { out[blockIdx.x * 4 + 0] = mfull; out[blockIdx.x * 4 + 1] = mLow; }
    if (threadIdx.x == 16) { out[blockIdx.x * 4 + 2] = mHigh; }
    if (threadIdx.x == 0)  { out[blockIdx.x * 4 + 3] = b; }
}

static void mode_ballot() {
    int bd = 32, gd = 2;
    unsigned* o; CK(cudaMalloc(&o, sizeof(unsigned) * gd * 4));
    k_ballot<<<gd, bd>>>(o); CK(cudaDeviceSynchronize());
    std::vector<unsigned> h(gd * 4);
    CK(cudaMemcpy(h.data(), o, sizeof(unsigned) * gd * 4, cudaMemcpyDeviceToHost));
    printf("BALLOT|blockDim=%d|gridDim=%d\n", bd, gd);
    for (int b = 0; b < gd; ++b)
        printf("BLK|bid=%d|full_mask=0x%08x|branch_low_mask=0x%08x|branch_high_mask=0x%08x|ballot_lane_lt16=0x%08x\n",
               b, h[b * 4 + 0], h[b * 4 + 1], h[b * 4 + 2], h[b * 4 + 3]);
}

// ---------- E6 occupancy：每 SM 能同时驻留多少 block/warp ----------
__global__ void k_noop() { }

static void mode_occ() {
    warm();
    cudaDeviceProp p; CK(cudaGetDeviceProperties(&p, 0));
    int bds[] = {32, 64, 96, 128, 192, 256, 384, 512, 768, 1024};
    printf("OCC|nsm=%d|maxThreadsPerSM=%d|maxWarpsPerSM=%d|maxBlocksPerSM=%d|regsPerSM=%d\n",
           p.multiProcessorCount, p.maxThreadsPerMultiProcessor,
           p.maxThreadsPerMultiProcessor / 32, p.maxBlocksPerMultiProcessor, p.regsPerMultiprocessor);
    int maxWarp = p.maxThreadsPerMultiProcessor / 32;
    for (int bd : bds) {
        int nb = 0;
        CK(cudaOccupancyMaxActiveBlocksPerMultiprocessor(&nb, k_noop, bd, 0));
        int wb = (bd + 31) / 32;
        int capWarps = maxWarp / wb;                        // 受 warp 名额限制
        int capThreads = p.maxThreadsPerMultiProcessor / bd; // 受线程名额限制
        int capBlocks = p.maxBlocksPerMultiProcessor;       // 受 block 名额限制
        int bound = capBlocks;
        const char* why = "blockSlot";
        if (capWarps < bound) { bound = capWarps; why = "warpSlot"; }
        if (capThreads < bound) { bound = capThreads; why = "threadSlot"; }
        printf("OCC_ROW|blockDim=%d|warpsPerBlock=%d|capByBlocks=%d|capByWarps=%d|capByThreads=%d|"
               "blocksPerSM=%d|residentWarpsPerSM=%d|residentThreadsPerSM=%d|residentThreadsGPU=%d|"
               "limited_by=%s|matches=%d\n",
               bd, wb, capBlocks, capWarps, capThreads, nb, nb * wb, nb * bd,
               nb * bd * p.multiProcessorCount, why, (nb == bound));
    }
}

// ---------- E7 尾效应：grid 不是 SM 整数倍 ----------
__global__ void k_tail(float* o, int iters) {
    float a = threadIdx.x * 1e-6f;
    for (int i = 0; i < iters; ++i) a = fmaf(a, 1.0000001f, 1e-7f);
    if (a == 12345.678f) o[0] = a;
}

static void mode_tail() {
    warm();
    cudaDeviceProp p; CK(cudaGetDeviceProperties(&p, 0));
    int nsm = p.multiProcessorCount, bd = 256, iters = 20000;
    int bps = 0;
    CK(cudaOccupancyMaxActiveBlocksPerMultiprocessor(&bps, k_tail, bd, 0));
    int wave = nsm * bps;   // 一个 wave 能同时装下的 block 数
    printf("TAIL_META|nsm=%d|blocksPerSM=%d|blockDim=%d|waveBlocks=%d|warpsPerWave=%d\n",
           nsm, bps, bd, wave, wave * bd / 32);
    float* o; CK(cudaMalloc(&o, sizeof(float)));
    int grids[] = {nsm, wave / 2, wave - 1, wave, wave + 1, wave * 2, wave * 2 + 1};
    double t1 = 0;
    for (int gd : grids) {
        k_tail<<<gd, bd>>>(o, iters); CK(cudaDeviceSynchronize());
        float ms = 0; const int rep = 20;
        cudaEvent_t a, b; cudaEventCreate(&a); cudaEventCreate(&b);
        cudaEventRecord(a);
        for (int r = 0; r < rep; ++r) k_tail<<<gd, bd>>>(o, iters);
        cudaEventRecord(b); CK(cudaEventSynchronize(b)); cudaEventElapsedTime(&ms, a, b);
        int waves = (gd + wave - 1) / wave;
        if (gd == wave) t1 = ms / rep;
        printf("TAIL|grid=%d|blockDim=%d|waves=%d|ms_per_rep=%.4f|ms_per_wave=%.4f|"
               "vs_full_wave=%.2f\n", gd, bd, waves, ms / rep, ms / rep / waves,
               t1 > 0 ? (ms / rep) / t1 : 0.0);
    }
    CK(cudaFree(o));
}

// ---------- E8 block 归约：warp shuffle vs 共享内存 ----------
// 值已在寄存器里，不读显存，专门测 warp 内 / block 内通信代价
__global__ void k_redl_shfl(float* out) {
    float s = (float)threadIdx.x;
    for (int off = 16; off > 0; off >>= 1) s += __shfl_down_sync(0xffffffff, s, off);
    __shared__ float w[32];
    if ((threadIdx.x & 31) == 0) w[threadIdx.x >> 5] = s;
    __syncthreads();
    if (threadIdx.x == 0) {
        float t = 0.f;
        for (int i = 0; i < blockDim.x / 32; ++i) t += w[i];
        out[blockIdx.x] = t;
    }
}

__global__ void k_redl_smem(float* out) {
    extern __shared__ float sm[];
    sm[threadIdx.x] = (float)threadIdx.x;
    __syncthreads();
    for (int st = blockDim.x / 2; st > 0; st >>= 1) {
        if (threadIdx.x < st) sm[threadIdx.x] += sm[threadIdx.x + st];
        __syncthreads();
    }
    if (threadIdx.x == 0) out[blockIdx.x] = sm[0];
}

static void mode_red() {
    warm();
    int bd = 256, gd = 0;
    cudaDeviceProp p; CK(cudaGetDeviceProperties(&p, 0));
    gd = p.multiProcessorCount * 4;
    float* o; CK(cudaMalloc(&o, sizeof(float) * gd));
    for (int k = 0; k < 2; ++k) {
        cudaEvent_t a, b; cudaEventCreate(&a); cudaEventCreate(&b);
        if (k == 0) k_redl_shfl<<<gd, bd>>>(o); else k_redl_smem<<<gd, bd, sizeof(float) * bd>>>(o);
        CK(cudaDeviceSynchronize());
        const int rep = 2000; float ms = 0;
        cudaEventRecord(a);
        for (int r = 0; r < rep; ++r) {
            if (k == 0) k_redl_shfl<<<gd, bd>>>(o);
            else        k_redl_smem<<<gd, bd, sizeof(float) * bd>>>(o);
        }
        cudaEventRecord(b); CK(cudaEventSynchronize(b)); cudaEventElapsedTime(&ms, a, b);
        float got = 0; CK(cudaMemcpy(&got, o, sizeof(float), cudaMemcpyDeviceToHost));
        printf("RED|impl=%s|blockDim=%d|gridDim=%d|ns_per_block_reduce=%.1f|sum_expect=%.0f|sum_got=%.0f\n",
               k == 0 ? "warp_shfl" : "shared_mem", bd, gd,
               ms * 1e6 / rep / gd, bd * (bd - 1) / 2.0, got);
    }
    CK(cudaFree(o));
}

// ---------- E9 启动开销：空 kernel × 不同 grid ----------
__global__ void k_empty() { }

static void mode_launch() {
    warm();
    int grids[] = {1, 100, 10000, 1000000};
    for (int gd : grids) {
        k_empty<<<gd, 32>>>(); CK(cudaDeviceSynchronize());
        cudaEvent_t a, b; cudaEventCreate(&a); cudaEventCreate(&b);
        const int rep = 200; float ms = 0;
        cudaEventRecord(a);
        for (int r = 0; r < rep; ++r) k_empty<<<gd, 32>>>();
        cudaEventRecord(b); CK(cudaEventSynchronize(b)); cudaEventElapsedTime(&ms, a, b);
        printf("LAUNCH|grid=%d|blockDim=32|us_per_launch=%.2f|ns_per_block=%.2f\n",
               gd, ms * 1000.0f / rep, ms * 1e6 / rep / gd);
    }
}

// ---------- E10 每 SM 驻留 warp 数 -> 吞吐 ----------
__global__ void k_sched(float* o, int iters) {
    long long t0 = clock64();
    float a = threadIdx.x * 1e-6f;
    for (int i = 0; i < iters; ++i) a = fmaf(a, 1.0000001f, 1e-7f);
    long long t1 = clock64();
    if (a == 12345.678f) o[0] = a;
    if (threadIdx.x == 0 && blockIdx.x == 0) o[1] = (float)(t1 - t0);
}

static void mode_sched() {
    warm();
    cudaDeviceProp p; CK(cudaGetDeviceProperties(&p, 0));
    int nsm = p.multiProcessorCount, bd = 32, iters = 20000;
    float* o; CK(cudaMalloc(&o, sizeof(float) * 2));
    double t1 = 0, best = 0;
    for (int k = 1; k <= 16; ++k) {
        int gd = nsm * k;
        k_sched<<<gd, bd>>>(o, iters); CK(cudaDeviceSynchronize());
        float ms = 0; const int rep = 20;
        cudaEvent_t a, b; cudaEventCreate(&a); cudaEventCreate(&b);
        cudaEventRecord(a);
        for (int r = 0; r < rep; ++r) k_sched<<<gd, bd>>>(o, iters);
        cudaEventRecord(b); CK(cudaEventSynchronize(b)); cudaEventElapsedTime(&ms, a, b);
        double flops = (double)gd * bd * iters * 2.0;
        double gf = flops / (ms / rep * 1e-3) / 1e9;
        float cyc = 0; CK(cudaMemcpy(&cyc, o + 1, sizeof(float), cudaMemcpyDeviceToHost));
        double eff_clk = cyc / (ms / rep * 1e-3) / 1e6;      // 反推有效 SM 频率 MHz
        double fma_per_warp_cyc = (double)iters / cyc;        // 每个 warp 每周期几个 FMA
        if (k == 1) t1 = ms / rep;
        if (gf > best) best = gf;
        printf("SCHED|warps_per_sm=%d|gridDim=%d|blockDim=%d|ms_per_rep=%.4f|GFLOPS=%.1f|"
               "speedup_vs_1warp=%.2f|pct_of_best=%.1f|warp_cycles=%d|eff_clk_mhz=%.0f|"
               "fma_per_warp_cycle=%.3f\n", k, gd, bd, ms / rep, gf,
               (t1 / (ms / rep)), 100.0 * gf / best, (int)cyc, eff_clk, fma_per_warp_cyc);
    }
    CK(cudaFree(o));
}

int main(int argc, char** argv) {
    const char* m = argc > 1 ? argv[1] : "dev";
    printf("=== gpu02 mode=%s ===\n", m);
    if (!strcmp(m, "dev"))    mode_dev();
    else if (!strcmp(m, "map"))    mode_map();
    else if (!strcmp(m, "lane"))   mode_lane();
    else if (!strcmp(m, "smid"))   mode_smid();
    else if (!strcmp(m, "div"))    mode_div();
    else if (!strcmp(m, "ballot")) mode_ballot();
    else if (!strcmp(m, "occ"))    mode_occ();
    else if (!strcmp(m, "tail"))   mode_tail();
    else if (!strcmp(m, "red"))    mode_red();
    else if (!strcmp(m, "launch")) mode_launch();
    else if (!strcmp(m, "sched"))  mode_sched();
    else { printf("unknown mode %s\n", m); return 2; }
    printf("=== done ===\n");
    return 0;
}
