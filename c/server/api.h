#ifndef CRTR_API_H
#define CRTR_API_H

#include "threadpool.h"
#include "jsonw.h"
#include "http.h"
#include "../core/graph.h"

/*
 * 服务层（PLAN §5-§7）：端点分发、算路编排、路由缓存、响应渲染。
 * metaSource 固定 "local"（PLAN §2：不做 12306 实时请求）。
 */

typedef struct crtr_service crtr_service;

crtr_service *crtr_service_create(const crtr_graph *g, crtr_threadpool *tp,
                                  const char *webroot, int port);
void crtr_service_destroy(crtr_service *s);

/* HTTP 入口（注册给 crtr_http_serve 的 handler，ctx = crtr_service*） */
void crtr_service_handle(void *ctx, crtr_conn *conn, crtr_http_req *req);

/* ---- 供 crtr_query CLI 复用的算路接口 ---- */

typedef struct {
    const char *code, *name;    /* 站名（指向图内存或 resp 内缓冲） */
    double lat, lon;
    const char *city, *addr;    /* NULL 输出为 JSON null */
    double snap_offset_m;
} crtr_endpoint;

typedef struct {
    double lat, lon;            /* 已 round6 */
} crtr_coord;

typedef struct {
    const char *code, *name;
    double lat, lon;
    double km;                  /* 沿线里程（公里，round2） */
    double offset_m;            /* 到线路垂距（米，round0） */
} crtr_passed_station;

typedef struct {
    crtr_coord from, to;
    double km;                  /* round2 */
} crtr_ferry_seg;

typedef struct {
    double length;              /* 总里程（米） */
    crtr_coord *coords;
    int n_coords;
    crtr_passed_station *passed;
    int n_passed;
    int n_nodes;
    crtr_ferry_seg *ferries;
    int n_ferries;
} crtr_route_result;

typedef struct {
    int ok;                     /* 业务是否成功 */
    int http_status;            /* 200 / 404 / 422 / 500 */
    char error[320];            /* 失败原因（业务失败时非空） */
    char hint[160];             /* 部分失败形态带 hint */
    int has_hint;
    char ufrom[128], uto[128];  /* 不可达形态附带的站名 */
    int has_names;
    crtr_endpoint ep_from, ep_to;
    int has_km;
    double km_requested, km_actual, km_diff;
    int km_reached;
    crtr_route_result rr;       /* ok 时有效 */
    int rr_owned;               /* 1 = resp 拥有 rr 的数组；0 = 指向缓存 */
    void *held_entry;           /* rr_owned==0 时持有的缓存条目 */
    long elapsed_ms;
} crtr_route_response;

/* 成功返回 1（resp->ok 置位）；业务/内部失败返回 0（error 已填）。 */
int crtr_service_route(crtr_service *s, const char *from_q, const char *to_q,
                       int has_km, double km, crtr_route_response *resp);

/* 渲染为与 server.py 逐字段对齐的 JSON（写入 jw）。 */
void crtr_route_response_json(const crtr_route_response *resp, crtr_jsonw *jw);

/* 释放 resp 持有的缓存引用（rr_owned==0 时必须先于 free 调用一次）。 */
void crtr_service_release_held(crtr_service *s, crtr_route_response *resp);

void crtr_route_response_free(crtr_route_response *resp);

#endif
