// M19: Win32 implementation of the pthread subset (see pthread.h).

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdlib.h>

#include "pthread.h"

typedef struct {
    void* (*start)(void*);
    void* arg;
    void* ret;
} thread_ctx_t;

static DWORD WINAPI thread_trampoline(LPVOID p) {
    thread_ctx_t* ctx = (thread_ctx_t*)p;
    ctx->ret = ctx->start(ctx->arg);
    return 0;
}

int pthread_create(pthread_t* thread, const void* attr, void* (*start)(void*), void* arg) {
    (void)attr;
    thread_ctx_t* ctx = (thread_ctx_t*)calloc(1, sizeof(thread_ctx_t));
    if (!ctx) return -1;
    ctx->start = start;
    ctx->arg = arg;
    HANDLE h = CreateThread(NULL, 0, thread_trampoline, ctx, 0, NULL);
    if (!h) {
        free(ctx);
        return -1;
    }
    // Pack the ctx pointer alongside the handle so join can reap the return
    // value and free it. We store the HANDLE; ctx leaks its struct only if the
    // thread is never joined (the UxPlay core always joins its threads).
    *thread = h;
    return 0;
}

int pthread_join(pthread_t thread, void** retval) {
    if (!thread) return -1;
    WaitForSingleObject((HANDLE)thread, INFINITE);
    if (retval) *retval = NULL; // UxPlay ignores thread return values
    CloseHandle((HANDLE)thread);
    return 0;
}

// Lazy init so a zero-initialized handle (POSIX PTHREAD_MUTEX_INITIALIZER, or
// a struct field the code never explicitly MUTEX_CREATEs - as httpd_t's
// run_mutex) is still a valid, lockable mutex. Thread-safe via an atomic CAS.
static void ensure_mutex(pthread_mutex_t* m) {
    if (*m) return;
    CRITICAL_SECTION* cs = (CRITICAL_SECTION*)malloc(sizeof(CRITICAL_SECTION));
    if (!cs) return;
    InitializeCriticalSection(cs);
    if (InterlockedCompareExchangePointer(m, cs, NULL) != NULL) {
        DeleteCriticalSection(cs); // lost the race
        free(cs);
    }
}

int pthread_mutex_init(pthread_mutex_t* m, const pthread_mutexattr_t* attr) {
    (void)attr;
    *m = NULL;
    ensure_mutex(m);
    return *m ? 0 : -1;
}
int pthread_mutex_lock(pthread_mutex_t* m) {
    ensure_mutex(m);
    EnterCriticalSection((CRITICAL_SECTION*)*m);
    return 0;
}
int pthread_mutex_unlock(pthread_mutex_t* m) {
    if (*m) LeaveCriticalSection((CRITICAL_SECTION*)*m);
    return 0;
}
int pthread_mutex_destroy(pthread_mutex_t* m) {
    if (m && *m) {
        DeleteCriticalSection((CRITICAL_SECTION*)*m);
        free(*m);
        *m = NULL;
    }
    return 0;
}

static void ensure_cond(pthread_cond_t* c) {
    if (*c) return;
    CONDITION_VARIABLE* cv = (CONDITION_VARIABLE*)malloc(sizeof(CONDITION_VARIABLE));
    if (!cv) return;
    InitializeConditionVariable(cv);
    if (InterlockedCompareExchangePointer(c, cv, NULL) != NULL) free(cv); // lost race
}

int pthread_cond_init(pthread_cond_t* c, const pthread_condattr_t* attr) {
    (void)attr;
    *c = NULL;
    ensure_cond(c);
    return *c ? 0 : -1;
}
int pthread_cond_signal(pthread_cond_t* c) {
    ensure_cond(c);
    WakeConditionVariable((CONDITION_VARIABLE*)*c);
    return 0;
}
int pthread_cond_broadcast(pthread_cond_t* c) {
    ensure_cond(c);
    WakeAllConditionVariable((CONDITION_VARIABLE*)*c);
    return 0;
}
int pthread_cond_wait(pthread_cond_t* c, pthread_mutex_t* m) {
    ensure_cond(c);
    ensure_mutex(m);
    SleepConditionVariableCS((CONDITION_VARIABLE*)*c, (CRITICAL_SECTION*)*m, INFINITE);
    return 0;
}
int pthread_cond_timedwait(pthread_cond_t* c, pthread_mutex_t* m,
                           const struct timespec* abstime) {
    struct timespec now;
    ensure_cond(c);
    ensure_mutex(m);
    vkr_clock_gettime(CLOCK_REALTIME, &now);
    long long ms = ((long long)abstime->tv_sec - now.tv_sec) * 1000 +
                   ((long long)abstime->tv_nsec - now.tv_nsec) / 1000000;
    if (ms < 0) ms = 0;
    BOOL ok = SleepConditionVariableCS((CONDITION_VARIABLE*)*c, (CRITICAL_SECTION*)*m,
                                       (DWORD)ms);
    return ok ? 0 : 110 /* ETIMEDOUT */;
}
int pthread_cond_destroy(pthread_cond_t* c) {
    if (c && *c) {
        free(*c);
        *c = NULL;
    }
    return 0;
}

int vkr_clock_gettime(int clk, struct timespec* ts) {
    if (clk == CLOCK_MONOTONIC) {
        static LARGE_INTEGER freq;
        if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        ts->tv_sec = (time_t)(now.QuadPart / freq.QuadPart);
        ts->tv_nsec = (long)(((now.QuadPart % freq.QuadPart) * 1000000000LL) / freq.QuadPart);
    } else {
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        t -= 116444736000000000ULL; // 1601 -> 1970 in 100ns ticks
        ts->tv_sec = (time_t)(t / 10000000ULL);
        ts->tv_nsec = (long)((t % 10000000ULL) * 100);
    }
    return 0;
}

void vkr_usleep(unsigned long usec) {
    if (usec >= 1000)
        Sleep(usec / 1000);
    else if (usec)
        Sleep(1);
}

#endif // _WIN32
