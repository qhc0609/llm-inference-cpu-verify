/* rk24.c —— RK3588（big.LITTLE 真机）上的第24篇实验
 * 与 pow24.c 的区别：这里频率、温度都读得到，所以每一行都带上它们。
 *   sustain2 <threads> <sec> <wmb> <win> [cpus]
 *   burst    <threads> <on> <off> <cycles> <wmb> [cpus]
 *   probe    <sec> <interval> [cpus]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <sys/resource.h>

static volatile float g_sink;
#define SAMP_CAP 16384

typedef struct {
    float *w; size_t n; float *x; int K;
    volatile int *stop;
    volatile unsigned long long tokens, cnt;
    double *samp;
    int cpus[16], ncpu;
} arg_t;

static double now_s(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec/1e9; }
static double cpu_thread_s(void){ struct rusage r; getrusage(RUSAGE_THREAD,&r);
    return r.ru_utime.tv_sec+r.ru_utime.tv_usec/1e6+r.ru_stime.tv_sec+r.ru_stime.tv_usec/1e6; }
/* 进程级：burst 的活在别的线程里，必须用 RUSAGE_SELF 才统计得到 */
static double cpu_s(void){ struct rusage r; getrusage(RUSAGE_SELF,&r);
    return r.ru_utime.tv_sec+r.ru_utime.tv_usec/1e6+r.ru_stime.tv_sec+r.ru_stime.tv_usec/1e6; }

static int rd_int(const char *p){ FILE *f=fopen(p,"r"); int v=-1;
    if(!f) return -1; if(fscanf(f,"%d",&v)!=1) v=-1; fclose(f); return v; }

static int clus_freq(int from,int to){ int s=0,n=0; char b[128];
    for(int c=from;c<to;c++){ snprintf(b,sizeof b,"/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq",c);
        int v=rd_int(b); if(v>0){ s+=v; n++; } }
    return n?s/n/1000:-1; }
static int big_freq(void){ return clus_freq(4,8); }
static int lit_freq(void){ return clus_freq(0,4); }
/* zone1 = bigcore0, zone3 = littlecore */
static int big_temp(void){ return rd_int("/sys/class/thermal/thermal_zone1/temp"); }
static int lit_temp(void){ return rd_int("/sys/class/thermal/thermal_zone3/temp"); }

static int parse_cpus(const char *s,int *out){
    int n=0; const char *p=s;
    while(*p && n<16){
        int a=atoi(p), b=a;
        while(*p && *p!=','){ if(*p=='-'){ p++; b=atoi(p);} else p++; }
        for(int c=a;c<=b && n<16;c++) out[n++]=c;
        if(*p==',') p++;
    }
    return n;
}
static void pin_self(const arg_t *a){
    if(a->ncpu<=0) return;
    cpu_set_t set; CPU_ZERO(&set);
    for(int i=0;i<a->ncpu;i++) CPU_SET(a->cpus[i],&set);
    sched_setaffinity(0,sizeof set,&set);
}

static int cmpd(const void *a,const void *b){ double x=*(const double*)a,y=*(const double*)b; return (x>y)-(x<y); }
static double pctile(double *v,int n,double p){
    if(n<=0) return 0; qsort(v,n,sizeof(double),cmpd);
    int k=(int)(p*(n-1)+0.5); if(k<0)k=0; if(k>=n)k=n-1; return v[k];
}

static void *worker(void *p){
    arg_t *a=(arg_t*)p; pin_self(a);
    const int mask=a->K-1;
    while(!*a->stop){
        double c0=cpu_thread_s();
        float acc=0.f;
        for(size_t i=0;i<a->n;i++) acc+=a->w[i]*a->x[i&mask];
        g_sink=acc;
        if(a->samp) a->samp[a->cnt%SAMP_CAP]=(cpu_thread_s()-c0)*1e6;
        a->tokens++; a->cnt++;
    }
    return NULL;
}

static void fill(arg_t *a,size_t n,int K,unsigned seed){
    a->n=n; a->K=K;
    a->w=aligned_alloc(64,n*sizeof(float));
    a->x=aligned_alloc(64,(K+16)*sizeof(float));
    if(!a->w||!a->x){ fprintf(stderr,"alloc fail\n"); exit(1); }
    srand(seed);
    for(size_t i=0;i<n;i++) a->w[i]=(float)((rand()%200)-100)*0.001f;
    for(int i=0;i<K+16;i++) a->x[i]=(float)((i%37)*0.01);
    a->tokens=0; a->cnt=0;
}

static double probe_ops(double sec){
    double a=1.0,b=1.000000001,t0=now_s();
    unsigned long long i=0;
    while(now_s()-t0<sec){ for(int k=0;k<1000000;k++){ a=a*b+1e-12; } i+=1000000ULL; }
    g_sink=(float)a;
    return (double)i/(now_s()-t0)/1e6;
}

static int run_sustain2(int threads,double seconds,int wmb,double win,const int *cpus,int ncpu){
    size_t n=(size_t)wmb*1024*1024/sizeof(float); n-=n%32;
    volatile int stop=0;
    arg_t *args=calloc(threads,sizeof(arg_t));
    pthread_t *th=calloc(threads,sizeof(pthread_t));
    for(int i=0;i<threads;i++){
        fill(&args[i],n,8192,1234+i);
        args[i].stop=&stop; args[i].samp=malloc(sizeof(double)*SAMP_CAP); args[i].cnt=0;
        if(ncpu>0){ args[i].ncpu=ncpu; args[i].cpus[0]=cpus[i%ncpu]; }
    }
    for(int i=0;i<threads;i++) pthread_create(&th[i],NULL,worker,&args[i]);

    printf("### sustain threads=%d seconds=%.0f wmb=%d window=%.0f\n",threads,seconds,wmb,win);
    printf("t_s,tokens,tokens_per_s,p10_us,p50_us,big_mhz,lit_mhz,big_c,lit_c\n");

    double t0=now_s();
    unsigned long long last_tok=0,last_cnt[64]={0};
    double *buf=malloc(sizeof(double)*SAMP_CAP);

    while(now_s()-t0<seconds){
        struct timespec req={(time_t)win,(long)((win-(long)win)*1e9)};
        nanosleep(&req,NULL);
        double t=now_s()-t0;
        unsigned long long tok=0; int m=0;
        for(int i=0;i<threads;i++){
            tok+=args[i].tokens;
            unsigned long long c=args[i].cnt,lc=last_cnt[i],w=c-lc;
            if(w>SAMP_CAP) w=SAMP_CAP;
            for(unsigned long long k=c-w;k<c;k++) buf[m++]=args[i].samp[k%SAMP_CAP];
            last_cnt[i]=c;
        }
        double dtok=(double)(tok-last_tok);
        printf("%.1f,%.0f,%.1f,%.1f,%.1f,%d,%d,%d,%d\n",t,dtok,dtok/win,
               pctile(buf,m,0.10),pctile(buf,m,0.50),big_freq(),lit_freq(),big_temp(),lit_temp());
        fflush(stdout);
        last_tok=tok;
    }
    stop=1;
    for(int i=0;i<threads;i++) pthread_join(th[i],NULL);
    printf("### done\n");
    for(int i=0;i<threads;i++){ free(args[i].w); free(args[i].x); free(args[i].samp); }
    free(buf); free(args); free(th);
    return 0;
}

static int run_burst(int threads,double on_s,double off_s,int cycles,int wmb,const int *cpus,int ncpu){
    size_t n=(size_t)wmb*1024*1024/sizeof(float); n-=n%32;
    printf("### burst threads=%d on=%.0f off=%.0f cycles=%d wmb=%d\n",threads,on_s,off_s,cycles,wmb);
    printf("cycle,on_s,tokens,tokens_per_s,cpu_us_per_token,big_mhz,big_c\n");
    for(int cyc=1;cyc<=cycles;cyc++){
        volatile int stop=0;
        arg_t *args=calloc(threads,sizeof(arg_t));
        pthread_t *th=calloc(threads,sizeof(pthread_t));
        for(int i=0;i<threads;i++){
            fill(&args[i],n,8192,777+i); args[i].stop=&stop;
            if(ncpu>0){ args[i].ncpu=ncpu; args[i].cpus[0]=cpus[i%ncpu]; }
        }
        double c0=cpu_s(),t0=now_s();
        for(int i=0;i<threads;i++) pthread_create(&th[i],NULL,worker,&args[i]);
        struct timespec req={(time_t)on_s,(long)((on_s-(long)on_s)*1e9)};
        nanosleep(&req,NULL);
        stop=1;
        for(int i=0;i<threads;i++) pthread_join(th[i],NULL);
        double dt=now_s()-t0,dc=cpu_s()-c0;
        unsigned long long tok=0;
        for(int i=0;i<threads;i++) tok+=args[i].tokens;
        printf("%d,%.1f,%.0f,%.1f,%.1f,%d,%d\n",cyc,dt,(double)tok,tok/dt,
               tok?dc*1e6/tok:0.0,big_freq(),big_temp());
        fflush(stdout);
        for(int i=0;i<threads;i++){ free(args[i].w); free(args[i].x); }
        free(args); free(th);
        if(cyc<cycles){ struct timespec off={(time_t)off_s,0}; nanosleep(&off,NULL); }
    }
    return 0;
}

static int run_probe(double seconds,double interval,const int *cpus,int ncpu){
    arg_t a; memset(&a,0,sizeof a); a.ncpu=ncpu;
    for(int i=0;i<ncpu;i++) a.cpus[i]=cpus[i];
    pin_self(&a);
    printf("### probe seconds=%.0f interval=%.1f\n",seconds,interval);
    printf("t_s,mops_per_s,big_mhz,big_c\n");
    double t0=now_s();
    while(now_s()-t0<seconds){
        double m=probe_ops(interval*0.8);
        printf("%.1f,%.4f,%d,%d\n",now_s()-t0,m,big_freq(),big_temp());
        fflush(stdout);
        struct timespec req={0,(long)(interval*0.2*1e9)};
        nanosleep(&req,NULL);
    }
    return 0;
}

int main(int argc,char **argv){
    int cpus[16],ncpu=0;
    if(argc<2){ fprintf(stderr,"usage: %s sustain2|burst|probe ...\n",argv[0]); return 2; }
    if(!strcmp(argv[1],"sustain2")&&argc>=6){
        if(argc>=7) ncpu=parse_cpus(argv[6],cpus);
        return run_sustain2(atoi(argv[2]),atof(argv[3]),atoi(argv[4]),atof(argv[5]),cpus,ncpu);
    }
    if(!strcmp(argv[1],"burst")&&argc>=7){
        if(argc>=8) ncpu=parse_cpus(argv[7],cpus);
        return run_burst(atoi(argv[2]),atof(argv[3]),atof(argv[4]),atoi(argv[5]),atoi(argv[6]),cpus,ncpu);
    }
    if(!strcmp(argv[1],"probe")&&argc>=4){
        if(argc>=5) ncpu=parse_cpus(argv[4],cpus);
        return run_probe(atof(argv[2]),atof(argv[3]),cpus,ncpu);
    }
    fprintf(stderr,"bad args\n");
    return 2;
}
