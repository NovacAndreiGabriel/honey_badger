#ifndef HONEY_BADGER_THREAD_H
#define HONEY_BADGER_THREAD_H

#include "types.h"

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
    typedef HANDLE Thread;
    typedef CRITICAL_SECTION Mutex;
    typedef CONDITION_VARIABLE CondVar;
#else
    #include <pthread.h>
    typedef pthread_t Thread;
    typedef pthread_mutex_t Mutex;
    typedef pthread_cond_t CondVar;
#endif

#define THREAD_STACK_SIZE ((u64)8 * 1024 * 1024)

typedef void (*ThreadFn)(void *arg);

bool thread_create(Thread *t, ThreadFn fn, void *arg);
void thread_join(Thread t);

void mutex_init(Mutex *m);
void mutex_destroy(Mutex *m);
void mutex_lock(Mutex *m);
void mutex_unlock(Mutex *m);

void cond_init(CondVar *cv);
void cond_destroy(CondVar *cv);
void cond_wait(CondVar *cv, Mutex *m);     
void cond_broadcast(CondVar *cv);

void thread_sleep_ms(u32 ms);

#endif