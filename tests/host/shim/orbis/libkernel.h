// Host shim: the few libkernel calls the channel store uses (mutex, file
// I/O), for tests/host. "/data/..." paths are redirected under $PS4CAST_DATA
// and FreeBSD open flags are translated to the host's.
#pragma once
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef pthread_mutex_t OrbisPthreadMutex;
static inline int scePthreadMutexInit(OrbisPthreadMutex *m, void *a, const char *n) {
    (void)a; (void)n;
    pthread_mutexattr_t at; pthread_mutexattr_init(&at);
    pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    return pthread_mutex_init(m, &at);
}
static inline int scePthreadMutexLock(OrbisPthreadMutex *m) { return pthread_mutex_lock(m); }
static inline int scePthreadMutexUnlock(OrbisPthreadMutex *m) { return pthread_mutex_unlock(m); }
static inline int sceKernelOpen(const char *p, int f, int m) {
    char path[1024];
    const char *d = getenv("PS4CAST_DATA");
    if (d && strncmp(p, "/data/", 6) == 0) { snprintf(path, sizeof path, "%s/%s", d, p + 6); p = path; }
    int hf = (f & 3) == 1 ? O_WRONLY : (f & 3) == 2 ? O_RDWR : O_RDONLY;
    if (f & 0x0200) hf |= O_CREAT;
    if (f & 0x0400) hf |= O_TRUNC;
    int fd = open(p, hf, m);
    return fd < 0 ? -1 : fd;
}
static inline int  sceKernelClose(int f) { return close(f); }
static inline long sceKernelRead(int f, void *b, size_t n) { return (long)read(f, b, n); }
static inline long sceKernelWrite(int f, const void *b, size_t n) { return (long)write(f, b, n); }
static inline long sceKernelLseek(int f, long o, int w) { return (long)lseek(f, o, w); }
