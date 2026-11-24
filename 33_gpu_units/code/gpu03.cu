// gpu03.cu —— 第 03 篇配套微基准：SM / CUDA Core / Tensor Core 如何协同计算
// 编译： nvcc -O3 -arch=native -o gpu03 gpu03.cu   （Kaggle T4: cc7.5, arch=sm_75）
// 用法： ./gpu03 <mode>
//   mode = dev|ccount|gemm_f32|gemm_tf32|gemm_f16|gemm_int8|mma|peak|reg|waves
// 说明：所有不支持的精度按 cc 自动跳过并打印 SKIP 原因，绝不估算冒充实测。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cstdint>
#include <cuda_runtime.h>
#include <cuda_fp16.h>
#if __CUDA_ARCH__ >= 800
#include <cuda_bf16.h>
#endif
#include <mma.h>
#include <algorithm>

#define CK(x) do{ cudaError_t e_=(x); if(e_!=cudaSuccess){ \
    printf("CUDA_ERR %s @%d: %s\n", #x, __LINE__, cudaGetErrorString(e_)); exit(1);} }while(0)

using namespace nvcuda;

// ---------- 预热：把 SM 频率拉到稳态 ----------
__global__ void k_spin(float* o, int iters) {
    float a = threadIdx.x * 1e-6f;
    for (int i = 0; i < iters; ++i) a = fmaf(a, 1.0000001f, 1e-7f);
    if (a == 12345.678f) o[0] = a;
}
static void warm(int ms_target = 500) {
    cudaDeviceProp p; CK(cudaGetDeviceProperties(&p, 0));
    int gd = p.multiProcessorCount * 8, bd = 128;
    float* o; CK(cudaMalloc(&o, sizeof(float)));
    cudaEvent_t a, b; cudaEventCreate(&a); cudaEventCreate(&b);
    cudaEventRecord(a); float el = 0; int guard = 0;
    while (el < ms_target && guard++ < 5000) {
        k_spin<<<gd, bd>>>(o, 20000);
        cudaEventRecord(b); CK(cudaEventSynchronize(b));
        cudaEventElapsedTime(&el, a, b);
    }
    int clk=0; cudaDeviceGetAttribute(&clk, cudaDevAttrClockRate, 0);
    printf("WARM|ms=%.0f|rounds=%d|clk_mhz=%.0f\n", el, guard, clk/1e3);
    CK(cudaFree(o));
}

// ---------- E0 设备属性 ----------
static void mode_dev() {
    int n=0; CK(cudaGetDeviceCount(&n));
    for (int i=0;i<n;++i){
        cudaDeviceProp p; CK(cudaGetDeviceProperties(&p,i));
        int clk=0,mclk=0; CK(cudaDeviceGetAttribute(&clk,cudaDevAttrClockRate,i));
        CK(cudaDeviceGetAttribute(&mclk,cudaDevAttrMemoryClockRate,i));
        printf("DEV|name=%s|cc=%d.%d|sm=%d|thr_per_sm=%d|warps_per_sm=%d|"
               "blocks_per_sm=%d|regs_per_sm=%d|smem_per_sm=%zu|"
               "clk_mhz=%.0f|mem_bus=%d|mem_clk_mhz=%.0f|l2=%d\n",
               p.name,p.major,p.minor,p.multiProcessorCount,
               p.maxThreadsPerMultiProcessor,p.maxThreadsPerMultiProcessor/32,
               p.maxBlocksPerMultiProcessor,p.regsPerMultiprocessor,
               p.sharedMemPerMultiprocessor,clk/1e3,p.memoryBusWidth,mclk/1e3,p.l2CacheSize);
    }
}

// ---------- E1 各类 Core 数量（纸面，由 SM 数推导，标注为"规格值"） ----------
static void mode_ccount() {
    cudaDeviceProp p; CK(cudaGetDeviceProperties(&p,0));
    int cc=p.major*10+p.minor;
    int fp32_per_sm = (p.major==7)?128:((p.major==8)?128:64);  // Turing/AMPERE SM 的 FP32 CUDA Core
    int tensor_per_sm = (p.major==7)?8:((p.major==8)?4:4);     // 每 SM Tensor Core 数（Turing 8 / Ampere 4 更强）
    printf("CCOUNT|cc=%d|sm=%d|fp32_core_per_sm=%d|fp32_core_total=%d|"
           "tensor_core_per_sm=%d|tensor_core_total=%d|note=spec_derived\n",
           cc,p.multiProcessorCount,fp32_per_sm,fp32_per_sm*p.multiProcessorCount,
           tensor_per_sm,tensor_per_sm*p.multiProcessorCount);
}

// ---------- E2 CUDA Core FP32 矩阵乘（朴素，纯 FMA，不用 Tensor Core） ----------
__global__ void k_gemm_f32(const float* A,const float* B,float* C,int N) {
    int col=blockIdx.x*blockDim.x+threadIdx.x;
    int row=blockIdx.y*blockDim.y+threadIdx.y;
    if(row<N && col<N){
        float s=0.f;
        for(int k=0;k<N;++k) s=fmaf(A[row*N+k],B[k*N+col],s);
        C[row*N+col]=s;
    }
}

// ---------- E3 WMMA Tensor Core FP16 矩阵乘（m16n16k16） ----------
__global__ void k_gemm_wmma_f16(const __half* A,const __half* B,float* C,int N) {
    int wM=16,wN=16,wK=16;
    int bx=blockIdx.x, by=blockIdx.y;
    int aRow=by*wM, bCol=bx*wN;
    wmma::fragment<wmma::matrix_a,16,16,16,__half,wmma::row_major> a_frag;
    wmma::fragment<wmma::matrix_b,16,16,16,__half,wmma::col_major> b_frag;
    wmma::fragment<wmma::accumulator,16,16,16,float> c_frag;
    wmma::fill_fragment(c_frag,0.0f);
    for(int k=0;k<N;k+=wK){
        wmma::load_matrix_sync(a_frag,A+(aRow*N+k),N);
        wmma::load_matrix_sync(b_frag,B+(k*N+bCol),N);
        wmma::mma_sync(c_frag,a_frag,b_frag,c_frag);
    }
    wmma::store_matrix_sync(C+(aRow*N+bCol),c_frag,N,wmma::mem_row_major);
}

// ---------- E4 WMMA Tensor Core INT8 矩阵乘（m16n16k16, cc>=7.5） ----------
__global__ void k_gemm_wmma_i8(const signed char* A,const signed char* B,int* C,int N) {
    int wM=16,wN=16,wK=16;
    int bx=blockIdx.x, by=blockIdx.y;
    int aRow=by*wM, bCol=bx*wN;
    wmma::fragment<wmma::matrix_a,16,16,16,signed char,wmma::row_major> a_frag;
    wmma::fragment<wmma::matrix_b,16,16,16,signed char,wmma::col_major> b_frag;
    wmma::fragment<wmma::accumulator,16,16,16,int> c_frag;
    wmma::fill_fragment(c_frag,0);
    for(int k=0;k<N;k+=wK){
        wmma::load_matrix_sync(a_frag,A+(aRow*N+k),N);
        wmma::load_matrix_sync(b_frag,B+(k*N+bCol),N);
        wmma::mma_sync(c_frag,a_frag,b_frag,c_frag);
    }
    wmma::store_matrix_sync(C+(aRow*N+bCol),c_frag,N,wmma::mem_row_major);
}

// ---------- 计时辅助 ----------
static double bench(const char* tag,int N,int reps,
        void(*launch)(int), double flops_per_call){
    // warm
    for(int i=0;i<5;++i) launch(N);
    CK(cudaDeviceSynchronize());
    cudaEvent_t s,e; cudaEventCreate(&s); cudaEventCreate(&e);
    cudaEventRecord(s);
    for(int i=0;i<reps;++i) launch(N);
    cudaEventRecord(e); CK(cudaEventSynchronize(e));
    float ms=0; cudaEventElapsedTime(&ms,s,e);
    double gf=flops_per_call*reps/(ms*1e6);
    printf("BENCH|%s|N=%d|reps=%d|ms=%.4f|GFLOPS=%.1f|ms_per_call=%.5f\n",
           tag,N,reps,ms,gf,ms/reps);
    return gf;
}

// 全局指针包装（供 bench 回调）
static float *dA32,*dB32,*dC32;
static __half *dAh,*dBh; static float* dCh;
static signed char *dAi,*dBi; static int* dCi;
static void launch_f32(int N){ dim3 b(16,16),g((N+15)/16,(N+15)/16); k_gemm_f32<<<g,b>>>(dA32,dB32,dC32,N); }
static void launch_f16(int N){ dim3 g(N/16,N/16); k_gemm_wmma_f16<<<g,32>>>(dAh,dBh,dCh,N); }
static void launch_i8 (int N){ dim3 g(N/16,N/16); k_gemm_wmma_i8 <<<g,32>>>(dAi,dBi,dCi,N); }

static void fill_data(int N){
    std::vector<float> a(N*N),b(N*N);
    for(int i=0;i<N*N;++i){ a[i]=(float)(i%17)/17.f; b[i]=(float)((i*7)%19)/19.f; }
    CK(cudaMalloc(&dA32,N*N*sizeof(float))); CK(cudaMalloc(&dB32,N*N*sizeof(float)));
    CK(cudaMalloc(&dC32,N*N*sizeof(float)));
    CK(cudaMemcpy(dA32,a.data(),N*N*sizeof(float),cudaMemcpyHostToDevice));
    CK(cudaMemcpy(dB32,b.data(),N*N*sizeof(float),cudaMemcpyHostToDevice));
    std::vector<__half> ah(N*N),bh(N*N);
    std::vector<signed char> ai(N*N),bi(N*N);
    for(int i=0;i<N*N;++i){ ah[i]=__float2half(a[i]); bh[i]=__float2half(b[i]);
        ai[i]=(signed char)((i%251)-125); bi[i]=(signed char)(((i*7)%251)-125); }
    CK(cudaMalloc(&dAh,N*N*sizeof(__half))); CK(cudaMalloc(&dBh,N*N*sizeof(__half)));
    CK(cudaMalloc(&dCh,N*N*sizeof(float)));
    CK(cudaMemcpy(dAh,ah.data(),N*N*sizeof(__half),cudaMemcpyHostToDevice));
    CK(cudaMemcpy(dBh,bh.data(),N*N*sizeof(__half),cudaMemcpyHostToDevice));
    CK(cudaMalloc(&dAi,N*N)); CK(cudaMalloc(&dBi,N*N)); CK(cudaMalloc(&dCi,N*N*sizeof(int)));
    CK(cudaMemcpy(dAi,ai.data(),N*N,cudaMemcpyHostToDevice));
    CK(cudaMemcpy(dBi,bi.data(),N*N,cudaMemcpyHostToDevice));
}

static void mode_gemm(const char* which){
    cudaDeviceProp p; CK(cudaGetDeviceProperties(&p,0));
    int cc=p.major*10+p.minor;
    int N=1024;
    fill_data(N);
    double flops=2.0*N*N*N;
    if(strcmp(which,"f32")==0){
        bench("GEMM_F32_CUDA_CORE",N,20,launch_f32,flops);
    } else if(strcmp(which,"f16")==0){
        bench("GEMM_F16_TENSOR_CORE",N,50,launch_f16,flops);
    } else if(strcmp(which,"int8")==0){
        if(cc<75){ printf("SKIP|INT8 WMMA needs cc>=7.5, got %d\n",cc); return; }
        bench("GEMM_INT8_TENSOR_CORE",N,50,launch_i8,flops); // 与 FP 同口径：1 MAC = 2 ops = 2*N^3
    } else if(strcmp(which,"tf32")==0){
        printf("SKIP|TF32 needs cc>=8.0 (Ampere+), got %d; Kaggle T4 is cc7.5\n",cc);
    } else if(strcmp(which,"bf16")==0){
        printf("SKIP|BF16 WMMA needs cc>=8.0 (Ampere+), got %d; Kaggle T4 is cc7.5\n",cc);
    }
}

// ---------- E5 寄存器与共享内存上限 ----------
static void mode_reg(){
    cudaDeviceProp p; CK(cudaGetDeviceProperties(&p,0));
    printf("MEM|regs_per_sm=%d|regs_per_block=%d|smem_per_sm=%zu|smem_per_block=%zu|"
           "smem_per_block_max_dyn=%zu|warps_per_sm=%d|thr_per_sm=%d\n",
           p.regsPerMultiprocessor,p.regsPerBlock,p.sharedMemPerMultiprocessor,
           p.sharedMemPerBlock,p.sharedMemPerBlockOptin,
           p.maxThreadsPerMultiProcessor/32,p.maxThreadsPerMultiProcessor);
}

// ---------- E6 峰值算力（规格值 + 实测频率推算，标注清楚） ----------
static void mode_peak(){
    cudaDeviceProp p; CK(cudaGetDeviceProperties(&p,0));
    int clk=0; CK(cudaDeviceGetAttribute(&clk,cudaDevAttrClockRate,0));
    int cc=p.major*10+p.minor;
    int sm=p.multiProcessorCount;
    int fp32_per_sm=(p.major==7)?128:((p.major==8)?128:64);
    double fclk=clk/1e3; // MHz
    double peak_f32=2.0*fp32_per_sm*sm*fclk/1e3; // FLOP/s = flop/cycle * sm * Hz; fclk is MHz -> /1e3 gives GFLOPS
    // Tensor Core 峰值：Turing 每 SM 每周期 1024 FLOP(FP16)
    double tc_f16_per_sm_cycle=(cc==75)?1024:((cc>=80)?2048:0);
    double peak_f16=tc_f16_per_sm_cycle*sm*fclk/1e3;
    printf("PEAK|cc=%d|sm=%d|clk_mhz=%.0f|fp32_core_sm=%d|"
           "peak_fp32_GFLOPS=%.0f|tc_f16_flop_sm_cycle=%.0f|peak_fp16_GFLOPS=%.0f|"
           "note=spec_x_clock_not_measured\n",
           cc,sm,fclk,fp32_per_sm,peak_f32,tc_f16_per_sm_cycle,peak_f16);
}

int main(int argc,char** argv){
    const char* m = argc>1?argv[1]:"dev";
    if(!strcmp(m,"dev"))      mode_dev();
    else if(!strcmp(m,"ccount")) mode_ccount();
    else if(!strcmp(m,"reg"))    mode_reg();
    else if(!strcmp(m,"peak"))   mode_peak();
    else if(!strncmp(m,"gemm_",5)) { warm(); mode_gemm(m+5); }
    else { printf("USAGE: dev|ccount|reg|peak|gemm_f32|gemm_f16|gemm_int8|gemm_tf32|gemm_bf16\n"); }
    CK(cudaDeviceSynchronize());
    return 0;
}
