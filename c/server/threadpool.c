#include "threadpool.h"

#include <stdlib.h>
#include <limits.h>

/* ---------------------------------------------------------------- 平台原语 */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>

typedef CRITICAL_SECTION      tp_lock_t;
typedef CONDITION_VARIABLE     tp_cond_t;
typedef HANDLE                 tp_thread_t;

static void tp_lock_init(tp_lock_t *l)            { InitializeCriticalSection(l); }
static void tp_lock_destroy(tp_lock_t *l)         { DeleteCriticalSection(l); }
static void tp_lock(tp_lock_t *l)                 { EnterCriticalSection(l); }
static void tp_unlock(tp_lock_t *l)               { LeaveCriticalSection(l); }
static void tp_cond_init(tp_cond_t *c)            { InitializeConditionVariable(c); }
static void tp_cond_destroy(tp_cond_t *c)         { (void)c; }  /* Win32 无销毁 */
static void tp_cond_wait(tp_cond_t *c, tp_lock_t *l) { SleepConditionVariableCS(c, l, INFINITE); }
static void tp_cond_wake_all(tp_cond_t *c)        { WakeAllConditionVariable(c); }

static tp_thread_t tp_thread_start(unsigned (__stdcall *fn)(void *), void *arg) {
    return (tp_thread_t)_beginthreadex(NULL, 0, fn, arg, 0, NULL);
}
static void tp_thread_join(tp_thread_t t)         { WaitForSingleObject(t, INFINITE); CloseHandle(t); }

static int tp_hw_threads(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}

#else /* pthreads */

#include <pthread.h>
#include <unistd.h>
#include <errno.h>

typedef pthread_mutex_t  tp_lock_t;
typedef pthread_cond_t   tp_cond_t;
typedef pthread_t        tp_thread_t;

static void tp_lock_init(tp_lock_t *l)            { pthread_mutex_init(l, NULL); }
static void tp_lock_destroy(tp_lock_t *l)         { pthread_mutex_destroy(l); }
static void tp_lock(tp_lock_t *l)                 { pthread_mutex_lock(l); }
static void tp_unlock(tp_lock_t *l)               { pthread_mutex_unlock(l); }
static void tp_cond_init(tp_cond_t *c)            { pthread_cond_init(c, NULL); }
static void tp_cond_destroy(tp_cond_t *c)         { pthread_cond_destroy(c); }
static void tp_cond_wait(tp_cond_t *c, tp_lock_t *l) { pthread_cond_wait(c, l); }
static void tp_cond_wake_all(tp_cond_t *c)        { pthread_cond_broadcast(c); }

typedef void * (*tp_fn_posix)(void *);
static tp_thread_t tp_thread_start(tp_fn_posix fn, void *arg) {
    tp_thread_t t;
    if (pthread_create(&t, NULL, fn, arg) != 0) return (tp_thread_t)0;
    return t;
}
static void tp_thread_join(tp_thread_t t)         { pthread_join(t, NULL); }

static int tp_hw_threads(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
}

#endif

/* ---------------------------------------------------------------- 实现 */

typedef struct tp_task {
    crtr_task_fn    fn;
    void           *arg;
    struct tp_task *next;
} tp_task;

struct crtr_threadpool {
    tp_lock_t    lock;
    tp_cond_t    has_work;
    tp_task     *head, *tail;      /* FIFO 队列 */
    int          shutdown;
    int          n_threads;
    tp_thread_t *threads;
};

static void tp_worker_loop(crtr_threadpool *tp) {
    for (;;) {
        tp_task *t = NULL;
        tp_lock(&tp->lock);
        while (tp->head == NULL && !tp->shutdown)
            tp_cond_wait(&tp->has_work, &tp->lock);
        if (tp->head != NULL) {
            t = tp->head;
            tp->head = t->next;
            if (tp->head == NULL) tp->tail = NULL;
        } else if (tp->shutdown) {
            tp_unlock(&tp->lock);
            break;
        }
        tp_unlock(&tp->lock);

        if (t != NULL) {
            t->fn(t->arg);   /* shutdown 置位后仍把队列里的任务执行完 */
            free(t);
        }
    }
}

#ifdef _WIN32
static unsigned __stdcall tp_worker_win(void *p) { tp_worker_loop((crtr_threadpool *)p); return 0; }
#else
static void *tp_worker_posix(void *p)            { tp_worker_loop((crtr_threadpool *)p); return NULL; }
#endif

crtr_threadpool *crtr_tp_create(int n_threads) {
    if (n_threads <= 0) {
        n_threads = tp_hw_threads();
    }
    if (n_threads > 16) n_threads = 16;
    if (n_threads < 1)  n_threads = 1;

    crtr_threadpool *tp = calloc(1, sizeof(*tp));
    if (!tp) return NULL;
    tp->n_threads = n_threads;
    tp->threads = calloc((size_t)n_threads, sizeof(tp_thread_t));
    if (!tp->threads) { free(tp); return NULL; }

    tp_lock_init(&tp->lock);
    tp_cond_init(&tp->has_work);

    for (int i = 0; i < n_threads; i++) {
#ifdef _WIN32
        tp->threads[i] = tp_thread_start(tp_worker_win, tp);
        if (tp->threads[i] == NULL) tp->threads[i] = INVALID_HANDLE_VALUE;
#else
        tp->threads[i] = tp_thread_start(tp_worker_posix, tp);
#endif
    }
    return tp;
}

void crtr_tp_submit(crtr_threadpool *tp, crtr_task_fn fn, void *arg) {
    tp_task *t = malloc(sizeof(*t));
    if (!t) return;               /* 分配失败时静默丢弃；上层有兜底语义 */
    t->fn = fn;
    t->arg = arg;
    t->next = NULL;

    tp_lock(&tp->lock);
    if (tp->tail != NULL) tp->tail->next = t;
    else                  tp->head = t;
    tp->tail = t;
    tp_unlock(&tp->lock);
    tp_cond_wake_all(&tp->has_work);
}

void crtr_tp_destroy(crtr_threadpool *tp) {
    if (!tp) return;

    tp_lock(&tp->lock);
    tp->shutdown = 1;
    tp_unlock(&tp->lock);
    tp_cond_wake_all(&tp->has_work);

    for (int i = 0; i < tp->n_threads; i++) {
#ifdef _WIN32
        if (tp->threads[i] != INVALID_HANDLE_VALUE)
            tp_thread_join(tp->threads[i]);
#else
        if (tp->threads[i] != (tp_thread_t)0)
            tp_thread_join(tp->threads[i]);
#endif
    }
    free(tp->threads);
    tp_cond_destroy(&tp->has_work);
    tp_lock_destroy(&tp->lock);
    free(tp);
}

int crtr_tp_threads(const crtr_threadpool *tp) {
    return tp ? tp->n_threads : 0;
}

/* ---------------------------------------------------------------- 时钟 */

#ifdef _WIN32
double crtr_monotonic_ms(void) {
    static LARGE_INTEGER freq = { 0 };
    LARGE_INTEGER c;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart * 1000.0;
}
#else
#include <time.h>
double crtr_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}
#endif
