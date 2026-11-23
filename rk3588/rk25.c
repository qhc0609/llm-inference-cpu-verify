/* rk25.c —— RK3588 上的第25篇实验：AVX2 换成 NEON
 *   chase  <kb> <accesses>
 *   gemv   <mb> scalar|neon|neonx4|mt:<n>
 *   gemm   <n>  naive|block:<bs>|mt:<n>
 *   stream <mb> read|write|copy
 * 环境变量 CPUS=4-7 可把线程绑到大核簇。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <unistd.h>
#include <sched.h>
#include <arm_neon.h>

static volatile float g_touch;

static double now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec*1e3+t.tv_nsec/1e6; }

/* CPUS=4-7 时把第 tid 个线程钉到第 tid 颗核，否则调度器会把四个线程塞进同一颗 */
static void pin_env(int tid){
    const char *e=getenv("CPUS"); if(!e) return;
    int a=atoi(e),b=a; const char *p=e;
    while(*p && *p!=','){ if(*p=='-'){ p++; b=atoi(p);} else p++; }
    int n=b-a+1; if(n<1) return;
    int c=a+(tid%n);
    cpu_set_t set; CPU_ZERO(&set); CPU_SET(c,&set);
    sched_setaffinity(0,sizeof set,&set);
}

/* ---------------- 指针追逐 ---------------- */
static int run_chase(size_t kb,long total){
    size_t n=kb*1024/sizeof(size_t); if(n<2) n=2;
    size_t *next=aligned_alloc(64,((n*sizeof(size_t))+63)/64*64);
    size_t *perm=malloc(n*sizeof(size_t));
    if(!next||!perm){ fprintf(stderr,"alloc fail\n"); return 1; }
    for(size_t i=0;i<n;i++) perm[i]=i;
    srand(20261122);
    for(size_t i=n-1;i>0;i--){ size_t j=(size_t)(rand()%(int)(i+1)); size_t t=perm[i];perm[i]=perm[j];perm[j]=t; }
    for(size_t i=0;i<n;i++) next[perm[i]]=perm[(i+1)%n];
    long reps=total/(long)n; if(reps<1) reps=1;
    size_t idx=perm[0]; long cnt=reps*(long)n;
    double t0=now_ms();
    for(long r=0;r<reps;r++) for(size_t i=0;i<n;i++) idx=next[idx];
    double dt=now_ms()-t0;
    printf("CHASE kb=%zu n=%zu reps=%ld ns_per_access=%.3f checksum=%zu\n",kb,n,reps,dt*1e6/(double)cnt,idx);
    free(next); free(perm); return 0;
}

/* ---------------- 顺序带宽 ---------------- */
static int run_stream(int mb,const char *mode){
    size_t n=(size_t)mb*1024*1024/sizeof(float);
    float *a=aligned_alloc(64,n*sizeof(float)),*b=aligned_alloc(64,n*sizeof(float));
    if(!a||!b){ fprintf(stderr,"alloc fail\n"); return 1; }
    for(size_t i=0;i<n;i++){ a[i]=(float)(i%97)*0.01f; b[i]=0.f; }
    int reps=10; double bytes=0,sink=0,t0=now_ms();
    if(!strcmp(mode,"read")){
        const uint32_t *p=(const uint32_t*)a;
        for(int r=0;r<reps;r++){ uint32_t s=0; for(size_t i=0;i<n;i++) s+=p[i]; sink+=(double)s; }
        bytes=(double)n*4*reps;
    } else if(!strcmp(mode,"write")){
        for(int r=0;r<reps;r++) for(size_t i=0;i<n;i++) b[i]=(float)r;
        bytes=(double)n*4*reps;
    } else if(!strcmp(mode,"copy")){
        for(int r=0;r<reps;r++) for(size_t i=0;i<n;i++) b[i]=a[i];
        bytes=(double)n*4*2*reps;
    } else { fprintf(stderr,"bad mode\n"); return 2; }
    double dt=now_ms()-t0;
    printf("STREAM mb=%d mode=%s ms=%.1f gbs=%.2f checksum=%.1f\n",mb,mode,dt,bytes/(dt/1e3)/1e9,sink+b[0]);
    free(a); free(b); return 0;
}

/* ---------------- GEMV ---------------- */
typedef struct { const float *w; float *out; const float *x; int K; size_t from,to; int reps; int tid; } gtask;

/* 关掉自动向量化，保证 scalar 真的是标量 */
__attribute__((optimize("no-tree-vectorize")))
static float gemv_scalar(size_t n,const float *w,const float *x,int K){
    g_touch=(float)n;
    const int mask=K-1; float acc=0.f;
    for(size_t i=0;i<n;i++) acc+=w[i]*x[i&mask];
    return acc;
}

static float gemv_neon(size_t n,const float *w,const float *x,int K){
    g_touch=(float)n;
    const int mask=K-1;
    float32x4_t acc=vdupq_n_f32(0.f);
    size_t i=0;
    for(;i+4<=n;i+=4) acc=vfmaq_f32(acc,vld1q_f32(w+i),vld1q_f32(x+(i&mask)));
    float s=vaddvq_f32(acc);
    for(;i<n;i++) s+=w[i]*x[i&mask];
    return s;
}

static float gemv_neonx4(size_t n,const float *w,const float *x,int K){
    g_touch=(float)n;
    const int mask=K-1;
    float32x4_t a0=vdupq_n_f32(0.f),a1=a0,a2=a0,a3=a0;
    size_t i=0;
    for(;i+16<=n;i+=16){
        a0=vfmaq_f32(a0,vld1q_f32(w+i),     vld1q_f32(x+(i&mask)));
        a1=vfmaq_f32(a1,vld1q_f32(w+i+4),   vld1q_f32(x+((i+4)&mask)));
        a2=vfmaq_f32(a2,vld1q_f32(w+i+8),   vld1q_f32(x+((i+8)&mask)));
        a3=vfmaq_f32(a3,vld1q_f32(w+i+12),  vld1q_f32(x+((i+12)&mask)));
    }
    float s=vaddvq_f32(vaddq_f32(vaddq_f32(a0,a1),vaddq_f32(a2,a3)));
    for(;i<n;i++) s+=w[i]*x[i&mask];
    return s;
}

static void *gemv_thread(void *p){
    gtask *t=(gtask*)p; pin_env(t->tid);
    size_t n=t->to-t->from; float acc=0.f;
    for(int r=0;r<t->reps;r++) acc+=gemv_neonx4(n,t->w+t->from,t->x,t->K);
    t->out[0]=acc; return NULL;
}

static int run_gemv(int mb,const char *variant){
    int K=8192;
    size_t n=(size_t)mb*1024*1024/sizeof(float); n-=n%32;
    float *w=aligned_alloc(64,n*sizeof(float)+64),*x=aligned_alloc(64,(K+16)*sizeof(float));
    if(!w||!x){ fprintf(stderr,"alloc fail\n"); return 1; }
    srand(4242);
    for(size_t i=0;i<n;i++) w[i]=(float)((rand()%200)-100)*0.001f;
    for(int i=0;i<K+16;i++) x[i]=(float)((i%37)*0.01);
    int reps=20; double bytes=(double)n*4*reps; float r=0;
    if(!strncmp(variant,"mt:",3)){
        int nt=atoi(variant+3); if(nt<1) nt=1;
        pthread_t th[64]; gtask t[64]; float out[64];
        for(int i=0;i<nt;i++) out[i]=0.f;
        size_t chunk=(n/nt)&~(size_t)31;
        for(int i=0;i<nt;i++){
            t[i].w=w; t[i].x=x; t[i].K=K; t[i].from=(size_t)i*chunk;
            t[i].to=(i==nt-1)?n:(size_t)(i+1)*chunk; t[i].out=&out[i]; t[i].reps=reps; t[i].tid=i;
        }
        double t0=now_ms();
        for(int i=0;i<nt;i++) pthread_create(&th[i],NULL,gemv_thread,&t[i]);
        for(int i=0;i<nt;i++){ pthread_join(th[i],NULL); r+=out[i]; }
        double dt=now_ms()-t0;
        printf("GEMV variant=mt:%d mb=%d n=%zu reps=%d ms=%.2f gbs=%.2f checksum=%.4f\n",
               nt,mb,n,reps,dt,bytes/(dt/1e3)/1e9,r);
    } else {
        float (*fn)(size_t,const float*,const float*,int)=gemv_scalar;
        if(!strcmp(variant,"neon")) fn=gemv_neon;
        else if(!strcmp(variant,"neonx4")) fn=gemv_neonx4;
        fn(n,w,x,K);
        double t0=now_ms();
        for(int i=0;i<reps;i++) r+=fn(n,w,x,K);
        double dt=now_ms()-t0;
        printf("GEMV variant=%s mb=%d n=%zu reps=%d ms=%.2f gbs=%.2f checksum=%.4f\n",
               variant,mb,n,reps,dt,bytes/(dt/1e3)/1e9,r);
    }
    free(w); free(x); return 0;
}

/* ---------------- GEMM ---------------- */
static double *gA,*gB,*gC;
static void gemm_naive(int n){
    for(int i=0;i<n;i++) for(int j=0;j<n;j++){
        double s=0; for(int k=0;k<n;k++) s+=gA[i*n+k]*gB[k*n+j]; gC[i*n+j]=s; }
}
static void gemm_block(int n,int BS){
    memset(gC,0,(size_t)n*n*sizeof(double));
    for(int ii=0;ii<n;ii+=BS) for(int kk=0;kk<n;kk+=BS) for(int jj=0;jj<n;jj+=BS)
        for(int i=ii;i<ii+BS&&i<n;i++) for(int k=kk;k<kk+BS&&k<n;k++){
            double a=gA[i*n+k];
            for(int j=jj;j<jj+BS&&j<n;j++) gC[i*n+j]+=a*gB[k*n+j];
        }
}
typedef struct { int n,from,to,BS,reps,tid; } btask;
static void *gemm_thread(void *p){
    btask *t=(btask*)p; pin_env(t->tid);
    int n=t->n,BS=t->BS;
    for(int r=0;r<t->reps;r++)
    for(int ii=t->from;ii<t->to;ii+=BS) for(int kk=0;kk<n;kk+=BS) for(int jj=0;jj<n;jj+=BS)
        for(int i=ii;i<ii+BS&&i<t->to;i++) for(int k=kk;k<kk+BS&&k<n;k++){
            double a=gA[i*n+k];
            for(int j=jj;j<jj+BS&&j<n;j++) gC[i*n+j]+=a*gB[k*n+j];
        }
    return NULL;
}
static int run_gemm(int n,const char *variant){
    size_t sz=(size_t)n*n;
    gA=malloc(sz*sizeof(double)); gB=malloc(sz*sizeof(double)); gC=malloc(sz*sizeof(double));
    if(!gA||!gB||!gC){ fprintf(stderr,"alloc fail\n"); return 1; }
    for(size_t i=0;i<sz;i++){ gA[i]=(double)(i%19)*0.01; gB[i]=(double)(i%23)*0.01; gC[i]=0; }
    double t0=now_ms();
    if(!strncmp(variant,"block:",6)) gemm_block(n,atoi(variant+6));
    else if(!strncmp(variant,"mt:",3)){
        int nt=atoi(variant+3); if(nt<1) nt=1;
        memset(gC,0,sz*sizeof(double));
        pthread_t th[64]; btask t[64]; int rows=n/nt;
        for(int i=0;i<nt;i++){
            t[i].n=n; t[i].BS=64; t[i].reps=1; t[i].from=i*rows;
            t[i].to=(i==nt-1)?n:(i+1)*rows; t[i].tid=i;
            pthread_create(&th[i],NULL,gemm_thread,&t[i]);
        }
        for(int i=0;i<nt;i++) pthread_join(th[i],NULL);
    } else gemm_naive(n);
    double dt=now_ms()-t0, sum=0;
    for(size_t i=0;i<sz;i++) sum+=gC[i];
    printf("GEMM variant=%s n=%d ms=%.1f gflops=%.2f checksum=%.3f\n",
           variant,n,dt,2.0*n*n*n/(dt/1e3)/1e9,sum);
    free(gA); free(gB); free(gC); return 0;
}

int main(int argc,char **argv){
    if(argc<2){ fprintf(stderr,"usage: chase|gemv|gemm|stream ...\n"); return 2; }
    if(!strcmp(argv[1],"chase")&&argc>=4) return run_chase((size_t)atol(argv[2]),atol(argv[3]));
    if(!strcmp(argv[1],"stream")&&argc>=4) return run_stream(atoi(argv[2]),argv[3]);
    if(!strcmp(argv[1],"gemv")&&argc>=4) return run_gemv(atoi(argv[2]),argv[3]);
    if(!strcmp(argv[1],"gemm")&&argc>=3) return run_gemm(atoi(argv[2]),argc>=4?argv[3]:"naive");
    fprintf(stderr,"bad args\n"); return 2;
}
