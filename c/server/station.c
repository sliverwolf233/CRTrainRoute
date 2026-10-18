#include "station.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 「站」的 UTF-8 编码 */
static const char ZHAN[4] = { (char)0xE7, (char)0xAB, (char)0x99, 0 };

/* ---------------------------------------------------------------- 反向表
 *
 * server.py：rev.setdefault(key, code)，key ∈ {name, name+"站"}。
 * 先插入者胜出，遍历顺序 = station_names.json 的文件顺序 = nm_* 数组顺序。
 * 开放寻址字符串哈希表。
 */

typedef struct {
    const char *key;
    const char *val;
} s_entry;

struct crtr_stationdb {
    const crtr_graph *g;
    s_entry *tab;
    size_t   cap;      /* 2 的幂 */
    size_t   n;
};

static size_t s_hash(const char *s) {
    size_t h = 1469598103934665603ULL;      /* FNV-1a */
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h;
}

static s_entry *s_find(crtr_stationdb *db, const char *key) {
    if (db->cap == 0) return NULL;
    size_t i = s_hash(key) & (db->cap - 1);
    for (;;) {
        s_entry *e = &db->tab[i];
        if (e->key == NULL) return NULL;
        if (strcmp(e->key, key) == 0) return e;
        i = (i + 1) & (db->cap - 1);
    }
}

static void s_insert(crtr_stationdb *db, const char *key, const char *val) {
    if (db->n + 1 > db->cap * 3 / 4) {
        size_t ncap = db->cap ? db->cap * 2 : 512;
        s_entry *nt = calloc(ncap, sizeof *nt);
        if (!nt) return;                     /* 扩容失败：丢弃该键（容忍） */
        for (size_t i = 0; i < db->cap; i++) {
            if (db->tab[i].key) {
                size_t j = s_hash(db->tab[i].key) & (ncap - 1);
                while (nt[j].key) j = (j + 1) & (ncap - 1);
                nt[j] = db->tab[i];
            }
        }
        free(db->tab);
        db->tab = nt;
        db->cap = ncap;
    }
    if (s_find(db, key)) return;             /* setdefault：已有则不动 */
    size_t i = s_hash(key) & (db->cap - 1);
    while (db->tab[i].key) i = (i + 1) & (db->cap - 1);
    db->tab[i].key = key;
    db->tab[i].val = val;
    db->n++;
}

crtr_stationdb *crtr_stationdb_new(const crtr_graph *g) {
    crtr_stationdb *db = calloc(1, sizeof *db);
    if (!db) return NULL;
    db->g = g;
    for (int32_t i = 0; i < g->n_names; i++) {
        if (!g->nm_code[i] || !g->nm_name[i]) continue;
        char buf[256];
        s_insert(db, g->nm_name[i], g->nm_code[i]);
        if (strlen(g->nm_name[i]) + 4 < sizeof buf) {
            snprintf(buf, sizeof buf, "%s%s", g->nm_name[i], ZHAN);
            s_insert(db, buf, g->nm_code[i]);   /* name+"站" */
        }
    }
    return db;
}

void crtr_stationdb_free(crtr_stationdb *db) {
    if (!db) return;
    free(db->tab);
    free(db);
}

/* ---------------------------------------------------------------- 小工具 */

/* 去除首尾空白（ASCII 空白 + 全角空格 U+3000），Python str.strip() 的常用子集 */
static char *xstrstrip(const char *s) {
    while (*s) {
        unsigned char c = (unsigned char)*s;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
            c == '\v' ||
            (c == 0xE3 && (unsigned char)s[1] == 0x80 && (unsigned char)s[2] == 0x80))
            s += (c == 0xE3) ? 3 : 1;
        else break;
    }
    size_t n = strlen(s);
    while (n > 0) {
        unsigned char c = (unsigned char)s[n - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
            c == '\v' ||
            (n >= 3 && c == 0x80 && (unsigned char)s[n - 2] == 0x80 &&
             (unsigned char)s[n - 3] == 0xE3))
            n--;
        else break;
    }
    char *out = malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static int ends_with_zhan(const char *s, size_t n) {
    return n >= 3 && (unsigned char)s[n - 3] == 0xE7 &&
           (unsigned char)s[n - 2] == 0xAB && (unsigned char)s[n - 1] == 0x99;
}

/* 全 ASCII 字母且非空（Python: up.isalpha() and up.isascii()） */
static int all_ascii_alpha(const char *s) {
    if (!*s) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (!isalpha(*p)) return 0;
    return 1;
}

int crtr_station_resolve(crtr_stationdb *db, const char *q,
                         char *code_buf, size_t code_sz,
                         char *err_buf, size_t err_sz) {
    code_buf[0] = '\0';
    err_buf[0] = '\0';

    char *qs = xstrstrip(q ? q : "");
    if (!qs) { snprintf(err_buf, err_sz, "参数为空"); return 0; }
    if (!*qs) { free(qs); snprintf(err_buf, err_sz, "参数为空"); return 0; }

    /* 1) 大写后直接命中电报码 */
    char up[32];
    size_t un = 0;
    for (const unsigned char *p = (const unsigned char *)qs; *p && un < sizeof up - 1; p++)
        up[un++] = (char)toupper(*p);
    up[un] = '\0';
    if (crtr_name_of(db->g, up)) {
        snprintf(code_buf, code_sz, "%s", up);
        free(qs);
        return 1;
    }
    /* 电报码形态（2-4 位 ASCII 字母）但不在表中 */
    if (un >= 2 && un <= 4 && all_ascii_alpha(up)) {
        snprintf(err_buf, err_sz, "电报码 %s 不存在", up);
        free(qs);
        return 0;
    }

    /* 2) 中文站名精确匹配：q、q+"站"；再去尾「站」再试 */
    char key[320];
    s_entry *e = NULL;
    if (strlen(qs) < sizeof key && (e = s_find(db, qs)) != NULL) {
        snprintf(code_buf, code_sz, "%s", e->val);
        free(qs);
        return 1;
    }
    size_t ql = strlen(qs);
    if (ql + 4 < sizeof key) {
        snprintf(key, sizeof key, "%s%s", qs, ZHAN);
        if ((e = s_find(db, key)) != NULL) {
            snprintf(code_buf, code_sz, "%s", e->val);
            free(qs);
            return 1;
        }
    }
    if (ends_with_zhan(qs, ql)) {
        qs[ql - 3] = '\0';                       /* 去掉尾「站」 */
        e = s_find(db, qs);
        qs[ql - 3] = (char)0xE7;                  /* 还原，供包含匹配用 */
        qs[ql - 2] = (char)0xAB;
        qs[ql - 1] = (char)0x99;
        if (e) {
            snprintf(code_buf, code_sz, "%s", e->val);
            free(qs);
            return 1;
        }
    }

    /* 3) 唯一包含匹配；多命中给前 8 个候选 */
    const crtr_graph *g = db->g;
    const char *hit_code[8];
    const char *hit_name[8];
    int nhit = 0;
    for (int32_t i = 0; i < g->n_names && nhit < 8; i++) {
        if (g->nm_name[i] && strstr(g->nm_name[i], qs)) {
            hit_code[nhit] = g->nm_code[i];
            hit_name[nhit] = g->nm_name[i];
            nhit++;
        }
    }
    if (nhit == 1) {
        snprintf(code_buf, code_sz, "%s", hit_code[0]);
        free(qs);
        return 1;
    }
    if (nhit >= 1) {
        int off = snprintf(err_buf, err_sz, "站名「%s」匹配到多个车站：", qs);
        for (int i = 0; i < nhit && off > 0 && (size_t)off < err_sz; i++) {
            off += snprintf(err_buf + off, err_sz - (size_t)off, "%s%s(%s)",
                            i ? "、" : "", hit_name[i], hit_code[i]);
        }
    } else {
        snprintf(err_buf, err_sz, "未找到车站「%s」", qs);
    }
    free(qs);
    return 0;
}

/* ---------------------------------------------------------------- searchStation
 *
 * 三级排序：精确(name == q 或 name+"站" == q) -> 前缀 -> 包含。
 * 每级内部保持 station_names.json 文件顺序；拼接后截取
 * max(1, min(limit, 50)) 条（同 server.py）。
 */

int crtr_station_search(crtr_stationdb *db, const char *q, int limit,
                        crtr_station_row *rows, int max_rows) {
    int nlimit = limit;
    if (nlimit > 50) nlimit = 50;
    if (nlimit < 1) nlimit = 1;
    if (nlimit > max_rows) nlimit = max_rows;

    char *qs = xstrstrip(q ? q : "");
    if (!qs || !*qs) { free(qs); return 0; }

    const crtr_graph *g = db->g;
    crtr_station_row exact[50], prefix[50], fuzzy[50];
    int ne = 0, npr = 0, nfu = 0;
    size_t qlen = strlen(qs);

    for (int32_t i = 0; i < g->n_names; i++) {
        const char *name = g->nm_name[i];
        if (!name || !g->nm_code[i]) continue;
        if (strcmp(name, qs) == 0) {
            if (ne < 50) exact[ne++] = (crtr_station_row){ g->nm_code[i], name };
            continue;
        }
        size_t nl = strlen(name);
        int tier;
        char with_zhan[256];
        int is_exact_zhan = 0;
        if (nl + 4 < sizeof with_zhan) {
            snprintf(with_zhan, sizeof with_zhan, "%s%s", name, ZHAN);
            is_exact_zhan = (strcmp(with_zhan, qs) == 0);
        }
        if (is_exact_zhan) tier = 0;
        else if (nl >= qlen && strncmp(name, qs, qlen) == 0) tier = 1;
        else if (strstr(name, qs) != NULL) tier = 2;
        else continue;

        if (tier == 0 && ne < 50)        exact[ne++]  = (crtr_station_row){ g->nm_code[i], name };
        else if (tier == 1 && npr < 50)  prefix[npr++] = (crtr_station_row){ g->nm_code[i], name };
        else if (tier == 2 && nfu < 50)  fuzzy[nfu++] = (crtr_station_row){ g->nm_code[i], name };
    }
    free(qs);

    int n = 0;
    for (int i = 0; i < ne  && n < nlimit; i++) rows[n++] = exact[i];
    for (int i = 0; i < npr && n < nlimit; i++) rows[n++] = prefix[i];
    for (int i = 0; i < nfu && n < nlimit; i++) rows[n++] = fuzzy[i];
    return n;
}
