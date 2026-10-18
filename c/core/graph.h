#ifndef CRTR_GRAPH_H
#define CRTR_GRAPH_H

#include <stdint.h>
#include <stddef.h>
#include <math.h>

/*
 * 铁路骨架图。内存布局见 c/PLAN.md §4（data/rail_graph.bin v1）。
 * 加载后所有数组指向一次分配的内存块；字符串以 NUL 结尾。
 */
typedef struct {
    int32_t n_nodes;          /* N */
    double *lat, *lon;        /* [N] */
    int32_t *comp;            /* [N] 连通分量 id */
    int32_t big_comp;

    int32_t n_edges;          /* E = 有向边槽数 */
    int32_t *head;            /* [N+1] CSR 偏移 */
    int32_t *eto;             /* [E] 目标节点 */
    double  *ew;              /* [E] 边权（米） */
    int32_t *egeom;           /* [E] 几何表下标，-1 = 直线 */

    int32_t n_geom;           /* G */
    int64_t *goff;            /* [G+1] 点偏移 */
    double  *gpts;            /* 紧凑点列，lat/lon 交错 */

    int32_t n_stations;       /* S */
    char  **st_code;
    int32_t *st_node;         /* 预吸附节点（构建期），可能为 -1 */
    double  *st_snap;         /* 吸附距离（米） */
    double  *st_lat, *st_lon;
    char  **st_name, **st_city, **st_addr;

    int32_t n_names;          /* K：电报码 -> 站名 */
    char  **nm_code, **nm_name;

    int32_t n_lanes;          /* L：同址异名标注 */
    char  **ln_code, **ln_name, **ln_type, **ln_alias, **ln_note;

    int32_t n_ferry;          /* F */
    int64_t (*ferry)[4];      /* 每行 [from_lat,from_lon,to_lat,to_lon] 微度 */

    /* 内部索引（加载时构建，free 时释放） */
    void *grid;               /* 0.05 座节点网格 */
    void *st_hash;            /* code -> 车站下标 */
    void *nm_hash;            /* code -> 站名下标 */
    void *ln_hash;            /* code -> 标注下标 */
    void *_arena;             /* 底层内存块 */
} crtr_graph;

/* 加载失败返回非 0 并把原因写入 err。 */
int    crtr_graph_load(crtr_graph *g, const char *path, char *err, size_t errlen);
void   crtr_graph_free(crtr_graph *g);

/* 最近节点。main_only=1 时只考虑 big_comp（语义同 server.py nearest_node，
 * 含远距离回退）。返回节点下标或 -1，米距离写入 *out_dist（可 NULL）。 */
int32_t crtr_nearest_node(const crtr_graph *g, double lat, double lon,
                          int main_only, double *out_dist);

/* 找有向边槽 a->b，无则 -1。 */
int32_t crtr_find_edge(const crtr_graph *g, int32_t a, int32_t b);

/* 路径实际长度（米），相邻点查不到边按 haversine 兜底（同 path_length）。 */
double  crtr_path_length(const crtr_graph *g, const int32_t *path, int32_t len);

/* 车站/站名/标注查询 */
int32_t     crtr_station_index(const crtr_graph *g, const char *code);
const char *crtr_name_of(const crtr_graph *g, const char *code);   /* NULL 若无 */
const char *crtr_lane_alias(const crtr_graph *g, const char *code);/* aliasOf 或 NULL */

/* 微度换算（轮渡判定用）：round(x*1e6)，与 Python round(x, 6) 对齐 */
static inline int64_t crtr_to_u6(double x) { return llround(x * 1e6); }

#endif
