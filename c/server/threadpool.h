#ifndef CRTR_THREADPOOL_H
#define CRTR_THREADPOOL_H

/*
 * 固定大小线程池（PLAN §7）
 *
 * - worker 数量创建时固定：默认取硬件线程数，上限 16。
 * - 任务为简单 FIFO 链表队列，互斥锁 + 条件变量保护。
 * - Windows 用 Win32 线程（_beginthreadex）+ CRITICAL_SECTION +
 *   CONDITION_VARIABLE；其余平台用 pthreads。
 * - destroy 语义：停止接新任务，排空已入队任务后回收线程。
 */

typedef struct crtr_threadpool crtr_threadpool;
typedef void (*crtr_task_fn)(void *arg);

/* n_threads <= 0 时取硬件线程数并夹到 [1,16]。失败返回 NULL。 */
crtr_threadpool *crtr_tp_create(int n_threads);

/* 提交一个任务（fn(arg)）。池销毁后不得再调用。 */
void crtr_tp_submit(crtr_threadpool *tp, crtr_task_fn fn, void *arg);

/* 排空队列并销毁池。 */
void crtr_tp_destroy(crtr_threadpool *tp);

/* 池内 worker 数量（调用方据此决定并行策略）。 */
int crtr_tp_threads(const crtr_threadpool *tp);

/* 单调时钟（毫秒，浮点；跨平台工具）。 */
double crtr_monotonic_ms(void);

#endif
