#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
    #define _POSIX_C_SOURCE 200809L
#endif

#include <stdlib.h>
#include <time.h>

#include "thread.h"

typedef struct ThreadStart {
    ThreadFn fn;
    void *arg;
} ThreadStart;

#if defined(_WIN32)

static DWORD WINAPI thread_entry(LPVOID p) {
    ThreadStart start = *(ThreadStart *)p;
    free(p);
    start.fn(start.arg);
    return 0;
}

bool thread_create(Thread *t, ThreadFn fn, void *arg) {
    ThreadStart *start = malloc(sizeof(ThreadStart));
    if (!start)
        return false;
    *start = (ThreadStart){ fn, arg };

    *t = CreateThread(NULL, (SIZE_T)THREAD_STACK_SIZE, thread_entry, start, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!*t) {
        free(start);
        return false;
    }
    return true;
}

void thread_join(Thread t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}

void mutex_init(Mutex *m)     { InitializeCriticalSection(m); }
void mutex_destroy(Mutex *m)  { DeleteCriticalSection(m); }
void mutex_lock(Mutex *m)     { EnterCriticalSection(m); }
void mutex_unlock(Mutex *m)   { LeaveCriticalSection(m); }

void cond_init(CondVar *cv)               { InitializeConditionVariable(cv); }
void cond_destroy(CondVar *cv)            { (void)cv; }
void cond_wait(CondVar *cv, Mutex *m)     { SleepConditionVariableCS(cv, m, INFINITE); }
void cond_broadcast(CondVar *cv)          { WakeAllConditionVariable(cv); }

void thread_sleep_ms(u32 ms)              { Sleep(ms); }

#else

static void *thread_entry(void *p) {
    ThreadStart start = *(ThreadStart *)p;
    free(p);
    start.fn(start.arg);
    return NULL;
}

bool thread_create(Thread *t, ThreadFn fn, void *arg) {
    ThreadStart *start = malloc(sizeof(ThreadStart));
    if (!start)
        return false;
    *start = (ThreadStart){ fn, arg };

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, (size_t)THREAD_STACK_SIZE);
    bool ok = pthread_create(t, &attr, thread_entry, start) == 0;
    pthread_attr_destroy(&attr);

    if (!ok)
        free(start);
    return ok;
}

void thread_join(Thread t) {
    pthread_join(t, NULL);
}

void mutex_init(Mutex *m)     { pthread_mutex_init(m, NULL); }
void mutex_destroy(Mutex *m)  { pthread_mutex_destroy(m); }
void mutex_lock(Mutex *m)     { pthread_mutex_lock(m); }
void mutex_unlock(Mutex *m)   { pthread_mutex_unlock(m); }

void cond_init(CondVar *cv)               { pthread_cond_init(cv, NULL); }
void cond_destroy(CondVar *cv)            { pthread_cond_destroy(cv); }
void cond_wait(CondVar *cv, Mutex *m)     { pthread_cond_wait(cv, m); }
void cond_broadcast(CondVar *cv)          { pthread_cond_broadcast(cv); }

void thread_sleep_ms(u32 ms) {
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

#endif