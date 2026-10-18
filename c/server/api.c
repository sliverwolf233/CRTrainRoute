#include "api.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "../core/geom.h"
#include "kmplan.h"
#include "station.h"

#ifdef _WIN32
typedef CRITICAL_SECTION  svc_lock_t;
#define SVC_LOCK_INIT(l)    InitializeCriticalSection(l)
#define SVC_LOCK_DESTROY(l) DeleteCriticalSection(l)
#define SVC_LOCK(l)         EnterCriticalSection(l)
#define SVC_UNLOCK(l)       LeaveCriticalSection(l)
#else
#include <pthread.h>
#include <time.h>
typedef pthread_mutex_t svc_lock_t;
#define SVC_LOCK_INIT(l)    pthread_mutex_init(l, NULL)
#define SVC_LOCK_DESTROY(l) pthread_mutex_destroy(l)
#define SVC_LOCK(l)         pthread_mutex_lock(l)
#define SVC_UNLOCK(l)       pthread_mutex_unlock(l)
#endif

/* ================================================================ 路由缓存
 *
 * key = (src 节点, dst 节点)，互斥锁保护，条数达 512 整表清空（同
 * server.py 语义）。条目带引用计数：清空时仍被响应持有的条目先「停靠」，
 * 最后一个引用释放时才真正回收（destroy 时统一兜底）。
 */

#define CACHE_SLOTS 2048   /* 2 的幂 */
#define CACHE_MAX   512

typedef struct cache_entry {
    uint64_t           key;
    crtr_route_result  rr;
    int                refs;
    int                in_table;
    struct cache_entry *parked_next;
} cache_entry;

struct crtr_service {
    const crtr_graph   *g;
    crtr_threadpool    *tp;
    crtr_stationdb     *stations;
    char                webroot[512];
    int                 port;
    svc_lock_t          cache_lock;
    cache_entry        *slots[CACHE_SLOTS];
    int                 count;
    cache_entry        *parked;      /* 已从表中摘除、等引用归零的条目 */
};

static uint64_t key_hash(uint64_t k) {
    k ^= k >> 33;
    k *= 0xFF51AFD7ED558CCDULL;
    k ^= k >> 33;
    k *= 0xC4CEB9FE1A85EC53ULL;
    k ^= k >> 33;
    return k;
}

static void entry_free(cache_entry *e) {
    free(e->rr.coords);
    free(e->rr.passed);
    free(e->rr.ferries);
    free(e);
}

/* 命中返回条目（refs 已 +1）；未命中返回 NULL。 */
static cache_entry *cache_acquire(crtr_service *s, uint64_t key) {
    SVC_LOCK(&s->cache_lock);
    size_t i = key_hash(key) & (CACHE_SLOTS - 1);   /* 开放寻址线性探测 */
    for (size_t probe = 0; probe < CACHE_SLOTS; probe++) {
        cache_entry *c = s->slots[i];
        if (!c) break;
        if (c->key == key) {
            c->refs++;
            SVC_UNLOCK(&s->cache_lock);
            return c;
        }
        i = (i + 1) & (CACHE_SLOTS - 1);
    }
    SVC_UNLOCK(&s->cache_lock);
    return NULL;
}

static void cache_release(crtr_service *s, cache_entry *e) {
    if (!e) return;
    SVC_LOCK(&s->cache_lock);
    if (--e->refs <= 0 && !e->in_table) {
        /* 从停靠链摘除 */
        cache_entry **pp = &s->parked;
        while (*pp && *pp != e) pp = &(*pp)->parked_next;
        if (*pp) *pp = e->parked_next;
        entry_free(e);
    }
    SVC_UNLOCK(&s->cache_lock);
}

/* 插入（rr 的数组所有权转移给缓存）。返回持有一次引用的条目。 */
static cache_entry *cache_insert(crtr_service *s, uint64_t key,
                                 crtr_route_result *rr) {
    SVC_LOCK(&s->cache_lock);
    if (s->count >= CACHE_MAX) {                 /* 同 Python：满则整表清空 */
        for (int i = 0; i < CACHE_SLOTS; i++) {
            cache_entry *c = s->slots[i];
            if (!c) continue;
            c->in_table = 0;
            if (c->refs <= 0) entry_free(c);
            else {                               /* 仍被持有：停靠等释放 */
                c->parked_next = s->parked;
                s->parked = c;
            }
            s->slots[i] = NULL;
        }
        s->count = 0;
    }
    cache_entry *e = calloc(1, sizeof *e);
    if (e) {
        e->key = key;
        e->rr = *rr;
        e->refs = 1;                             /* 调用方（本次响应）持有 */
        e->in_table = 1;
        size_t i = key_hash(key) & (CACHE_SLOTS - 1);
        while (s->slots[i]) i = (i + 1) & (CACHE_SLOTS - 1);
        s->slots[i] = e;
        s->count++;
    }
    SVC_UNLOCK(&s->cache_lock);
    return e;
}

crtr_service *crtr_service_create(const crtr_graph *g, crtr_threadpool *tp,
                                  const char *webroot, int port) {
    crtr_service *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->g = g;
    s->tp = tp;
    s->port = port;
    snprintf(s->webroot, sizeof s->webroot, "%s", webroot ? webroot : ".");
    s->stations = crtr_stationdb_new(g);
    if (!s->stations) { free(s); return NULL; }
    SVC_LOCK_INIT(&s->cache_lock);
    return s;
}

void crtr_service_destroy(crtr_service *s) {
    if (!s) return;
    for (int i = 0; i < CACHE_SLOTS; i++)
        if (s->slots[i]) entry_free(s->slots[i]);
    cache_entry *p = s->parked;
    while (p) { cache_entry *n = p->parked_next; entry_free(p); p = n; }
    crtr_stationdb_free(s->stations);
    SVC_LOCK_DESTROY(&s->cache_lock);
    free(s);
}

/* ================================================================ _path_coords 移植
 *
 * 链方向按「链首/链尾谁离边起点 a 近」判定；跳过等于 a 的首点；链尾不
 * 是 b 则补 b；相邻重复点（微度相等）去重；无链退化为直线（PLAN §6）。
 */

static int ferry_hit(const crtr_graph *g, int32_t a, int32_t b) {
    if (g->n_ferry <= 0) return 0;
    int64_t al = crtr_to_u6(g->lat[a]), an = crtr_to_u6(g->lon[a]);
    int64_t bl = crtr_to_u6(g->lat[b]), bn = crtr_to_u6(g->lon[b]);
    for (int i = 0; i < g->n_ferry; i++) {
        const int64_t *f = g->ferry[i];
        if ((f[0] == al && f[1] == an && f[2] == bl && f[3] == bn) ||
            (f[0] == bl && f[1] == bn && f[2] == al && f[3] == an))
            return 1;
    }
    return 0;
}

static int coords_push(crtr_coord **arr, int *n, int *cap, double lat, double lon) {
    if (*n >= *cap) {
        int nc = *cap ? *cap * 2 : 64;
        crtr_coord *na = realloc(*arr, (size_t)nc * sizeof *na);
        if (!na) return -1;
        *arr = na;
        *cap = nc;
    }
    (*arr)[*n].lat = crtr_round(lat, 6);
    (*arr)[*n].lon = crtr_round(lon, 6);
    (*n)++;
    return 0;
}

static int path_coords(const crtr_graph *g, const int32_t *path, int32_t npath,
                       crtr_route_result *rr) {
    rr->coords = NULL;
    rr->n_coords = 0;
    rr->ferries = NULL;
    rr->n_ferries = 0;
    if (npath <= 0) return -1;

    int cap = 0;
    int64_t last_lat = 0, last_lon = 0;
    if (coords_push(&rr->coords, &rr->n_coords, &cap,
                    g->lat[path[0]], g->lon[path[0]]) != 0) return -1;
    last_lat = crtr_to_u6(g->lat[path[0]]);
    last_lon = crtr_to_u6(g->lon[path[0]]);

    int fcap = 0;
    for (int32_t e = 0; e + 1 < npath; e++) {
        int32_t a = path[e], b = path[e + 1];
        int is_ferry = ferry_hit(g, a, b);
        int start_idx = rr->n_coords - 1;

        int64_t a_lat = crtr_to_u6(g->lat[a]), a_lon = crtr_to_u6(g->lon[a]);
        int64_t b_lat = crtr_to_u6(g->lat[b]), b_lon = crtr_to_u6(g->lon[b]);

        /* 几何链：egeom 指向无向条目，方向运行时判别 */
        int64_t first = -1, last = -1;
        int32_t slot = crtr_find_edge(g, a, b);
        if (slot >= 0 && g->egeom[slot] >= 0) {
            int32_t gi = g->egeom[slot];
            if (g->goff[gi] < g->goff[gi + 1]) {
                first = g->goff[gi];
                last = g->goff[gi + 1] - 1;      /* 点下标（每点 2 个 f64） */
            }
        }
        int chain_cnt = (first >= 0) ? (int)(last - first + 1) : 0;

        int forward = 1;
        int64_t last_seq_lat = 0, last_seq_lon = 0;
        if (chain_cnt > 0) {
            double d_first = crtr_haversine(g->lat[a], g->lon[a],
                                            g->gpts[2 * first], g->gpts[2 * first + 1]);
            double d_last = crtr_haversine(g->lat[a], g->lon[a],
                                           g->gpts[2 * last], g->gpts[2 * last + 1]);
            forward = (d_first <= d_last);
            last_seq_lat = crtr_to_u6(forward ? g->gpts[2 * last] : g->gpts[2 * first]);
            last_seq_lon = crtr_to_u6(forward ? g->gpts[2 * last + 1] : g->gpts[2 * first + 1]);
        }

        for (int k = 0; k < chain_cnt; k++) {
            int64_t pi = forward ? (first + k) : (last - k);
            double clat = g->gpts[2 * pi], clon = g->gpts[2 * pi + 1];
            if (k == 0 && crtr_to_u6(clat) == a_lat && crtr_to_u6(clon) == a_lon)
                continue;                         /* 首点等于 a：跳过 */
            if (crtr_to_u6(clat) == last_lat && crtr_to_u6(clon) == last_lon)
                continue;                         /* 相邻重复点去重 */
            if (coords_push(&rr->coords, &rr->n_coords, &cap, clat, clon) != 0)
                return -1;
            last_lat = crtr_to_u6(clat);
            last_lon = crtr_to_u6(clon);
        }
        /* 链尾不是 b（或无链）：补终点 */
        if (chain_cnt == 0 || last_seq_lat != b_lat || last_seq_lon != b_lon) {
            if (b_lat != last_lat || b_lon != last_lon) {
                if (coords_push(&rr->coords, &rr->n_coords, &cap,
                                g->lat[b], g->lon[b]) != 0) return -1;
                last_lat = b_lat;
                last_lon = b_lon;
            }
        }

        if (is_ferry) {
            if (rr->n_ferries >= fcap) {
                int fc = fcap ? fcap * 2 : 4;
                crtr_ferry_seg *nf = realloc(rr->ferries, (size_t)fc * sizeof *nf);
                if (!nf) return -1;
                rr->ferries = nf;
                fcap = fc;
            }
            crtr_ferry_seg *fs = &rr->ferries[rr->n_ferries++];
            fs->from = rr->coords[start_idx];     /* from = 本段起点 */
            fs->to = rr->coords[rr->n_coords - 1];
            fs->km = crtr_round(crtr_haversine(g->lat[a], g->lon[a],
                                               g->lat[b], g->lon[b]) / 1000.0, 2);
        }
    }
    return 0;
}

/* ================================================================ _stations_along 移植 */

#define TOL_M 800.0
#define SGRID 0.05

typedef struct {
    int64_t   key;                /* (gx<<32)|gy */
    int32_t  *segs;
    int       n, cap;
    int       used;
} cellbucket;

typedef struct {
    cellbucket *slots;
    size_t      cap;              /* 2 的幂 */
} seggrid;

static size_t sg_hash(int64_t k) {
    uint64_t h = (uint64_t)k;
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDULL;
    h ^= h >> 29;
    return (size_t)h;
}

static cellbucket *sg_get(seggrid *sg, int64_t key, int create) {
    if (sg->cap == 0) {
        if (!create) return NULL;
        sg->cap = 1024;
        sg->slots = calloc(sg->cap, sizeof *sg->slots);
        if (!sg->slots) { sg->cap = 0; return NULL; }
    }
    size_t i = sg_hash(key) & (sg->cap - 1);
    for (;;) {
        cellbucket *b = &sg->slots[i];
        if (!b->used) {
            if (!create) return NULL;
            b->used = 1;
            b->key = key;
            return b;
        }
        if (b->key == key) return b;
        i = (i + 1) & (sg->cap - 1);
    }
}

static int sg_add(seggrid *sg, int64_t key, int32_t seg) {
    cellbucket *b = sg_get(sg, key, 1);
    if (!b) return -1;
    if (b->n >= b->cap) {
        int nc = b->cap ? b->cap * 2 : 4;
        int32_t *nv = realloc(b->segs, (size_t)nc * sizeof *nv);
        if (!nv) return -1;
        b->segs = nv;
        b->cap = nc;
    }
    b->segs[b->n++] = seg;
    return 0;
}

static void sg_free(seggrid *sg) {
    if (!sg->slots) return;
    for (size_t i = 0; i < sg->cap; i++) free(sg->slots[i].segs);
    free(sg->slots);
    sg->slots = NULL;
    sg->cap = 0;
}

static int dbl_cmp_stable(double a, double b) { return a < b ? -1 : a > b ? 1 : 0; }

static int stations_along(const crtr_graph *g, const char *from_code,
                          const char *to_code, const crtr_coord *coords,
                          int n_coords, crtr_route_result *rr) {
    rr->passed = NULL;
    rr->n_passed = 0;
    if (n_coords < 2 || g->n_stations <= 0) return 0;

    /* 累计里程（与坐标一一对应） */
    double *cum = malloc((size_t)n_coords * sizeof *cum);
    int32_t *stamp = calloc((size_t)n_coords, sizeof *stamp);
    crtr_passed_station *pv = NULL;              /* 提前声明：oom 路径要 free */
    if (!cum || !stamp) { free(cum); free(stamp); return -1; }
    cum[0] = 0.0;
    for (int i = 0; i + 1 < n_coords; i++)
        cum[i + 1] = cum[i] + crtr_haversine(coords[i].lat, coords[i].lon,
                                             coords[i + 1].lat, coords[i + 1].lon);

    /* 包围盒粗筛 */
    double pad = TOL_M / 90000.0 + 0.02;
    double lo_lon = coords[0].lon, hi_lon = coords[0].lon;
    double lo_lat = coords[0].lat, hi_lat = coords[0].lat;
    for (int i = 1; i < n_coords; i++) {
        if (coords[i].lon < lo_lon) lo_lon = coords[i].lon;
        if (coords[i].lon > hi_lon) hi_lon = coords[i].lon;
        if (coords[i].lat < lo_lat) lo_lat = coords[i].lat;
        if (coords[i].lat > hi_lat) hi_lat = coords[i].lat;
    }
    lo_lon -= pad; hi_lon += pad;
    lo_lat -= pad; hi_lat += pad;

    /* 路径折线按 0.05 度网格分桶 */
    seggrid sg = { NULL, 0 };
    for (int i = 0; i + 1 < n_coords; i++) {
        int gx0 = (int)(coords[i].lon / SGRID), gx1 = (int)(coords[i + 1].lon / SGRID);
        int gy0 = (int)(coords[i].lat / SGRID), gy1 = (int)(coords[i + 1].lat / SGRID);
        if (gx0 > gx1) { int t = gx0; gx0 = gx1; gx1 = t; }
        if (gy0 > gy1) { int t = gy0; gy0 = gy1; gy1 = t; }
        if (gx1 - gx0 > 40 || gy1 - gy0 > 40) { gx1 = gx0; gy1 = gy0; }
        for (int gx = gx0; gx <= gx1; gx++)
            for (int gy = gy0; gy <= gy1; gy++) {
                if (sg_add(&sg, ((int64_t)gx << 32) ^ (int64_t)(uint32_t)gy, i) != 0)
                    goto oom;
            }
    }

    /* 逐车站：邻近桶内线段取最小垂距 */
    int pcap = 32;
    pv = malloc((size_t)pcap * sizeof *pv);
    if (!pv) goto oom;
    for (int32_t si = 0; si < g->n_stations; si++) {
        const char *code = g->st_code[si];
        if (!code) continue;
        if ((from_code && strcmp(code, from_code) == 0) ||
            (to_code && strcmp(code, to_code) == 0)) continue;
        double lat = g->st_lat[si], lon = g->st_lon[si];
        if (lon < lo_lon || lon > hi_lon || lat < lo_lat || lat > hi_lat) continue;

        int gx = (int)(lon / SGRID), gy = (int)(lat / SGRID);
        double best_d = 1e18, best_km = 0.0;
        int stamp_v = si + 1;                    /* 每站独立戳记 */
        for (int dx = -1; dx <= 1 && best_d > TOL_M; dx++) {
            for (int dy = -1; dy <= 1; dy++) {
                cellbucket *b = sg_get(&sg,
                    ((int64_t)(gx + dx) << 32) ^ (int64_t)(uint32_t)(gy + dy), 0);
                if (!b) continue;
                for (int k = 0; k < b->n; k++) {
                    int32_t id = b->segs[k];
                    if (stamp[id] == stamp_v) continue;
                    stamp[id] = stamp_v;
                    double d = crtr_seg_point_dist(lat, lon,
                        coords[id].lat, coords[id].lon,
                        coords[id + 1].lat, coords[id + 1].lon);
                    if (d < best_d) { best_d = d; best_km = cum[id]; }
                }
            }
        }
        if (best_d <= TOL_M) {
            if (rr->n_passed >= pcap) {
                int nc = pcap * 2;
                crtr_passed_station *npv = realloc(pv, (size_t)nc * sizeof *npv);
                if (!npv) goto oom;
                pv = npv;
                pcap = nc;
            }
            crtr_passed_station *ps = &pv[rr->n_passed++];
            ps->code = code;
            ps->name = g->st_name[si] ? g->st_name[si] : "";
            ps->lat = crtr_round(lat, 6);
            ps->lon = crtr_round(lon, 6);
            ps->km = crtr_round(best_km / 1000.0, 2);
            ps->offset_m = crtr_round(best_d, 0);
        }
    }

    /* 按沿线里程稳定排序（Python sort 为稳定排序） */
    for (int a = 1; a < rr->n_passed; a++) {
        crtr_passed_station tmp = pv[a];
        int b = a - 1;
        while (b >= 0 && dbl_cmp_stable(pv[b].km, tmp.km) > 0) {
            pv[b + 1] = pv[b];
            b--;
        }
        pv[b + 1] = tmp;
    }

    /* 同名去重：保 offset 最小（先到先得），位置保持首次出现处 */
    int m = 0;
    for (int i = 0; i < rr->n_passed; i++) {
        int found = -1;
        for (int j = 0; j < m; j++)
            if (strcmp(pv[j].name, pv[i].name) == 0) { found = j; break; }
        if (found < 0) pv[m++] = pv[i];
        else if (pv[i].offset_m < pv[found].offset_m) pv[found] = pv[i];
    }

    /* 再按 km 稳定排序 */
    for (int a = 1; a < m; a++) {
        crtr_passed_station tmp = pv[a];
        int b = a - 1;
        while (b >= 0 && dbl_cmp_stable(pv[b].km, tmp.km) > 0) {
            pv[b + 1] = pv[b];
            b--;
        }
        pv[b + 1] = tmp;
    }

    /* 同址异名剔除：别名声向的站在其 aliasOf 同场出现时去掉 */
    int w = 0;
    for (int i = 0; i < m; i++) {
        const char *alias = crtr_lane_alias(g, pv[i].code);
        if (alias) {
            int present = 0;
            for (int j = 0; j < m && !present; j++)
                if (strcmp(pv[j].code, alias) == 0) present = 1;
            if (!present && from_code && strcmp(from_code, alias) == 0) present = 1;
            if (!present && to_code && strcmp(to_code, alias) == 0) present = 1;
            if (present) continue;
        }
        pv[w++] = pv[i];
    }

    rr->passed = pv;
    rr->n_passed = w;
    free(cum);
    free(stamp);
    sg_free(&sg);
    return 0;

oom:
    free(pv);
    free(cum);
    free(stamp);
    sg_free(&sg);
    return -1;
}

/* ================================================================ 结果构建 */

static int build_route_result(const crtr_graph *g, const int32_t *path,
                              int32_t npath, double length,
                              const char *from_code, const char *to_code,
                              crtr_route_result *rr) {
    memset(rr, 0, sizeof *rr);
    rr->length = length;
    rr->n_nodes = npath;
    if (path_coords(g, path, npath, rr) != 0) return -1;
    if (stations_along(g, from_code, to_code, rr->coords, rr->n_coords, rr) != 0) {
        free(rr->coords);
        rr->coords = NULL;
        return -1;
    }
    return 0;
}

/* ================================================================ 算路入口 */

static void fill_endpoint(const crtr_graph *g, int32_t si, crtr_endpoint *ep) {
    ep->code = g->st_code[si];
    ep->name = (g->st_name[si] && g->st_name[si][0]) ? g->st_name[si]
              : (crtr_name_of(g, ep->code) ? crtr_name_of(g, ep->code) : ep->code);
    ep->lat = g->st_lat[si];
    ep->lon = g->st_lon[si];
    ep->city = (g->st_city[si] && g->st_city[si][0]) ? g->st_city[si] : NULL;
    ep->addr = (g->st_addr[si] && g->st_addr[si][0]) ? g->st_addr[si] : NULL;
    ep->snap_offset_m = 0.0;
}

int crtr_service_route(crtr_service *s, const char *from_q, const char *to_q,
                       int has_km, double km, crtr_route_response *resp) {
    memset(resp, 0, sizeof *resp);
    double t0 = crtr_monotonic_ms();
    const crtr_graph *g = s->g;

    /* 1) 站名解析（电报码 / 站名） */
    char fcode[32], tcode[32], ferr[320];
    if (!crtr_station_resolve(s->stations, from_q, fcode, sizeof fcode,
                              ferr, sizeof ferr)) {
        resp->http_status = 404;
        snprintf(resp->error, sizeof resp->error, "%s", ferr);
        snprintf(resp->hint, sizeof resp->hint,
                 "可填电报码(如 IZQ)或站名(如 广州南)");
        resp->has_hint = 1;
        resp->elapsed_ms = (long)(crtr_monotonic_ms() - t0);
        return 0;
    }
    if (!crtr_station_resolve(s->stations, to_q, tcode, sizeof tcode,
                              ferr, sizeof ferr)) {
        resp->http_status = 404;
        snprintf(resp->error, sizeof resp->error, "%s", ferr);
        snprintf(resp->hint, sizeof resp->hint,
                 "可填电报码(如 BXP)或站名(如 北京西)");
        resp->has_hint = 1;
        resp->elapsed_ms = (long)(crtr_monotonic_ms() - t0);
        return 0;
    }

    /* 2) 坐标（本地 stations 表；无坐标 -> 404） */
    int32_t si1 = crtr_station_index(g, fcode);
    if (si1 < 0) {
        resp->http_status = 404;
        snprintf(resp->error, sizeof resp->error, "车站 %s 暂无坐标", fcode);
        snprintf(resp->hint, sizeof resp->hint, "12306 未提供该站坐标");
        resp->has_hint = 1;
        resp->elapsed_ms = (long)(crtr_monotonic_ms() - t0);
        return 0;
    }
    int32_t si2 = crtr_station_index(g, tcode);
    if (si2 < 0) {
        resp->http_status = 404;
        snprintf(resp->error, sizeof resp->error, "车站 %s 暂无坐标", tcode);
        snprintf(resp->hint, sizeof resp->hint, "12306 未提供该站坐标");
        resp->has_hint = 1;
        resp->elapsed_ms = (long)(crtr_monotonic_ms() - t0);
        return 0;
    }
    fill_endpoint(g, si1, &resp->ep_from);
    fill_endpoint(g, si2, &resp->ep_to);

    /* 3) 吸附到主网最近节点 */
    double off1 = 0, off2 = 0;
    int32_t src = crtr_nearest_node(g, resp->ep_from.lat, resp->ep_from.lon, 1, &off1);
    int32_t dst = crtr_nearest_node(g, resp->ep_to.lat, resp->ep_to.lon, 1, &off2);
    if (src < 0 || dst < 0) {
        resp->http_status = 422;
        snprintf(resp->error, sizeof resp->error, "车站附近没有可用铁路网络");
        resp->elapsed_ms = (long)(crtr_monotonic_ms() - t0);
        return 0;
    }
    resp->ep_from.snap_offset_m = off1;
    resp->ep_to.snap_offset_m = off2;

    /* 4) 算路（km= 规划不走缓存） */
    if (has_km) {
        int32_t *path = NULL;
        int32_t npath = 0;
        int okk = 0;
        double len = crtr_plan_by_km(g, s->tp, src, dst, km, &path, &npath, &okk);
        if (len < 0.0 || path == NULL) {
            resp->http_status = 422;
            snprintf(resp->error, sizeof resp->error, "两站之间在铁路网络中不可达");
            snprintf(resp->ufrom, sizeof resp->ufrom, "%s",
                     resp->ep_from.name ? resp->ep_from.name : "");
            snprintf(resp->uto, sizeof resp->uto, "%s",
                     resp->ep_to.name ? resp->ep_to.name : "");
            resp->has_names = 1;
            resp->elapsed_ms = (long)(crtr_monotonic_ms() - t0);
            return 0;
        }
        resp->has_km = 1;
        resp->km_requested = crtr_round(km, 1);
        resp->km_actual = crtr_round(len / 1000.0, 2);
        resp->km_diff = crtr_round(len / 1000.0 - km, 2);
        resp->km_reached = okk;
        int rc = build_route_result(g, path, npath, len, fcode, tcode, &resp->rr);
        free(path);
        if (rc != 0) goto oom;
        resp->rr_owned = 1;
    } else {
        uint64_t key = ((uint64_t)(uint32_t)src << 32) | (uint32_t)(uint32_t)dst;
        cache_entry *e = cache_acquire(s, key);
        if (e) {
            resp->rr = e->rr;                    /* 零拷贝：持引用 */
            resp->rr_owned = 0;
            resp->held_entry = e;
        } else {
            crtr_astar *a = crtr_astar_tls_get(g);
            if (!a) goto oom;
            int32_t *path = NULL;
            int32_t npath = 0;
            double len = crtr_astar_run(g, a, src, dst, NULL, &path, &npath);
            if (len < 0.0 || path == NULL) {
                free(path);
                resp->http_status = 422;
                snprintf(resp->error, sizeof resp->error, "两站之间在铁路网络中不可达");
                snprintf(resp->ufrom, sizeof resp->ufrom, "%s",
                         resp->ep_from.name ? resp->ep_from.name : "");
                snprintf(resp->uto, sizeof resp->uto, "%s",
                         resp->ep_to.name ? resp->ep_to.name : "");
                resp->has_names = 1;
                resp->elapsed_ms = (long)(crtr_monotonic_ms() - t0);
                return 0;
            }
            crtr_route_result rr;
            if (build_route_result(g, path, npath, len, fcode, tcode, &rr) != 0) {
                free(path);
                goto oom;
            }
            free(path);
            e = cache_insert(s, key, &rr);       /* 数组所有权进缓存 */
            if (!e) {                            /* 插入失败：自己持有 */
                resp->rr = rr;
                resp->rr_owned = 1;
            } else {
                resp->rr = e->rr;
                resp->rr_owned = 0;
                resp->held_entry = e;
            }
        }
    }

    resp->ok = 1;
    resp->http_status = 200;
    resp->elapsed_ms = (long)(crtr_monotonic_ms() - t0);
    return 1;

oom:
    resp->ok = 0;
    resp->http_status = 500;
    snprintf(resp->error, sizeof resp->error, "内部错误: 内存分配失败");
    resp->elapsed_ms = (long)(crtr_monotonic_ms() - t0);
    return 0;
}

void crtr_route_response_free(crtr_route_response *resp) {
    if (resp->rr_owned) {
        free(resp->rr.coords);
        free(resp->rr.passed);
        free(resp->rr.ferries);
    } else if (resp->held_entry) {
        /* 由服务缓存管理生命周期；此处无服务指针，释放动作发生在
         * crtr_service_release_held()。 */
    }
    resp->held_entry = NULL;
    memset(resp, 0, sizeof *resp);
}

/* 释放 resp 持有的缓存引用（与 crtr_route_response_free 配对调用一次） */
void crtr_service_release_held(crtr_service *s, crtr_route_response *resp) {
    if (!resp->rr_owned && resp->held_entry)
        cache_release(s, (cache_entry *)resp->held_entry);
    resp->held_entry = NULL;
}

/* ================================================================ 响应渲染 */

static void render_endpoint(crtr_jsonw *jw, const char *key,
                            const crtr_endpoint *ep) {
    crtr_jw_key(jw, key);
    crtr_jw_begin_obj(jw);
    crtr_jw_key(jw, "code");   crtr_jw_str(jw, ep->code);
    crtr_jw_key(jw, "name");   crtr_jw_str(jw, ep->name);
    crtr_jw_key(jw, "lat");    crtr_jw_double(jw, ep->lat);
    crtr_jw_key(jw, "lon");    crtr_jw_double(jw, ep->lon);
    crtr_jw_key(jw, "city");   crtr_jw_str(jw, ep->city);
    crtr_jw_key(jw, "address");crtr_jw_str(jw, ep->addr);
    crtr_jw_key(jw, "snapOffsetM"); crtr_jw_double(jw, crtr_round(ep->snap_offset_m, 1));
    crtr_jw_key(jw, "metaSource");  crtr_jw_str(jw, "local");
    crtr_jw_end_obj(jw);
}

void crtr_route_response_json(const crtr_route_response *resp, crtr_jsonw *jw) {
    crtr_jw_reset(jw);

    if (!resp->ok) {
        crtr_jw_begin_obj(jw);
        crtr_jw_key(jw, "ok");    crtr_jw_bool(jw, 0);
        crtr_jw_key(jw, "code");  crtr_jw_int(jw, resp->http_status);
        crtr_jw_key(jw, "error"); crtr_jw_str(jw, resp->error);
        if (resp->has_hint) { crtr_jw_key(jw, "hint"); crtr_jw_str(jw, resp->hint); }
        if (resp->has_names) {
            crtr_jw_key(jw, "from"); crtr_jw_str(jw, resp->ufrom);
            crtr_jw_key(jw, "to");   crtr_jw_str(jw, resp->uto);
        }
        crtr_jw_end_obj(jw);
        return;
    }

    const crtr_route_result *rr = &resp->rr;

    /* 轮渡总里程：先对各段 round2 再求和再 round2（同 Python） */
    double fsum = 0.0;
    for (int i = 0; i < rr->n_ferries; i++) fsum += rr->ferries[i].km;
    double ferry_km = crtr_round(fsum, 2);

    crtr_jw_begin_obj(jw);
    crtr_jw_key(jw, "ok"); crtr_jw_bool(jw, 1);

    render_endpoint(jw, "from", &resp->ep_from);
    render_endpoint(jw, "to", &resp->ep_to);

    crtr_jw_key(jw, "distance");
    crtr_jw_begin_obj(jw);
    crtr_jw_key(jw, "meters");     crtr_jw_double(jw, crtr_round(rr->length, 0));
    crtr_jw_key(jw, "kilometers"); crtr_jw_double(jw, crtr_round(rr->length / 1000.0, 3));
    crtr_jw_end_obj(jw);

    crtr_jw_key(jw, "stationCount"); crtr_jw_int(jw, rr->n_passed);

    crtr_jw_key(jw, "kmPlan");
    if (resp->has_km) {
        crtr_jw_begin_obj(jw);
        crtr_jw_key(jw, "requestedKm"); crtr_jw_double(jw, resp->km_requested);
        crtr_jw_key(jw, "actualKm");    crtr_jw_double(jw, resp->km_actual);
        crtr_jw_key(jw, "diffKm");      crtr_jw_double(jw, resp->km_diff);
        crtr_jw_key(jw, "reached");     crtr_jw_bool(jw, resp->km_reached);
        crtr_jw_end_obj(jw);
    } else {
        crtr_jw_null(jw);
    }

    crtr_jw_key(jw, "ferry");
    crtr_jw_begin_obj(jw);
    crtr_jw_key(jw, "hasFerry"); crtr_jw_bool(jw, rr->n_ferries > 0);
    crtr_jw_key(jw, "km");       crtr_jw_double(jw, ferry_km);
    crtr_jw_key(jw, "segments");
    crtr_jw_begin_arr(jw);
    for (int i = 0; i < rr->n_ferries; i++) {
        const crtr_ferry_seg *fs = &rr->ferries[i];
        crtr_jw_begin_obj(jw);
        crtr_jw_key(jw, "from");
        crtr_jw_begin_arr(jw);
        crtr_jw_double(jw, fs->from.lon);
        crtr_jw_double(jw, fs->from.lat);
        crtr_jw_end_arr(jw);
        crtr_jw_key(jw, "to");
        crtr_jw_begin_arr(jw);
        crtr_jw_double(jw, fs->to.lon);
        crtr_jw_double(jw, fs->to.lat);
        crtr_jw_end_arr(jw);
        crtr_jw_key(jw, "km"); crtr_jw_double(jw, fs->km);
        crtr_jw_end_obj(jw);
    }
    crtr_jw_end_arr(jw);
    crtr_jw_end_obj(jw);

    crtr_jw_key(jw, "stations");
    crtr_jw_begin_arr(jw);
    for (int i = 0; i < rr->n_passed; i++) {
        const crtr_passed_station *ps = &rr->passed[i];
        crtr_jw_begin_obj(jw);
        crtr_jw_key(jw, "code"); crtr_jw_str(jw, ps->code);
        crtr_jw_key(jw, "name"); crtr_jw_str(jw, ps->name);
        crtr_jw_key(jw, "lat");  crtr_jw_double(jw, ps->lat);
        crtr_jw_key(jw, "lon");  crtr_jw_double(jw, ps->lon);
        crtr_jw_key(jw, "km");   crtr_jw_double(jw, ps->km);
        crtr_jw_key(jw, "offsetM"); crtr_jw_double(jw, ps->offset_m);
        crtr_jw_end_obj(jw);
    }
    crtr_jw_end_arr(jw);

    crtr_jw_key(jw, "warnings");
    crtr_jw_begin_arr(jw);
    {
        char wbuf[512];
        const char *labels[2] = { "起点", "终点" };
        const crtr_endpoint *eps[2] = { &resp->ep_from, &resp->ep_to };
        for (int i = 0; i < 2; i++) {
            if (eps[i]->snap_offset_m > 2000.0) {
                snprintf(wbuf, sizeof wbuf,
                         "%s%s距最近铁路网 %.1f km，该段线路数据可能不完整，里程为近似值",
                         labels[i], eps[i]->name ? eps[i]->name : "",
                         eps[i]->snap_offset_m / 1000.0);
                crtr_jw_str(jw, wbuf);
            }
        }
        if (rr->n_ferries > 0) {
            snprintf(wbuf, sizeof wbuf,
                     "线路含跨海轮渡段 %.1f km（按直线距离计），需乘船过海", ferry_km);
            crtr_jw_str(jw, wbuf);
        }
    }
    crtr_jw_end_arr(jw);

    crtr_jw_key(jw, "geometry");
    crtr_jw_begin_obj(jw);
    crtr_jw_key(jw, "type"); crtr_jw_str(jw, "LineString");
    crtr_jw_key(jw, "coordinates");
    crtr_jw_begin_arr(jw);
    for (int i = 0; i < rr->n_coords; i++) {
        crtr_jw_begin_arr(jw);
        crtr_jw_double(jw, rr->coords[i].lon);
        crtr_jw_double(jw, rr->coords[i].lat);
        crtr_jw_end_arr(jw);
    }
    crtr_jw_end_arr(jw);
    crtr_jw_end_obj(jw);

    crtr_jw_key(jw, "nodeCount");   crtr_jw_int(jw, rr->n_nodes);
    crtr_jw_key(jw, "elapsedMs");   crtr_jw_int(jw, resp->elapsed_ms);
    crtr_jw_end_obj(jw);
}

/* ================================================================ HTTP 分发 */

static void send_jw(crtr_conn *c, int status, crtr_jsonw *jw) {
    const char *body = crtr_jw_finish(jw);
    crtr_http_send_json(c, status, body, jw->len);
}

static void handle_info(crtr_service *s, crtr_conn *c) {
    crtr_jsonw jw;
    crtr_jw_init(&jw);
    crtr_jw_begin_obj(&jw);
    crtr_jw_key(&jw, "ok"); crtr_jw_bool(&jw, 1);
    crtr_jw_key(&jw, "service"); crtr_jw_str(&jw, "中国铁路算路 API");
    crtr_jw_key(&jw, "version"); crtr_jw_str(&jw, "v3");
    crtr_jw_key(&jw, "pages");
    crtr_jw_begin_obj(&jw);
    char url[128];
    snprintf(url, sizeof url, "http://127.0.0.1:%d/", s->port);
    crtr_jw_key(&jw, "index"); crtr_jw_str(&jw, url);
    crtr_jw_end_obj(&jw);
    crtr_jw_key(&jw, "endpoints");
    crtr_jw_begin_obj(&jw);
    crtr_jw_key(&jw, "stationRoute");
    crtr_jw_str(&jw, "/api/v3/stationRoute?from=IZQ&to=BXP");
    crtr_jw_key(&jw, "searchStation");
    crtr_jw_str(&jw, "/api/v3/searchStation?q=广州");
    crtr_jw_end_obj(&jw);
    crtr_jw_end_obj(&jw);
    send_jw(c, 200, &jw);
    crtr_jw_free(&jw);
}

static void handle_search(crtr_service *s, crtr_conn *c, crtr_http_req *req) {
    char q[256], lim_raw[32];
    crtr_http_query_get(req, "q", q, sizeof q);
    crtr_http_query_get(req, "limit", lim_raw, sizeof lim_raw);

    int limit = 10;
    if (lim_raw[0]) {
        char *end = NULL;
        long v = strtol(lim_raw, &end, 10);
        if (end && *end == '\0') limit = (int)v;   /* 非法值回默认 10 */
    }

    crtr_station_row rows[50];
    int n = crtr_station_search(s->stations, q, limit, rows, 50);

    crtr_jsonw jw;
    crtr_jw_init(&jw);
    crtr_jw_begin_obj(&jw);
    crtr_jw_key(&jw, "ok"); crtr_jw_bool(&jw, 1);
    crtr_jw_key(&jw, "stations");
    crtr_jw_begin_arr(&jw);
    for (int i = 0; i < n; i++) {
        crtr_jw_begin_obj(&jw);
        crtr_jw_key(&jw, "code"); crtr_jw_str(&jw, rows[i].code);
        crtr_jw_key(&jw, "name"); crtr_jw_str(&jw, rows[i].name);
        crtr_jw_end_obj(&jw);
    }
    crtr_jw_end_arr(&jw);
    crtr_jw_end_obj(&jw);
    send_jw(c, 200, &jw);
    crtr_jw_free(&jw);
}

static void handle_route(crtr_service *s, crtr_conn *c, crtr_http_req *req) {
    char from_q[256], to_q[256], km_raw[64];
    int has_from = crtr_http_query_get(req, "from", from_q, sizeof from_q);
    int has_to = crtr_http_query_get(req, "to", to_q, sizeof to_q);

    if (!has_from || !has_to) {
        crtr_jsonw jw;
        crtr_jw_init(&jw);
        crtr_jw_begin_obj(&jw);
        crtr_jw_key(&jw, "ok"); crtr_jw_bool(&jw, 0);
        crtr_jw_key(&jw, "error"); crtr_jw_str(&jw, "缺少参数");
        crtr_jw_key(&jw, "usage");
        crtr_jw_str(&jw, "/api/v3/stationRoute?from=IZQ&to=BXP");
        crtr_jw_end_obj(&jw);
        send_jw(c, 400, &jw);
        crtr_jw_free(&jw);
        return;
    }

    int has_km = 0;
    double km = 0.0;
    if (crtr_http_query_get(req, "km", km_raw, sizeof km_raw)) {
        char *end = NULL;
        double v = strtod(km_raw, &end);
        while (end && (*end == ' ' || *end == '\t')) end++;
        if (end == km_raw || (end && *end != '\0') || !(v > 0.0)) {
            crtr_jsonw jw;
            crtr_jw_init(&jw);
            crtr_jw_begin_obj(&jw);
            crtr_jw_key(&jw, "ok"); crtr_jw_bool(&jw, 0);
            crtr_jw_key(&jw, "error");
            crtr_jw_str(&jw, "km 参数应为正数（单位：公里）");
            crtr_jw_key(&jw, "got"); crtr_jw_str(&jw, km_raw);
            crtr_jw_end_obj(&jw);
            send_jw(c, 400, &jw);
            crtr_jw_free(&jw);
            return;
        }
        has_km = 1;
        km = v;
    }

    crtr_route_response resp;
    crtr_service_route(s, from_q, to_q, has_km, km, &resp);

    crtr_jsonw jw;
    crtr_jw_init(&jw);
    crtr_route_response_json(&resp, &jw);
    send_jw(c, resp.http_status, &jw);
    crtr_jw_free(&jw);

    crtr_service_release_held(s, &resp);
    crtr_route_response_free(&resp);
}

void crtr_service_handle(void *ctx, crtr_conn *c, crtr_http_req *req) {
    crtr_service *s = (crtr_service *)ctx;

    /* 规范化路径：去尾部斜杠（保留根） */
    char path[CRTR_HTTP_MAX_PATH];
    snprintf(path, sizeof path, "%s", req->path);
    size_t pl = strlen(path);
    while (pl > 1 && path[pl - 1] == '/') path[--pl] = '\0';
    if (pl == 0) { path[0] = '/'; path[1] = '\0'; }

    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        crtr_http_send_static(c, s->webroot, "index.html", "text/html; charset=utf-8");
        return;
    }
    if (strcmp(path, "/config.js") == 0) {
        crtr_http_send_static(c, s->webroot, "config.js",
                              "application/javascript; charset=utf-8");
        return;
    }
    if (strcmp(path, "/api/v3") == 0 || strcmp(path, "/health") == 0 ||
        strcmp(path, "/api/v3/health") == 0) {
        handle_info(s, c);
        return;
    }
    if (strcmp(path, "/api/v3/searchStation") == 0) {
        handle_search(s, c, req);
        return;
    }
    if (strcmp(path, "/api/v3/stationRoute") == 0) {
        handle_route(s, c, req);
        return;
    }

    crtr_jsonw jw;
    crtr_jw_init(&jw);
    crtr_jw_begin_obj(&jw);
    crtr_jw_key(&jw, "ok"); crtr_jw_bool(&jw, 0);
    crtr_jw_key(&jw, "error"); crtr_jw_str(&jw, "接口不存在");
    crtr_jw_key(&jw, "path"); crtr_jw_str(&jw, path);
    crtr_jw_end_obj(&jw);
    send_jw(c, 404, &jw);
    crtr_jw_free(&jw);
}
