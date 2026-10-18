#ifndef CRTR_JSONW_H
#define CRTR_JSONW_H

#include <stddef.h>

/*
 * JSON 输出器（PLAN §6）
 *
 * - 输出 UTF-8，ensure_ascii=False 语义：非 ASCII 原样写出。
 * - 缩进 2 空格，字段顺序由调用方决定（对齐 server.py 的 dict 顺序）。
 * - 浮点用「最短可往返」表示并对齐 Python repr：整值浮点补 ".0"。
 */

typedef struct {
    char  *buf;
    size_t len, cap;
    int    depth;                        /* 当前容器嵌套深度 */
    unsigned char sep[32];               /* 每层分隔状态，见 .c */
} crtr_jsonw;

void crtr_jw_init(crtr_jsonw *w);
void crtr_jw_free(crtr_jsonw *w);
void crtr_jw_reset(crtr_jsonw *w);       /* 复用缓冲区 */
const char *crtr_jw_finish(crtr_jsonw *w); /* NUL 结尾并返回（指针归 w 所有） */

/* 容器 */
void crtr_jw_begin_obj(crtr_jsonw *w);
void crtr_jw_end_obj(crtr_jsonw *w);
void crtr_jw_begin_arr(crtr_jsonw *w);
void crtr_jw_end_arr(crtr_jsonw *w);

/* 字段名（之后紧跟一个值） */
void crtr_jw_key(crtr_jsonw *w, const char *name);

/* 值 */
void crtr_jw_str(crtr_jsonw *w, const char *utf8);  /* NULL 输出 null */
void crtr_jw_double(crtr_jsonw *w, double v);
void crtr_jw_int(crtr_jsonw *w, long long v);
void crtr_jw_bool(crtr_jsonw *w, int v);
void crtr_jw_null(crtr_jsonw *w);

/* Python round(x, digits) 语义（最近偶舍入，二进制值上的实现） */
double crtr_round(double v, int digits);

#endif
