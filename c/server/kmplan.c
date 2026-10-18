#include "kmplan.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================ 线程局部上下文 */

typedef struct {
    const crtr_graph *g;
    crtr_astar       *a;
    uint64_t         *bits;
} tls_ctx;

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static DWORD       g_tls;
static INIT_ONCE   g_tls_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK tls_once_cb(PINIT_ONCE o, PVOID p, PVOID *ctx) {
    (void)o; (void)p; (void)ctx;
    g_tls = TlsAlloc();
    return TRUE;
}
static tls_ctx *tls_get(void) {
    InitOnceExecuteOnce(&g_tls_once, tls_once_cb, NULL, NULL);
    return (tls_ctx *)TlsGetValue(g_tls);
}
static void tls_set(tls_ctx *v) { TlsSetValue(g_tls, v); }
/* Win32 TLS 无析构回调；worker 与进程同寿命，退出由 OS 回收 */

#else
#include <pthread.h>

static pthread_key_t  g_tls;
static pthread_once_t g_tls_once = PTHREAD_ONCE_INIT;

static void tls_dtor(void *v) {
    tls_ctx *c = v;
    if (!c) return;
    if (c->a) crtr_astar_free(c->a);
    crtr_bits_free(c->bits);
    free(c);
}
static void tls_once_cb(void) { pthread_key_create(&g_tls, tls_dtor); }
static tls_ctx *tls_get(void) {
    pthread_once(&g_tls_once, tls_once_cb);
    return (tls_ctx *)pthread_getspecific(g_tls);
}
static void tls_set(tls_ctx *v) {
    pthread_once(&g_tls_once, tls_once_cb);
    pthread_setspecific(g_tls, v);
}
#endif

static tls_ctx *tls_ctx_get(const crtr_graph *g) {
    tls_ctx *c = tls_get();
    if (c && c->g == g) return c;
    if (c) {                         /* 图换了（正常流程不会发生）：重建 */
        if (c->a) crtr_astar_free(c->a);
        crtr_bits_free(c->bits);
        memset(c, 0, sizeof *c);
    } else {
        c = calloc(1, sizeof *c);
        if (!c) return NULL;
        tls_set(c);
    }
    c->g = g;
    c->a = crtr_astar_new(g);
    c->bits = crtr_bits_alloc(g);
    if (!c->a || !c->bits) return NULL;
    return c;
}

crtr_astar *crtr_astar_tls_get(const crtr_graph *g) {
    tls_ctx *c = tls_ctx_get(g);
    return c ? c->a : NULL;
}

uint64_t *crtr_bits_tls(const crtr_graph *g) {
    tls_ctx *c = tls_ctx_get(g);
    return c ? c->bits : NULL;
}

/* ================================================================ 并行 for
 *
 * 把 [i0,i1) 均分成 chunks 份：第 1..chunks-1 份投线程池，第 0 份由
 * 调用线程自己执行——即使调用线程本身也是池线程也不会死锁。
 */

#ifdef _WIN32
typedef CRITICAL_SECTION    kp_lock_t;
typedef CONDITION_VARIABLE   kp_cond_t;
#define KP_LOCK_INIT(l)     InitializeCriticalSection(l)
#define KP_LOCK_DESTROY(l)  DeleteCriticalSection(l)
#define KP_LOCK(l)          EnterCriticalSection(l)
#define KP_UNLOCK(l)        LeaveCriticalSection(l)
#define KP_COND_INIT(c)     InitializeConditionVariable(c)
#define KP_COND_DESTROY(c)  (void)(c)
#define KP_COND_WAIT(c, l)  SleepConditionVariableCS(c, l, INFINITE)
#define KP_COND_WAKE(c)     WakeAllConditionVariable(c)
#else
typedef pthread_mutex_t kp_lock_t;
typedef pthread_cond_t  kp_cond_t;
#define KP_LOCK_INIT(l)     pthread_mutex_init(l, NULL)
#define KP_LOCK_DESTROY(l)  pthread_mutex_destroy(l)
#define KP_LOCK(l)          pthread_mutex_lock(l)
#define KP_UNLOCK(l)        pthread_mutex_unlock(l)
#define KP_COND_INIT(c)     pthread_cond_init(c, NULL)
#define KP_COND_DESTROY(c)  pthread_cond_destroy(c)
#define KP_COND_WAIT(c, l)  pthread_cond_wait(c, l)
#define KP_COND_WAKE(c)     pthread_cond_broadcast(c)
#endif

typedef void (*kp_fn)(void *ud, int i);

typedef struct {
    kp_fn       fn;
    void       *ud;
    atomic_int  pending;      /* 未完成的份数 */
    kp_lock_t   lock;
    kp_cond_t   cond;
} kp_parfor;

typedef struct {
    kp_parfor *pf;
    int        c0, c1;
} kp_chunk;

static void kp_chunk_done(kp_parfor *pf) {
    KP_LOCK(&pf->lock);
    atomic_fetch_sub(&pf->pending, 1);
    KP_COND_WAKE(&pf->cond);
    KP_UNLOCK(&pf->lock);
}

static void kp_chunk_run(void *arg) {
    kp_chunk *kc = arg;
    for (int i = kc->c0; i < kc->c1; i++) kc->pf->fn(kc->pf->ud, i);
    kp_parfor *pf = kc->pf;
    free(kc);
    kp_chunk_done(pf);
}

static void parfor(crtr_threadpool *tp, kp_fn fn, void *ud, int i0, int i1) {
    int n = i1 - i0;
    if (n <= 0) return;

    int threads = crtr_tp_threads(tp);
    int chunks = threads < n ? threads : n;
    if (chunks <= 1) {                       /* 无池 / 单线程：串行 */
        for (int i = i0; i < i1; i++) fn(ud, i);
        return;
    }

    kp_parfor pf;
    pf.fn = fn;
    pf.ud = ud;
    atomic_init(&pf.pending, chunks);
    KP_LOCK_INIT(&pf.lock);
    KP_COND_INIT(&pf.cond);

    int base = n / chunks, rem = n % chunks;
    int start = i0;
    for (int c = 0; c < chunks; c++) {
        int len = base + (c < rem ? 1 : 0);
        int c0 = start, c1 = start + len;
        start = c1;
        if (c == 0) {
            for (int i = c0; i < c1; i++) fn(ud, i);   /* 自己干第一份 */
            kp_chunk_done(&pf);
        } else {
            kp_chunk *kc = malloc(sizeof *kc);
            if (!kc) {                                  /* 提交失败：本地补做 */
                for (int i = c0; i < c1; i++) fn(ud, i);
                kp_chunk_done(&pf);
                continue;
            }
            kc->pf = &pf;
            kc->c0 = c0;
            kc->c1 = c1;
            crtr_tp_submit(tp, kp_chunk_run, kc);
        }
    }

    KP_LOCK(&pf.lock);
    while (atomic_load(&pf.pending) > 0) KP_COND_WAIT(&pf.cond, &pf.lock);
    KP_UNLOCK(&pf.lock);
    KP_COND_DESTROY(&pf.cond);
    KP_LOCK_DESTROY(&pf.lock);
}

/* ================================================================ 路径工具 */

typedef struct {
    double   len;        /* real_len（米） */
    int32_t *p;
    int32_t  n;
} kmpath;

/* 深拷贝（失败返回 -1，dst 不变） */
static int path_copy(kmpath *dst, const kmpath *src) {
    int32_t *p = NULL;
    if (src->n > 0) {
        p = malloc((size_t)src->n * sizeof *p);
        if (!p) return -1;
        memcpy(p, src->p, (size_t)src->n * sizeof *p);
    }
    free(dst->p);
    dst->len = src->len;
    dst->n = src->n;
    dst->p = p;
    return 0;
}

/* 预算包装的 A* 搜索：成功返回 1（out 获得路径所有权） */
static int astar_path(const crtr_graph *g, int32_t src, int32_t dst,
                      const crtr_edge_filter *f, kmpath *out,
                      atomic_int *budget) {
    if (budget && atomic_fetch_sub(budget, 1) <= 0) return 0;
    crtr_astar *a = crtr_astar_tls_get(g);
    if (!a) return 0;
    int32_t *p = NULL;
    int32_t n = 0;
    double d = crtr_astar_run(g, a, src, dst, f, &p, &n);
    if (d < 0.0) { free(p); return 0; }
    out->len = d;
    out->p = p;
    out->n = n;
    return 1;
}

/* ================================================================ 阶段2 批任务 */

typedef struct {
    const crtr_graph *g;
    const int32_t    *cand;      /* 只读的当前候选路径 */
    int32_t           i0;        /* 首个 i（=1） */
    int32_t           step;
    int32_t           seg;
    kmpath           *results;   /* results[k] 对应 i = i0 + k*step */
    atomic_int       *budget;
    double            deadline;  /* 超过此刻（monotonic ms）不再开工 */
} seg_batch;

static void seg_one(void *ud, int k) {
    seg_batch *b = ud;
    int i = b->i0 + k * b->step;
    kmpath *out = &b->results[k];
    out->p = NULL;
    out->n = 0;
    out->len = 0.0;

    int32_t u = b->cand[i], v = b->cand[i + b->seg];
    if (u == v) return;                          /* _shortest_avoiding 语义 */
    if (b->deadline > 0 && crtr_monotonic_ms() > b->deadline) return;
    if (b->budget && atomic_fetch_sub(b->budget, 1) <= 0) return;

    uint64_t *bits = crtr_bits_tls(b->g);
    crtr_astar *a = crtr_astar_tls_get(b->g);
    if (!bits || !a) return;
    memset(bits, 0, crtr_bits_bytes(b->g));
    for (int j = i; j < i + b->seg; j++) {       /* 区间内双向禁用 */
        int32_t x = b->cand[j], y = b->cand[j + 1];
        int32_t s1 = crtr_find_edge(b->g, x, y);
        if (s1 >= 0) crtr_bits_set(bits, s1);
        int32_t s2 = crtr_find_edge(b->g, y, x);
        if (s2 >= 0) crtr_bits_set(bits, s2);
    }

    crtr_edge_filter f = { bits, 0.0, 1 };       /* ban 模式 */
    int32_t *p = NULL;
    int32_t n = 0;
    double d = crtr_astar_run(b->g, a, u, v, &f, &p, &n);
    if (d < 0.0) { free(p); return; }
    out->len = d;
    out->p = p;
    out->n = n;
}

/* ================================================================ plan_by_km */

#define KM_BUDGET  1200         /* 单次规划最大搜索数（保险丝） */
#define KM_TIME_MS  8000.0      /* 单次规划墙钟上限（毫秒，保险丝） */

double crtr_plan_by_km(const crtr_graph *g, crtr_threadpool *tp,
                       int32_t src, int32_t dst, double target_km,
                       int32_t **out_path, int32_t *out_len, int *ok) {
    *out_path = NULL;
    *out_len = 0;
    *ok = 0;

    atomic_int budget;
    atomic_init(&budget, KM_BUDGET);
    double deadline = crtr_monotonic_ms() + KM_TIME_MS;

    /* ---- 基线最短路 ---- */
    kmpath base = { 0.0, NULL, 0 };
    if (!astar_path(g, src, dst, NULL, &base, &budget)) return -1.0;
    double target = target_km * 1000.0;
    if (target <= 0.0 || base.len >= target) {
        *out_path = base.p;
        *out_len = base.n;
        return base.len;
    }

    kmpath best = { 0.0, NULL, 0 };
    if (path_copy(&best, &base) != 0) {
        *out_path = base.p;
        *out_len = base.n;
        return base.len;                         /* 内存不足：退回基线 */
    }

    /* ---- 阶段1：全局惩罚迭代（对已选路径全部有向边 *6.0） ---- */
    kmpath candidates[13];                       /* 基线 + 最多 12 次迭代 */
    int ncand = 0;
    candidates[ncand].len = base.len;
    candidates[ncand].p = base.p;                /* 所有权移交给列表 */
    candidates[ncand].n = base.n;
    ncand++;

    uint64_t *pen = crtr_bits_tls(g);
    if (pen) memset(pen, 0, crtr_bits_bytes(g));

    kmpath cur = { base.len, base.p, base.n };   /* 借用（列表持有） */
    for (int iter = 0; iter < 12 && pen; iter++) {
        for (int32_t j = 0; j + 1 < cur.n; j++) {
            int32_t s = crtr_find_edge(g, cur.p[j], cur.p[j + 1]);
            if (s >= 0) crtr_bits_set(pen, s);
        }
        kmpath p2 = { 0.0, NULL, 0 };
        crtr_edge_filter f = { pen, 6.0, 0 };    /* 惩罚模式 */
        if (!astar_path(g, src, dst, &f, &p2, &budget)) break;
        if (ncand < 13) {
            candidates[ncand].len = p2.len;
            candidates[ncand].p = p2.p;
            candidates[ncand].n = p2.n;
            ncand++;
        } else {
            free(p2.p);                          /* 实际不会发生（上限吻合） */
            break;
        }
        if (fabs(p2.len - target) < fabs(best.len - target))
            path_copy(&best, &p2);
        cur = p2;
        if (fabs(p2.len - target) <= 1000.0) break;
    }

    /* ---- 阶段2：局部微调 ---- */
    static const int SEGS[4] = { 30, 60, 120, 200 };
    int order[13];
    for (int i = 0; i < ncand; i++) order[i] = i;
    for (int a = 1; a < ncand; a++) {            /* 稳定排序：|len-target| 升序 */
        int oi = order[a], b = a - 1;
        while (b >= 0 && fabs(candidates[order[b]].len - target) >
                        fabs(candidates[oi].len - target)) {
            order[b + 1] = order[b];
            b--;
        }
        order[b + 1] = oi;
    }
    int top = ncand < 3 ? ncand : 3;

    for (int t = 0; t < top; t++) {
        if (fabs(best.len - target) <= 500.0) break;
        if (crtr_monotonic_ms() > deadline) break;
        kmpath cand = { 0.0, NULL, 0 };
        if (path_copy(&cand, &candidates[order[t]]) != 0) continue;
        if (cand.len >= target || target - cand.len > 1500e3) {
            free(cand.p);
            continue;                            /* 差距 >1500km：填不满 */
        }

        int stop = 0;                            /* OOM 等致命情况退出 */
        for (int si = 0; si < 4 && !stop; si++) {
            int seg = SEGS[si];
            if (fabs(best.len - target) <= 500.0 || cand.len >= target) break;
            if (cand.n <= seg + 2) continue;
            int step = seg / 2 > 1 ? seg / 2 : 1;

            for (;;) {
                if (fabs(best.len - target) <= 500.0) break;
                if (crtr_monotonic_ms() > deadline) { stop = 1; break; }
                /* i 取值：range(1, n-seg-1, step) */
                int32_t imax = cand.n - seg - 1;         /* i < imax */
                int count = imax > 1 ? (int)((imax - 1 + step - 1) / step) : 0;
                if (count <= 0) break;

                kmpath *results = calloc((size_t)count, sizeof *results);
                if (!results) { stop = 1; break; }
                seg_batch batch = { g, cand.p, 1, step, seg, results, &budget,
                                    deadline };
                parfor(tp, seg_one, &batch, 0, count);

                /* i 升序取第一个能改进的替换（对应 Python 的即时变异） */
                int accepted = 0;
                for (int kk = 0; kk < count && !accepted && !stop; kk++) {
                    kmpath *alt = &results[kk];
                    if (!alt->p) continue;
                    int i = 1 + kk * step;
                    /* 拼接：cand[0..i] + alt[1..] + cand[i+seg+1..] */
                    int32_t total = (int32_t)(i + 1 + (alt->n - 1) +
                                              (cand.n - (i + seg + 1)));
                    if (total <= 0) continue;
                    int32_t *np = malloc((size_t)total * sizeof *np);
                    if (!np) { stop = 1; break; }
                    int32_t w = 0;
                    for (int32_t j = 0; j <= i; j++)              np[w++] = cand.p[j];
                    for (int32_t j = 1; j < alt->n; j++)          np[w++] = alt->p[j];
                    for (int32_t j = i + seg + 1; j < cand.n; j++) np[w++] = cand.p[j];

                    double new_len = crtr_path_length(g, np, w);
                    if (fabs(new_len - target) < fabs(best.len - target)) {
                        kmpath newp = { new_len, np, w };
                        kmpath dup = { 0.0, NULL, 0 };
                        if (path_copy(&dup, &newp) != 0) {
                            free(np);
                            stop = 1;
                            break;
                        }
                        free(best.p);
                        best = newp;              /* best 持原件 */
                        free(cand.p);
                        cand = dup;               /* cand 持副本，互不干扰 */
                        accepted = 1;
                    } else {
                        free(np);
                    }
                }
                for (int kk = 0; kk < count; kk++) free(results[kk].p);
                free(results);
                if (stop || !accepted) break;     /* 无改进：换下一个 seg */
            }
        }
        free(cand.p);
    }

    for (int i = 0; i < ncand; i++) free(candidates[i].p);

    double tol = target * 0.05;
    *ok = fabs(best.len - target) <= (5000.0 > tol ? 5000.0 : tol);
    *out_path = best.p;
    *out_len = best.n;
    return best.len;
}
