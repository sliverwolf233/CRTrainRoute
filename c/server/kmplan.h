#ifndef CRTR_KMPLAN_H
#define CRTR_KMPLAN_H

#include <stdint.h>
#include "../core/astar.h"
#include "threadpool.h"

/*
 * 指定里程规划（PLAN §6，对齐 server.py plan_by_km / _shortest_avoiding）
 *
 * 两阶段：
 *  1) 全局绕行：迭代对已选路径的全部有向边加 6.0 惩罚再求解（<=12 次）；
 *  2) 局部微调：对最接近目标的 <=3 个候选，把若干区间替换为「避开原边」
 *     的次短路；各 (i, i+seg) 替换搜索相互独立，投入线程池并行
 *     （PLAN §7），首个改进即采纳并重扫（对应 Python 的即时变异）。
 *
 * 结果只需满足 ±5 km / 5% 容差（tests 断言），不强求与 Python 逐位一致。
 */

/* 线程局部 A* 上下文（懒创建、线程存活期内复用；搜索过程零锁） */
crtr_astar *crtr_astar_tls_get(const crtr_graph *g);

/* 线程局部的边过滤位图（每次使用前必须 memset 清零） */
uint64_t *crtr_bits_tls(const crtr_graph *g);

/* 图上不可达返回 -1.0（*out_path 不动）；成功返回实际里程（米），
 * *out_path 为 malloc 的节点序列（调用方 free），*ok 写入是否达标。 */
double crtr_plan_by_km(const crtr_graph *g, crtr_threadpool *tp,
                       int32_t src, int32_t dst, double target_km,
                       int32_t **out_path, int32_t *out_len, int *ok);

#endif
