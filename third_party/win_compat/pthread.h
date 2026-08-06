// M19: minimal pthread shim for MSVC, implementing exactly the subset the
// vendored UxPlay core uses (create/join, mutex, cond init/signal/destroy/
// timedwait) on Win32 primitives. Only in the include path for the Windows
// AirPlay target; every other platform uses the real pthread.
#ifndef VKR_WIN_PTHREAD_H
#define VKR_WIN_PTHREAD_H

#ifdef _WIN32

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* pthread_t;
typedef void* pthread_mutex_t;      // opaque; heap CRITICAL_SECTION
typedef void* pthread_mutexattr_t;
typedef void* pthread_cond_t;       // opaque; heap CONDITION_VARIABLE
typedef void* pthread_condattr_t;

int pthread_create(pthread_t* thread, const void* attr, void* (*start)(void*), void* arg);
int pthread_join(pthread_t thread, void** retval);

int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* attr);
int pthread_mutex_lock(pthread_mutex_t* m);
int pthread_mutex_unlock(pthread_mutex_t* m);
int pthread_mutex_destroy(pthread_mutex_t* m);

int pthread_cond_init(pthread_cond_t* c, const pthread_condattr_t* attr);
int pthread_cond_signal(pthread_cond_t* c);
int pthread_cond_broadcast(pthread_cond_t* c);
int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m, const struct timespec* abstime);
int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m);
int pthread_cond_destroy(pthread_cond_t* c);

// POSIX time/sleep the core reaches for (via threads.h / raop_ntp.c).
int vkr_clock_gettime(int clk, struct timespec* ts);
void vkr_usleep(unsigned long usec);

#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif
#define clock_gettime(clk, ts) vkr_clock_gettime((clk), (ts))
#define usleep(us) vkr_usleep((us))

#ifdef __cplusplus
}
#endif

#endif // _WIN32
#endif // VKR_WIN_PTHREAD_H
