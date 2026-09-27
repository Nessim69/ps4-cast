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
static inline int sceKernelOpen(const char*p,int f,int m){(void)p;(void)f;(void)m;return -1;}
static inline int sceKernelClose(int f){(void)f;return 0;}
static inline long sceKernelWrite(int f,const void*b,unsigned long n){(void)f;(void)b;return (long)n;}
