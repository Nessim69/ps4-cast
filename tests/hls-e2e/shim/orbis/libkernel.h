// Host shim: just enough of OpenOrbis' libkernel for hls.c/aseg.c/httpsrc.c
// (pthreads, clocks) to run on macOS/Linux in tests/hls-e2e.
#pragma once
#include <stdint.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
typedef struct timeval OrbisKernelTimeval;
typedef pthread_t OrbisPthread;
typedef pthread_mutex_t OrbisPthreadMutex;
typedef pthread_cond_t OrbisPthreadCond;
typedef void *OrbisPthreadAttr;
static inline uint64_t sceKernelGetProcessTime(void){struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);return (uint64_t)ts.tv_sec*1000000ull+ts.tv_nsec/1000;}
static inline int sceKernelUsleep(unsigned us){return usleep(us);}
static inline int sceKernelGettimeofday(OrbisKernelTimeval*tv){return gettimeofday(tv,0);}
static inline int scePthreadMutexInit(OrbisPthreadMutex*m,void*a,const char*n){(void)a;(void)n;pthread_mutexattr_t at;pthread_mutexattr_init(&at);pthread_mutexattr_settype(&at,PTHREAD_MUTEX_RECURSIVE);return pthread_mutex_init(m,&at);}
static inline int scePthreadMutexDestroy(OrbisPthreadMutex*m){return pthread_mutex_destroy(m);}
static inline int scePthreadMutexLock(OrbisPthreadMutex*m){return pthread_mutex_lock(m);}
static inline int scePthreadMutexUnlock(OrbisPthreadMutex*m){return pthread_mutex_unlock(m);}
static inline int scePthreadCondInit(OrbisPthreadCond*c,void*a,const char*n){(void)a;(void)n;return pthread_cond_init(c,0);}
static inline int scePthreadCondDestroy(OrbisPthreadCond*c){return pthread_cond_destroy(c);}
static inline int scePthreadCondSignal(OrbisPthreadCond*c){return pthread_cond_signal(c);}
static inline int scePthreadCondWait(OrbisPthreadCond*c,OrbisPthreadMutex*m){return pthread_cond_wait(c,m);}
static inline int scePthreadCondTimedwait(OrbisPthreadCond*c,OrbisPthreadMutex*m,unsigned us){struct timespec ts;clock_gettime(CLOCK_REALTIME,&ts);ts.tv_sec+=us/1000000;ts.tv_nsec+=(long)(us%1000000)*1000;if(ts.tv_nsec>=1000000000){ts.tv_sec++;ts.tv_nsec-=1000000000;}return pthread_cond_timedwait(c,m,&ts);}
static inline int scePthreadCreate(OrbisPthread*t,void*a,void*(*f)(void*),void*arg,const char*n){(void)a;(void)n;return pthread_create(t,0,f,arg);}
static inline int scePthreadJoin(OrbisPthread t,void**r){return pthread_join(t,r);}
static inline OrbisPthread scePthreadSelf(void){return pthread_self();}
// Files: only "/data/..." and only when $PS4CAST_DATA names a directory to
// stand in for it (the programme-guide test); otherwise opens fail as before.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static inline const char *shim_data_path(const char*p,char*buf,size_t cap){const char*d=getenv("PS4CAST_DATA");if(!d||strncmp(p,"/data/",6)!=0)return NULL;snprintf(buf,cap,"%s/%s",d,p+6);return buf;}
static inline int sceKernelOpen(const char*p,int f,int m){char b[1024];const char*q=shim_data_path(p,b,sizeof b);if(!q)return -1;int hf=(f&3)==1?O_WRONLY:(f&3)==2?O_RDWR:O_RDONLY;if(f&0x0200)hf|=O_CREAT;if(f&0x0400)hf|=O_TRUNC;int fd=open(q,hf,m);return fd<0?-1:fd;}
static inline int sceKernelClose(int f){return f>=0?close(f):0;}
static inline long sceKernelWrite(int f,const void*b,unsigned long n){return f>=0?(long)write(f,b,n):(long)n;}
static inline long sceKernelRead(int f,void*b,unsigned long n){return (long)read(f,b,n);}
static inline long sceKernelLseek(int f,long o,int w){return (long)lseek(f,o,w);}
static inline int sceKernelUnlink(const char*p){char b[1024];const char*q=shim_data_path(p,b,sizeof b);return q?unlink(q):-1;}
static inline int sceKernelRename(const char*a,const char*c){char b1[1024],b2[1024];const char*x=shim_data_path(a,b1,sizeof b1),*y=shim_data_path(c,b2,sizeof b2);return x&&y?rename(x,y):-1;}
static inline int scePthreadDetach(OrbisPthread t){return pthread_detach(t);}
static inline int scePthreadAttrInit(OrbisPthreadAttr*a){*a=0;return 0;}
static inline int scePthreadAttrSetstacksize(OrbisPthreadAttr*a,size_t n){(void)a;(void)n;return 0;}
static inline int scePthreadAttrDestroy(OrbisPthreadAttr*a){(void)a;return 0;}
