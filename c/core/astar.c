/* A* 最短路引擎 —— 语义对齐 server.py dijkstra() / _shortest_avoiding()。
 *
 * - 启发式 = 到终点 haversine 大圆距离（可采纳且一致）。
 * - 堆为二叉最小堆 + 惰性删除：堆项 (f, d, 节点)，与 Python 的
 *   (f, d, i) 元组同序比较（先 f，再 d，再节点号），弹出已 done 项跳过。
 * - dist/prev/done 用「代际号」管理有效性：每次搜索代际 +1，旧数据
 *   自然失效，避免整图 memset（图约 20 万节点，热查询收益明显）。
 * - 边过滤：bits 位图按有向边槽标记，ban=1 完全跳过（对应
 *   _shortest_avoiding），否则 w *= factor（对应 plan_by_km 惩罚形态）。
 *   返回值为过滤语义下的搜索距离（同 Python dijkstra 的返回）。
 */
#include "astar.h"
#include "geom.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

struct crtr_astar {
    int32_t n;
    double *dist;        /* [n] 最短距离估计 */
    int32_t *prev;       /* [n] 前驱节点 */
    uint32_t *gen;       /* [n] dist 有效代际 */
    uint32_t *dgen;      /* [n] done 代际 */
    uint32_t cur;        /* 当前代际（>=1，回绕时全清） */
    /* 二叉最小堆（数组隐式存储） */
    int32_t heap_cap;
    int32_t heap_len;
    double *hf;          /* f = g + h */
    double *hd;          /* g */
    int32_t *hi;         /* 节点下标 */
};

#define CRTR_HEAP_MIN 256

crtr_astar *crtr_astar_new(const crtr_graph *g)
{
    crtr_astar *a;
    size_t n;

    if (!g)
        return NULL;
    n = (size_t)(g->n_nodes > 0 ? g->n_nodes : 1);
    a = (crtr_astar *)calloc(1, sizeof *a);
    if (!a)
        return NULL;
    a->n = g->n_nodes;
    a->dist = (double *)malloc(n * sizeof(double));
    a->prev = (int32_t *)malloc(n * sizeof(int32_t));
    a->gen = (uint32_t *)calloc(n, sizeof(uint32_t));
    a->dgen = (uint32_t *)calloc(n, sizeof(uint32_t));
    a->heap_cap = CRTR_HEAP_MIN;
    a->hf = (double *)malloc((size_t)a->heap_cap * sizeof(double));
    a->hd = (double *)malloc((size_t)a->heap_cap * sizeof(double));
    a->hi = (int32_t *)malloc((size_t)a->heap_cap * sizeof(int32_t));
    if (!a->dist || !a->prev || !a->gen || !a->dgen ||
        !a->hf || !a->hd || !a->hi) {
        crtr_astar_free(a);
        return NULL;
    }
    return a;
}

void crtr_astar_free(crtr_astar *a)
{
    if (!a)
        return;
    free(a->dist);
    free(a->prev);
    free(a->gen);
    free(a->dgen);
    free(a->hf);
    free(a->hd);
    free(a->hi);
    free(a);
}

/* 代际状态读写 */
static double dist_of(const crtr_astar *a, int32_t v)
{
    return a->gen[v] == a->cur ? a->dist[v] : (double)INFINITY;
}

static int done_has(const crtr_astar *a, int32_t v)
{
    return a->dgen[v] == a->cur;
}

/* 堆项比较：先 f、再 d、再节点号（同 Python 元组序） */
static int heap_less(const crtr_astar *a, int32_t x, int32_t y)
{
    if (a->hf[x] != a->hf[y])
        return a->hf[x] < a->hf[y];
    if (a->hd[x] != a->hd[y])
        return a->hd[x] < a->hd[y];
    return a->hi[x] < a->hi[y];
}

static void heap_swap(crtr_astar *a, int32_t x, int32_t y)
{
    double tf = a->hf[x], td = a->hd[x];
    int32_t ti = a->hi[x];
    a->hf[x] = a->hf[y]; a->hd[x] = a->hd[y]; a->hi[x] = a->hi[y];
    a->hf[y] = tf;       a->hd[y] = td;       a->hi[y] = ti;
}

static void heap_sift_up(crtr_astar *a, int32_t i)
{
    while (i > 0) {
        int32_t p = (i - 1) >> 1;
        if (!heap_less(a, i, p))
            break;
        heap_swap(a, i, p);
        i = p;
    }
}

static void heap_sift_down(crtr_astar *a, int32_t i)
{
    for (;;) {
        int32_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < a->heap_len && heap_less(a, l, m))
            m = l;
        if (r < a->heap_len && heap_less(a, r, m))
            m = r;
        if (m == i)
            break;
        heap_swap(a, i, m);
        i = m;
    }
}

static int heap_push(crtr_astar *a, double f, double d, int32_t v)
{
    if (a->heap_len >= a->heap_cap) {
        int32_t ncap = a->heap_cap * 2;
        double *nf = (double *)realloc(a->hf, (size_t)ncap * sizeof(double));
        double *nd = (nf ? (double *)realloc(a->hd, (size_t)ncap * sizeof(double)) : NULL);
        int32_t *ni = (nd ? (int32_t *)realloc(a->hi, (size_t)ncap * sizeof(int32_t)) : NULL);
        if (!ni) {
            /* 任一 realloc 失败：回退已成功的指针，放弃本次扩容 */
            if (nf) a->hf = nf;
            if (nd) a->hd = nd;
            return -1;
        }
        a->hf = nf;
        a->hd = nd;
        a->hi = ni;
        a->heap_cap = ncap;
    }
    {
        int32_t i = a->heap_len++;
        a->hf[i] = f;
        a->hd[i] = d;
        a->hi[i] = v;
        heap_sift_up(a, i);
    }
    return 0;
}

double crtr_astar_run(const crtr_graph *g, crtr_astar *a,
                      int32_t src, int32_t dst,
                      const crtr_edge_filter *filter,
                      int32_t **out_path, int32_t *out_len)
{
    double tlat, tlon, result;
    int32_t len = 0, c, k;
    int32_t *path = NULL;

    if (out_path)
        *out_path = NULL;
    if (out_len)
        *out_len = 0;
    if (!g || !a || g->n_nodes <= 0)
        return -1.0;
    if ((uint32_t)src >= (uint32_t)g->n_nodes ||
        (uint32_t)dst >= (uint32_t)g->n_nodes)
        return -1.0;
    if (src == dst) {
        path = (int32_t *)malloc(sizeof(int32_t));
        if (!path)
            return -1.0;
        path[0] = src;
        if (out_path)
            *out_path = path;
        else
            free(path);
        if (out_len)
            *out_len = 1;
        return 0.0;
    }

    /* 代际推进（回绕时全量清零重新计代） */
    if (++a->cur == 0) {
        memset(a->gen, 0, (size_t)a->n * sizeof(uint32_t));
        memset(a->dgen, 0, (size_t)a->n * sizeof(uint32_t));
        a->cur = 1;
    }
    a->heap_len = 0;
    a->dist[src] = 0.0;
    a->gen[src] = a->cur;
    a->prev[src] = -1;
    tlat = g->lat[dst];
    tlon = g->lon[dst];
    if (heap_push(a, crtr_haversine(g->lat[src], g->lon[src], tlat, tlon), 0.0, src) != 0)
        return -1.0;

    while (a->heap_len > 0) {
        double d = a->hd[0];
        int32_t i = a->hi[0];
        /* 弹出堆顶（惰性删除：done 项直接丢弃） */
        a->heap_len--;
        heap_swap(a, 0, a->heap_len);
        heap_sift_down(a, 0);
        if (done_has(a, i))
            continue;
        a->dgen[i] = a->cur;
        if (i == dst)
            break;
        for (k = g->head[i]; k < g->head[i + 1]; k++) {
            int32_t j = g->eto[k];
            double w = g->ew[k];
            double nd;
            if (done_has(a, j))
                continue;
            if (filter && filter->bits && crtr_bits_get(filter->bits, k)) {
                if (filter->ban)
                    continue;
                w *= filter->factor;
            }
            nd = d + w;
            if (nd < dist_of(a, j)) {
                a->dist[j] = nd;
                a->gen[j] = a->cur;
                a->prev[j] = i;
                if (heap_push(a, nd + crtr_haversine(g->lat[j], g->lon[j], tlat, tlon), nd, j) != 0)
                    return -1.0;
            }
        }
    }

    if (a->gen[dst] != a->cur)
        return -1.0;    /* 不可达 */

    /* 回溯路径（prev 仅在本代有效；计数上限防御异常数据） */
    for (c = dst; ; c = a->prev[c]) {
        len++;
        if (c == src || len > a->n)
            break;
    }
    if (c != src)
        return -1.0;
    path = (int32_t *)malloc((size_t)len * sizeof(int32_t));
    if (!path)
        return -1.0;
    {
        int32_t idx = len - 1;
        for (c = dst; ; c = a->prev[c]) {
            path[idx--] = c;
            if (c == src)
                break;
        }
    }
    result = a->dist[dst];
    if (out_path)
        *out_path = path;
    else
        free(path);
    if (out_len)
        *out_len = len;
    return result;
}

/* ---------------------------------------------------------------- 位图 */

uint64_t *crtr_bits_alloc(const crtr_graph *g)
{
    size_t bytes = crtr_bits_bytes(g);
    /* calloc(0) 行为实现定义，至少分配一个字保证非 NULL */
    return (uint64_t *)calloc(1, bytes ? bytes : sizeof(uint64_t));
}

void crtr_bits_free(uint64_t *bits)
{
    free(bits);
}

size_t crtr_bits_bytes(const crtr_graph *g)
{
    size_t e = g ? (size_t)(g->n_edges > 0 ? g->n_edges : 0) : 0;
    return ((e + 63) / 64) * sizeof(uint64_t);
}
