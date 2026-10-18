#include "jsonw.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- 缓冲区 */

static void jw_grow(crtr_jsonw *w, size_t need) {
    if (w->len + need + 1 <= w->cap) return;
    size_t cap = w->cap ? w->cap : 256;
    while (cap < w->len + need + 1) cap *= 2;
    char *nb = realloc(w->buf, cap);
    if (!nb) abort();              /* 输出缓冲 OOM 无力恢复，直接终止 */
    w->buf = nb;
    w->cap = cap;
}

static void jw_puts(crtr_jsonw *w, const char *s, size_t n) {
    jw_grow(w, n);
    memcpy(w->buf + w->len, s, n);
    w->len += n;
}

static void jw_putc(crtr_jsonw *w, char c) { jw_grow(w, 1); w->buf[w->len++] = c; }

/* ---------------------------------------------------------------- 缩进与分隔
 *
 * sep[d] 含义（d = 当前容器深度）：
 *   0 = 容器刚打开，下一个成员前只要换行缩进；
 *   1 = 已有成员，下一个成员前要 ",\n" + 缩进；
 *   2 = 刚写了 key，接下来的值跟在同一行（一个空格）。
 * 顶层（depth==0）标量不加任何空白。
 */

static void jw_indent(crtr_jsonw *w, int d) {
    for (int i = 0; i < d * 2; i++) jw_putc(w, ' ');
}

static void jw_item_sep(crtr_jsonw *w) {
    if (w->depth == 0) return;
    unsigned char s = w->sep[w->depth];
    if (s == 1) { jw_putc(w, ','); jw_putc(w, '\n'); jw_indent(w, w->depth); }
    else if (s == 0) { jw_putc(w, '\n'); jw_indent(w, w->depth); }
    else { jw_putc(w, ' '); }
}

void crtr_jw_begin_obj(crtr_jsonw *w) {
    jw_item_sep(w);
    if (w->depth > 0) w->sep[w->depth] = 1;   /* 父容器的这个成员已落笔 */
    jw_putc(w, '{');
    if (w->depth < (int)(sizeof(w->sep) - 1)) w->sep[++w->depth] = 0;
    else abort();
}

void crtr_jw_begin_arr(crtr_jsonw *w) {
    jw_item_sep(w);
    if (w->depth > 0) w->sep[w->depth] = 1;
    jw_putc(w, '[');
    if (w->depth < (int)(sizeof(w->sep) - 1)) w->sep[++w->depth] = 0;
    else abort();
}

static void jw_end(crtr_jsonw *w, char close) {
    if (w->depth == 0) return;
    int had_items = (w->sep[w->depth] != 0);
    w->depth--;
    if (had_items) { jw_putc(w, '\n'); jw_indent(w, w->depth); }
    jw_putc(w, close);
}

void crtr_jw_end_obj(crtr_jsonw *w) { jw_end(w, '}'); }
void crtr_jw_end_arr(crtr_jsonw *w) { jw_end(w, ']'); }

void crtr_jw_key(crtr_jsonw *w, const char *name) {
    jw_item_sep(w);
    jw_putc(w, '"');
    jw_puts(w, name, strlen(name));
    jw_puts(w, "\": ", 2);
    w->sep[w->depth] = 2;
}

/* ---------------------------------------------------------------- 字符串转义
 *
 * 与 json.dumps(ensure_ascii=False) 一致：只转义 " 、\ 与控制字符；
 * 控制字符用 \uXXXX（\b \f \n \r \t 用短形式），其余字节原样。
 */

static void jw_hex4(crtr_jsonw *w, unsigned v) {
    static const char hex[] = "0123456789abcdef";
    char tmp[6] = { '\\', 'u', hex[(v >> 12) & 15], hex[(v >> 8) & 15],
                    hex[(v >> 4) & 15], hex[v & 15] };
    jw_puts(w, tmp, 6);
}

void crtr_jw_str(crtr_jsonw *w, const char *utf8) {
    if (!utf8) { crtr_jw_null(w); return; }
    jw_item_sep(w);
    jw_putc(w, '"');
    const unsigned char *p = (const unsigned char *)utf8;
    while (*p) {
        unsigned char c = *p;
        if (c == '"')  { jw_puts(w, "\\\"", 2); p++; }
        else if (c == '\\') { jw_puts(w, "\\\\", 2); p++; }
        else if (c == '\b') { jw_puts(w, "\\b", 2); p++; }
        else if (c == '\f') { jw_puts(w, "\\f", 2); p++; }
        else if (c == '\n') { jw_puts(w, "\\n", 2); p++; }
        else if (c == '\r') { jw_puts(w, "\\r", 2); p++; }
        else if (c == '\t') { jw_puts(w, "\\t", 2); p++; }
        else if (c < 0x20)   { jw_hex4(w, c); p++; }
        else { jw_putc(w, (char)c); p++; }   /* UTF-8 多字节序列原样透传 */
    }
    jw_putc(w, '"');
    if (w->depth > 0) w->sep[w->depth] = 1;
}

/* ---------------------------------------------------------------- 数值
 *
 * crtr_round：与 Python round(x, n) 对齐 —— 对二进制值做十进制最近偶
 * 舍入。乘 10^n 后用 nearbyint（按默认舍入模式 = 最近偶），再除回。
 * .5 边界上可能与 Python 差 1ulp（PLAN §6 明确允许）。
 */

double crtr_round(double v, int digits) {
    static const double POW10[7] = { 1.0, 10.0, 100.0, 1000.0, 1e4, 1e5, 1e6 };
    if (digits <= 0) digits = 0;
    if (digits > 6)  digits = 6;
    double p = POW10[digits];
    return nearbyint(v * p) / p;
}

void crtr_jw_double(crtr_jsonw *w, double v) {
    jw_item_sep(w);
    if (!isfinite(v)) { jw_puts(w, "null", 4); }
    else {
        char tmp[40];
        /* 最短可往返表示：15..17 位有效数字里取第一个能还原的 */
        for (int p = 15; p <= 17; p++) {
            snprintf(tmp, sizeof tmp, "%.*g", p, v);
            if (strtod(tmp, NULL) == v) break;
        }
        const char *has = strpbrk(tmp, ".eE");
        jw_puts(w, tmp, strlen(tmp));
        /* Python 的 float repr 总带小数点：1 -> "1.0" */
        if (!has) jw_puts(w, ".0", 2);
    }
    if (w->depth > 0) w->sep[w->depth] = 1;
}

void crtr_jw_int(crtr_jsonw *w, long long v) {
    jw_item_sep(w);
    char tmp[24];
    int n = snprintf(tmp, sizeof tmp, "%lld", v);
    jw_puts(w, tmp, (size_t)n);
    if (w->depth > 0) w->sep[w->depth] = 1;
}

void crtr_jw_bool(crtr_jsonw *w, int v) {
    jw_item_sep(w);
    jw_puts(w, v ? "true" : "false", v ? 4 : 5);
    if (w->depth > 0) w->sep[w->depth] = 1;
}

void crtr_jw_null(crtr_jsonw *w) {
    jw_item_sep(w);
    jw_puts(w, "null", 4);
    if (w->depth > 0) w->sep[w->depth] = 1;
}

/* ---------------------------------------------------------------- 生命周期 */

void crtr_jw_init(crtr_jsonw *w) {
    w->buf = NULL; w->len = w->cap = 0; w->depth = 0;
    w->sep[0] = 0;
}

void crtr_jw_free(crtr_jsonw *w) { free(w->buf); crtr_jw_init(w); }

void crtr_jw_reset(crtr_jsonw *w) { w->len = 0; w->depth = 0; w->sep[0] = 0; }

const char *crtr_jw_finish(crtr_jsonw *w) {
    jw_grow(w, 1);
    w->buf[w->len] = '\0';
    return w->buf;
}
