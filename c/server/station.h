#ifndef CRTR_STATION_H
#define CRTR_STATION_H

#include <stddef.h>
#include "../core/graph.h"

/*
 * 站名解析与联想（PLAN §6，对齐 server.py StationClient.resolve / searchStation）
 *
 * - resolve：电报码 -> 精确名(name、name+站) -> 去尾「站」 -> 唯一包含匹配，
 *   多命中时返回候选列表错误。
 * - searchStation：精确/前缀/包含三级排序，limit 默认 10、上限 50。
 */

typedef struct crtr_stationdb crtr_stationdb;

crtr_stationdb *crtr_stationdb_new(const crtr_graph *g);
void            crtr_stationdb_free(crtr_stationdb *db);

/* 解析用户输入。成功返回 1 并把大写电报码写入 code_buf；
 * 失败返回 0 并把错误说明写入 errbuf（语义同 server.py resolve）。 */
int crtr_station_resolve(crtr_stationdb *db, const char *q,
                         char *code_buf, size_t code_sz,
                         char *err_buf, size_t err_sz);

/* 站名联想：返回写入门 rows 的条数（<= limit_eff）。
 * limit 语义同 Python：max(1, min(limit, 50))。 */
typedef struct {
    const char *code;
    const char *name;
} crtr_station_row;

int crtr_station_search(crtr_stationdb *db, const char *q, int limit,
                        crtr_station_row *rows, int max_rows);

#endif
