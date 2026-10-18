#ifndef CRTR_ASTAR_H
#define CRTR_ASTAR_H

#include "graph.h"

/* 有向边过滤器：bits 为按边槽位图（调用方分配/清零/释放）。
 * ban=1  ：命中的边完全跳过（_shortest_avoiding 形态）
 * ban=0  ：命中的边 w *= factor（plan_by_km 惩罚形态）
 * bits=NULL 表示不过滤。 */
typedef struct {
    const uint64_t *bits;
    double factor;
    int    ban;
} crtr_edge_filter;

/* 每线程搜索上下文（dist/prev/done/堆），不线程安全，一线程一个。 */
typedef struct crtr_astar crtr_astar;

crtr_astar *crtr_astar_new(const crtr_graph *g);
void        crtr_astar_free(crtr_astar *a);

/* A* 搜索（启发式 = 到终点 haversine）。成功返回总米数并把路径写入
 * *out_path（malloc 的 int32 数组，调用方 free；out_path 可 NULL）与 *out_len；
 * 不可达返回 -1.0。src==dst 返回 0 与单点路径。 */
double crtr_astar_run(const crtr_graph *g, crtr_astar *a,
                      int32_t src, int32_t dst,
                      const crtr_edge_filter *filter,
                      int32_t **out_path, int32_t *out_len);

/* 位图辅助 */
uint64_t *crtr_bits_alloc(const crtr_graph *g);
void      crtr_bits_free(uint64_t *bits);
size_t    crtr_bits_bytes(const crtr_graph *g);
static inline void crtr_bits_set(uint64_t *bits, int32_t slot) {
    bits[(uint32_t)slot >> 6] |= (uint64_t)1 << (slot & 63);
}
static inline int  crtr_bits_get(const uint64_t *bits, int32_t slot) {
    return (int)((bits[(uint32_t)slot >> 6] >> (slot & 63)) & 1u);
}

#endif
