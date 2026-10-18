/* 骨架图加载与查询 —— data/rail_graph.bin v1（格式见 c/PLAN.md §4）。
 *
 * 设计要点：
 * - 文件内各段偏移不保证 8 字节对齐，因此所有多字节值按小端手工装配，
 *   再拷入一次分配的 _arena（8 字节段在前、4 字节段居中、字符串居尾；
 *   malloc 返回地址天然满足最大对齐，前两段尺寸均为 8/4 的倍数，无填充）。
 * - 加载分两遍扫描文件：第一遍测量（几何点总数、字符串总字节数），
 *   第二遍填充 arena；两遍共用 wl_sections，保证布局一致。
 * - 内部索引：0.05 度节点网格（链式哈希桶，语义同 server.py grid）与
 *   三张电报码哈希表（开放寻址、预分配不扩容）。
 * - 加载失败时释放全部资源并保持 g 为零值，err 写入中文原因。
 */
#include "graph.h"
#include "geom.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- 内部类型 */

/* 0.05 度节点网格：链式哈希，cell_i/cell_j 记录每个节点所在桶坐标 */
typedef struct {
    int32_t cap;        /* 桶数（2 的幂，>=16） */
    int32_t *bucket;    /* [cap] 链头节点，-1 为空 */
    int32_t *next;      /* [N] 同桶下一节点 */
    int32_t *cell_i;    /* [N] 纬度桶坐标 */
    int32_t *cell_j;    /* [N] 经度桶坐标 */
} crtr_grid;

/* 电报码 -> 下标 开放寻址哈希（key 指向 arena 内字符串） */
typedef struct {
    int32_t cap;        /* 桶数（2 的幂，0 表示空表） */
    int32_t mask;
    char **key;         /* [cap] 空槽为 NULL */
    int32_t *val;       /* [cap] */
} crtr_map;

/* 各计数的合理上限（防伪造头部导致尺寸溢出；真实图 N 约 20 万） */
#define CRTR_CNT_MAX 200000000

/* 网格边长（度），同 server.py self.cell */
static const double CRTR_CELL = 0.05;

/* ---------------------------------------------------------------- 小端读取 */

typedef struct {
    const unsigned char *p;    /* 整个文件缓冲 */
    size_t len;                /* 文件字节数 */
    size_t off;                /* 当前游标 */
    int fill;                  /* 0=测量 1=填充 */
    crtr_graph *g;             /* 填充目标（字段已指向 arena） */
    /* 头部计数 */
    size_t n, e, gg, s, kk, l, f;
    /* 测量累加 */
    size_t total_pts;          /* 几何点总数 P */
    size_t str_bytes;          /* 字符串总字节数 T */
    /* 填充游标 */
    char *strcur;              /* 字符串区写指针 */
    /* 错误状态 */
    int bad;
    char *err;
    size_t errlen;
} wl;

static void wl_fail(wl *w, const char *msg)
{
    if (!w->bad) {
        w->bad = 1;
        if (w->err && w->errlen)
            snprintf(w->err, w->errlen, "%s", msg);
    }
}

static int wl_need(wl *w, size_t n)
{
    if (w->bad)
        return 0;
    if (w->len - w->off < n) {
        char msg[96];
        snprintf(msg, sizeof msg,
                 "文件意外结束（偏移 %zu 处需 %zu 字节，仅剩 %zu）",
                 w->off, n, w->len - w->off);
        wl_fail(w, msg);
        return 0;
    }
    return 1;
}

static uint16_t le_u16(const unsigned char *b)
{
    return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

static uint32_t le_u32(const unsigned char *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint64_t le_u64(const unsigned char *b)
{
    return (uint64_t)le_u32(b) | ((uint64_t)le_u32(b + 4) << 32);
}

static uint16_t wl_u16(wl *w)
{
    uint16_t v;
    if (!wl_need(w, 2))
        return 0;
    v = le_u16(w->p + w->off);
    w->off += 2;
    return v;
}

static uint32_t wl_u32(wl *w)
{
    uint32_t v;
    if (!wl_need(w, 4))
        return 0;
    v = le_u32(w->p + w->off);
    w->off += 4;
    return v;
}

static int32_t wl_i32(wl *w)
{
    return (int32_t)wl_u32(w);
}

static uint8_t wl_u8(wl *w)
{
    if (!wl_need(w, 1))
        return 0;
    return w->p[w->off++];
}

static double wl_f64(wl *w)
{
    uint64_t u;
    double d;
    if (!wl_need(w, 8))
        return 0.0;
    u = le_u64(w->p + w->off);
    memcpy(&d, &u, sizeof d);
    w->off += 8;
    return d;
}

/* 读 n 个 f64（dst 为 NULL 时仅推进，用于测量遍） */
static void wl_f64v(wl *w, double *dst, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        uint64_t u;
        if (!wl_need(w, 8))
            return;
        u = le_u64(w->p + w->off);
        if (dst)
            memcpy(&dst[i], &u, sizeof(double));
        w->off += 8;
    }
}

/* 读 n 个 i32（dst 为 NULL 时仅推进） */
static void wl_i32v(wl *w, int32_t *dst, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (!wl_need(w, 4))
            return;
        if (dst)
            dst[i] = (int32_t)le_u32(w->p + w->off);
        w->off += 4;
    }
}

/* 读一个 str：u16 字节长度 + UTF-8 字节 + NUL。
 * 填充模式拷入字符串区并返回其地址；测量模式返回 NULL。 */
static char *wl_str(wl *w)
{
    uint16_t n = wl_u16(w);
    char *ret = NULL;
    if (w->bad)
        return NULL;
    if (!wl_need(w, (size_t)n + 1))
        return NULL;
    if (w->p[w->off + n] != '\0') {
        wl_fail(w, "字符串缺少 NUL 终止符");
        return NULL;
    }
    if (w->fill) {
        memcpy(w->strcur, w->p + w->off, (size_t)n + 1);
        ret = w->strcur;
        w->strcur += (size_t)n + 1;
    }
    w->str_bytes += (size_t)n + 1;
    w->off += (size_t)n + 1;
    return ret;
}

/* ------------------------------------------------------------ 两遍共享遍历 */

static void wl_sections(wl *w)
{
    crtr_graph *g = w->g;
    size_t i, k;
    int32_t head_prev = 0;

    /* ---- 节点坐标与连通分量 ---- */
    wl_f64v(w, w->fill ? g->lat : NULL, w->n);
    wl_f64v(w, w->fill ? g->lon : NULL, w->n);
    wl_i32v(w, w->fill ? g->comp : NULL, w->n);

    /* ---- CSR 头：校验 head[0]==0、单调不减、head[N]==E ---- */
    for (i = 0; i <= w->n; i++) {
        int32_t v = wl_i32(w);
        if (w->bad)
            return;
        if (i == 0) {
            if (v != 0) {
                wl_fail(w, "CSR head[0] 不为 0");
                return;
            }
        } else if (v < head_prev) {
            wl_fail(w, "CSR head 非单调不减");
            return;
        }
        head_prev = v;
        if (w->fill)
            g->head[i] = v;
    }
    if (head_prev != (int32_t)w->e) {
        wl_fail(w, "CSR head 末值与有向边数不一致");
        return;
    }

    /* ---- 有向边：目标节点 / 权重 / 几何下标 ---- */
    for (k = 0; k < w->e; k++) {
        int32_t v = wl_i32(w);
        if (w->bad)
            return;
        if (v < 0 || (size_t)v >= w->n) {
            wl_fail(w, "边目标节点越界");
            return;
        }
        if (w->fill)
            g->eto[k] = v;
    }
    wl_f64v(w, w->fill ? g->ew : NULL, w->e);
    for (k = 0; k < w->e; k++) {
        int32_t v = wl_i32(w);
        if (w->bad)
            return;
        if (v < -1 || v >= (int32_t)w->gg) {
            wl_fail(w, "egeom 几何下标越界");
            return;
        }
        if (w->fill)
            g->egeom[k] = v;
    }

    /* ---- 几何表：先整段 u32 gcount[G]，其后按条目顺序存 lat/lon 交错点 ---- */
    {
        uint32_t *gc = (uint32_t *)malloc((w->gg ? w->gg : 1) * sizeof(uint32_t));
        size_t pt = 0;   /* gpts 已写下标（填充模式） */
        if (!gc) {
            wl_fail(w, "内存不足（几何计数临时数组）");
            return;
        }
        for (i = 0; i < w->gg; i++) {
            gc[i] = wl_u32(w);
            if (w->bad) {
                free(gc);
                return;
            }
            w->total_pts += gc[i];
        }
        if (w->fill)
            g->goff[0] = 0;
        for (i = 0; i < w->gg; i++) {
            size_t p;
            if (!wl_need(w, (size_t)gc[i] * 16)) {
                free(gc);
                return;
            }
            if (w->fill)
                g->goff[i + 1] = (int64_t)((size_t)g->goff[i] + gc[i]);
            for (p = 0; p < gc[i]; p++) {
                uint64_t u;
                double d;
                u = le_u64(w->p + w->off);
                memcpy(&d, &u, sizeof d);
                if (w->fill)
                    g->gpts[pt++] = d;
                w->off += 8;
                u = le_u64(w->p + w->off);
                memcpy(&d, &u, sizeof d);
                if (w->fill)
                    g->gpts[pt++] = d;
                w->off += 8;
            }
        }
        free(gc);
    }

    /* ---- 车站 S 条：code, node, snap, lat, lon, name, city, addr ---- */
    for (i = 0; i < w->s; i++) {
        char *code = wl_str(w);
        int32_t node = wl_i32(w);
        double snap, slat, slon;
        char *name, *city, *addr;
        if (w->bad)
            return;
        if (node < -1 || node >= (int32_t)w->n) {
            wl_fail(w, "车站吸附节点越界");
            return;
        }
        snap = wl_f64(w);
        slat = wl_f64(w);
        slon = wl_f64(w);
        name = wl_str(w);
        city = wl_str(w);
        addr = wl_str(w);
        if (w->bad)
            return;
        if (w->fill) {
            g->st_code[i] = code;
            g->st_node[i] = node;
            g->st_snap[i] = snap;
            g->st_lat[i] = slat;
            g->st_lon[i] = slon;
            g->st_name[i] = name;
            g->st_city[i] = city;
            g->st_addr[i] = addr;
        }
    }

    /* ---- 站名表 K 条：code, name ---- */
    for (i = 0; i < w->kk; i++) {
        char *code = wl_str(w);
        char *name = wl_str(w);
        if (w->bad)
            return;
        if (w->fill) {
            g->nm_code[i] = code;
            g->nm_name[i] = name;
        }
    }

    /* ---- 同址标注 L 条：code, name, lane_type, alias_of, note ---- */
    for (i = 0; i < w->l; i++) {
        char *code = wl_str(w);
        char *name = wl_str(w);
        char *type = wl_str(w);
        char *alias = wl_str(w);
        char *note = wl_str(w);
        if (w->bad)
            return;
        if (w->fill) {
            g->ln_code[i] = code;
            g->ln_name[i] = name;
            g->ln_type[i] = type;
            g->ln_alias[i] = alias;
            g->ln_note[i] = note;
        }
    }

    /* ---- 轮渡 F 条：4×i64 微度, f64 km, u8 n, n×str（契约只保留坐标） ---- */
    for (i = 0; i < w->f; i++) {
        int64_t c[4];
        size_t q;
        int n_names, t;
        for (q = 0; q < 4; q++) {
            uint64_t u;
            if (!wl_need(w, 8))
                return;
            u = le_u64(w->p + w->off);
            memcpy(&c[q], &u, sizeof c[q]);
            w->off += 8;
        }
        (void)wl_f64(w);                 /* km：契约未保留，仅越过的解析校验 */
        n_names = (int)wl_u8(w);
        if (w->bad)
            return;
        for (t = 0; t < n_names; t++)
            (void)wl_str(w);              /* 名称串：同样只解析校验 */
        if (w->bad)
            return;
        if (w->fill) {
            for (q = 0; q < 4; q++)
                g->ferry[i][q] = c[q];
        }
    }
}

/* ---------------------------------------------------------------- 网格索引 */

static uint32_t cell_hash(int32_t ci, int32_t cj)
{
    /* boost::hash_combine 风格的混合，保证相邻桶也散列分散 */
    uint32_t x = (uint32_t)ci * 0x85EBCA6Bu;
    uint32_t y = (uint32_t)cj * 0xC2B2AE35u;
    uint32_t h = x ^ (y + 0x9E3779B9u + (x << 6) + (x >> 2));
    h ^= h >> 15;
    h *= 0x2545F491u;
    return h ^ (h >> 13);
}

static void grid_destroy(crtr_grid *gr)
{
    if (!gr)
        return;
    free(gr->bucket);
    free(gr->next);
    free(gr->cell_i);
    free(gr->cell_j);
    free(gr);
}

static int grid_build(crtr_graph *g)
{
    crtr_grid *gr = (crtr_grid *)calloc(1, sizeof *gr);
    int32_t n = g->n_nodes;
    int32_t cap = 16;
    int32_t v;

    if (!gr)
        return -1;
    while (cap < 2 * n)          /* 装填因子 <= 0.5 */
        cap <<= 1;
    gr->cap = cap;
    gr->bucket = (int32_t *)malloc((size_t)cap * sizeof(int32_t));
    gr->next = (int32_t *)malloc(((size_t)(n > 0 ? n : 1)) * sizeof(int32_t));
    gr->cell_i = (int32_t *)malloc(((size_t)(n > 0 ? n : 1)) * sizeof(int32_t));
    gr->cell_j = (int32_t *)malloc(((size_t)(n > 0 ? n : 1)) * sizeof(int32_t));
    if (!gr->bucket || !gr->next || !gr->cell_i || !gr->cell_j) {
        grid_destroy(gr);
        return -1;
    }
    for (v = 0; v < cap; v++)
        gr->bucket[v] = -1;
    for (v = 0; v < n; v++) {
        int32_t ci = (int32_t)(g->lat[v] / CRTR_CELL);
        int32_t cj = (int32_t)(g->lon[v] / CRTR_CELL);
        uint32_t h = cell_hash(ci, cj) & (uint32_t)(cap - 1);
        gr->cell_i[v] = ci;
        gr->cell_j[v] = cj;
        gr->next[v] = gr->bucket[h];
        gr->bucket[h] = v;
    }
    g->grid = gr;
    return 0;
}

/* ---------------------------------------------------------------- 哈希表 */

static uint32_t str_hash(const char *s)
{
    /* FNV-1a 64 折叠到 32 位 */
    uint64_t h = 1469598103934665603ull;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ull;
    }
    return (uint32_t)(h ^ (h >> 32));
}

static void map_destroy(crtr_map *m)
{
    if (!m)
        return;
    free(m->key);
    free(m->val);
    free(m);
}

static int map_build(crtr_map **out, char *const *keys, int32_t count)
{
    crtr_map *m = (crtr_map *)calloc(1, sizeof *m);
    int32_t cap = 16;
    int32_t i;

    *out = NULL;
    if (!m)
        return -1;
    if (count > 0) {
        while (cap < 2 * count)      /* 装填因子 <= 0.5，线性探测必终止 */
            cap <<= 1;
        m->cap = cap;
        m->mask = cap - 1;
        m->key = (char **)calloc((size_t)cap, sizeof(char *));
        m->val = (int32_t *)malloc((size_t)cap * sizeof(int32_t));
        if (!m->key || !m->val) {
            map_destroy(m);
            return -1;
        }
        for (i = 0; i < count; i++) {
            uint32_t h = str_hash(keys[i]) & (uint32_t)m->mask;
            while (m->key[h]) {
                if (strcmp(m->key[h], keys[i]) == 0)
                    break;            /* 重复电报码：后者覆盖 */
                h = (h + 1) & (uint32_t)m->mask;
            }
            m->key[h] = keys[i];
            m->val[h] = i;
        }
    }
    *out = m;
    return 0;
}

static int32_t map_get(const crtr_map *m, const char *code)
{
    uint32_t h;
    if (!m || m->cap == 0 || !code)
        return -1;
    h = str_hash(code) & (uint32_t)m->mask;
    while (m->key[h]) {
        if (strcmp(m->key[h], code) == 0)
            return m->val[h];
        h = (h + 1) & (uint32_t)m->mask;
    }
    return -1;
}

/* ---------------------------------------------------------------- 加载 */

int crtr_graph_load(crtr_graph *g, const char *path, char *err, size_t errlen)
{
    FILE *fp;
    long fsz;
    unsigned char *buf = NULL;
    size_t size, off4, str_off, total;
    wl w;
    uint32_t hdr[9];
    int32_t big_comp;

    if (err && errlen)
        err[0] = '\0';
    memset(g, 0, sizeof *g);

    fp = fopen(path, "rb");
    if (!fp) {
        if (err && errlen)
            snprintf(err, errlen, "无法打开图文件：%s", path);
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (fsz = ftell(fp)) < 0 ||
        fseek(fp, 0, SEEK_SET) != 0) {
        if (err && errlen)
            snprintf(err, errlen, "无法确定图文件大小");
        fclose(fp);
        return -1;
    }
    if (fsz == 0) {
        if (err && errlen)
            snprintf(err, errlen, "图文件为空");
        fclose(fp);
        return -1;
    }
    if (fsz > 0x60000000L) {         /* 1.5 GiB 上限，防御异常大文件 */
        if (err && errlen)
            snprintf(err, errlen, "图文件过大（超过 1.5 GiB）");
        fclose(fp);
        return -1;
    }
    size = (size_t)fsz;
    buf = (unsigned char *)malloc(size);
    if (!buf) {
        if (err && errlen)
            snprintf(err, errlen, "内存不足（读取图文件）");
        fclose(fp);
        return -1;
    }
    if (fread(buf, 1, size, fp) != size) {
        if (err && errlen)
            snprintf(err, errlen, "图文件读取不完整");
        free(buf);
        fclose(fp);
        return -1;
    }
    fclose(fp);

    /* ---- 头部：magic + version + 9×u32 + i32 big_comp，共 44 字节 ---- */
    if (size < 44) {
        if (err && errlen)
            snprintf(err, errlen, "文件小于头部（%zu 字节）", size);
        free(buf);
        return -1;
    }
    if (memcmp(buf, "CRTR", 4) != 0) {
        if (err && errlen)
            snprintf(err, errlen, "magic 不符（期望 CRTR）");
        free(buf);
        return -1;
    }
    for (size_t i = 0; i < 9; i++)
        hdr[i] = le_u32(buf + 4 + 4 * i);
    if (hdr[0] != 1) {
        if (err && errlen)
            snprintf(err, errlen, "版本不支持（%u，期望 1）", hdr[0]);
        free(buf);
        return -1;
    }
    big_comp = (int32_t)le_u32(buf + 40);
    if (hdr[1] > CRTR_CNT_MAX || hdr[2] > CRTR_CNT_MAX || hdr[3] > CRTR_CNT_MAX ||
        hdr[4] > CRTR_CNT_MAX || hdr[5] > CRTR_CNT_MAX || hdr[6] > CRTR_CNT_MAX ||
        hdr[7] > CRTR_CNT_MAX || hdr[8] > CRTR_CNT_MAX) {
        if (err && errlen)
            snprintf(err, errlen, "头部计数超出合理上限");
        free(buf);
        return -1;
    }
    if (hdr[7] != 0 && (size_t)hdr[8] > size) {
        if (err && errlen)
            snprintf(err, errlen, "轮渡条目数超出文件大小");
        free(buf);
        return -1;
    }

    memset(&w, 0, sizeof w);
    w.p = buf;
    w.len = size;
    w.off = 44;
    w.fill = 0;
    w.g = g;
    w.n = hdr[1];
    w.e = hdr[2];
    w.gg = hdr[3];
    w.s = hdr[4];
    w.kk = hdr[5];
    w.l = hdr[6];
    w.f = hdr[7];
    w.err = err;
    w.errlen = errlen;

    /* ---- 第一遍：测量（顺带做全部结构校验） ---- */
    wl_sections(&w);
    if (w.bad || w.off != size) {
        if (!w.bad && err && errlen)
            snprintf(err, errlen, "文件尾部有 %zu 字节多余数据", size - w.off);
        free(buf);
        return -1;
    }

    /* ---- 计算一次性 arena 布局：8 字节段 -> 4 字节段 -> 字符串 ---- */
    {
        size_t n = w.n, e = w.e, gg = w.gg, s = w.s, kk = w.kk, l = w.l, f = w.f;
        size_t p8 = 8 * (2 * n + e + (gg + 1) + 2 * w.total_pts + 4 * f + 7 * s + 2 * kk + 5 * l);
        size_t p4 = 4 * (n + (n + 1) + e + e + s);
        off4 = p8;
        str_off = p8 + p4;
        total = str_off + w.str_bytes;
        if (total < w.str_bytes || p8 < 8 * 2 * n) {   /* 溢出防御 */
            if (err && errlen)
                snprintf(err, errlen, "图尺寸计算溢出");
            free(buf);
            return -1;
        }
        g->_arena = malloc(total ? total : 1);
        if (!g->_arena) {
            if (err && errlen)
                snprintf(err, errlen, "内存不足（图数据区 %zu 字节）", total);
            free(buf);
            return -1;
        }
        g->lat = (double *)(g->_arena);
        g->lon = g->lat + n;
        g->ew = g->lon + n;
        g->goff = (int64_t *)(g->ew + e);
        g->gpts = (double *)(g->goff + (gg + 1));
        g->ferry = (int64_t (*)[4])(g->gpts + 2 * w.total_pts);
        {
            char **pp = (char **)(g->ferry + f);
            g->st_snap = (double *)pp;             pp += s;
            g->st_lat = (double *)pp;              pp += s;
            g->st_lon = (double *)pp;              pp += s;
            g->st_code = pp;                       pp += s;
            g->st_name = pp;                       pp += s;
            g->st_city = pp;                       pp += s;
            g->st_addr = pp;                       pp += s;
            g->nm_code = pp;                       pp += kk;
            g->nm_name = pp;                       pp += kk;
            g->ln_code = pp;                       pp += l;
            g->ln_name = pp;                       pp += l;
            g->ln_type = pp;                       pp += l;
            g->ln_alias = pp;                      pp += l;
            g->ln_note = pp;
        }
        g->comp = (int32_t *)((char *)g->_arena + off4);
        g->head = g->comp + n;
        g->eto = g->head + (n + 1);
        g->egeom = g->eto + e;
        g->st_node = g->egeom + e;
        g->n_nodes = (int32_t)n;
        g->n_edges = (int32_t)e;
        g->n_geom = (int32_t)gg;
        g->n_stations = (int32_t)s;
        g->n_names = (int32_t)kk;
        g->n_lanes = (int32_t)l;
        g->n_ferry = (int32_t)f;
        g->big_comp = big_comp;
    }

    /* ---- 第二遍：填充 ---- */
    w.fill = 1;
    w.off = 44;
    w.strcur = (char *)g->_arena + str_off;
    wl_sections(&w);
    if (w.bad) {
        crtr_graph_free(g);
        free(buf);
        return -1;
    }
    free(buf);

    /* ---- 内部索引 ---- */
    if (grid_build(g) != 0) {
        if (err && errlen)
            snprintf(err, errlen, "内存不足（节点网格）");
        crtr_graph_free(g);
        return -1;
    }
    {
        crtr_map *m;
        if (map_build(&m, g->st_code, g->n_stations) != 0) {
            m = NULL;
            goto hash_fail;
        }
        g->st_hash = m;
        if (map_build(&m, g->nm_code, g->n_names) != 0) {
            m = NULL;
            goto hash_fail;
        }
        g->nm_hash = m;
        if (map_build(&m, g->ln_code, g->n_lanes) != 0) {
            m = NULL;
            goto hash_fail;
        }
        g->ln_hash = m;
    }
    return 0;

hash_fail:
    if (err && errlen)
        snprintf(err, errlen, "内存不足（哈希表）");
    crtr_graph_free(g);
    return -1;
}

void crtr_graph_free(crtr_graph *g)
{
    if (!g)
        return;
    grid_destroy((crtr_grid *)g->grid);
    map_destroy((crtr_map *)g->st_hash);
    map_destroy((crtr_map *)g->nm_hash);
    map_destroy((crtr_map *)g->ln_hash);
    free(g->_arena);
    memset(g, 0, sizeof *g);
}

/* ---------------------------------------------------------------- 查询 */

int32_t crtr_nearest_node(const crtr_graph *g, double lat, double lon,
                          int main_only, double *out_dist)
{
    const crtr_grid *gr = (const crtr_grid *)g->grid;
    int32_t best = -1;
    double bd = 1e18;
    int32_t want = 0;
    int filt;
    int32_t ci, cj, steps, di, dj, v;

    if (g->n_nodes <= 0) {
        if (out_dist)
            *out_dist = 1e18;
        return -1;
    }
    filt = main_only;
    want = g->big_comp;
    /* 桶坐标与扫描步数同 server.py：int(x/cell) 与 int(0.6/0.05)+1 = 12 */
    ci = (int32_t)(lat / CRTR_CELL);
    cj = (int32_t)(lon / CRTR_CELL);
    steps = (int)(0.6 / CRTR_CELL) + 1;

    for (di = -steps; di <= steps; di++) {
        for (dj = -steps; dj <= steps; dj++) {
            uint32_t h = cell_hash(ci + di, cj + dj) & (uint32_t)(gr->cap - 1);
            for (v = gr->bucket[h]; v >= 0; v = gr->next[v]) {
                double d;
                if (gr->cell_i[v] != ci + di || gr->cell_j[v] != cj + dj)
                    continue;
                if (filt && g->comp[v] != want)
                    continue;
                d = crtr_haversine(lat, lon, g->lat[v], g->lon[v]);
                if (d < bd) {
                    bd = d;
                    best = v;
                }
            }
        }
    }
    if (best < 0) {
        /* 回退：全图线性扫描（主网节点远离查询点时，同 server.py） */
        for (v = 0; v < g->n_nodes; v++) {
            double d;
            if (filt && g->comp[v] != want)
                continue;
            d = crtr_haversine(lat, lon, g->lat[v], g->lon[v]);
            if (d < bd) {
                bd = d;
                best = v;
            }
        }
    }
    if (out_dist)
        *out_dist = bd;
    return best;
}

int32_t crtr_find_edge(const crtr_graph *g, int32_t a, int32_t b)
{
    int32_t k, end;
    if ((uint32_t)a >= (uint32_t)g->n_nodes)
        return -1;
    end = g->head[a + 1];
    for (k = g->head[a]; k < end; k++)
        if (g->eto[k] == b)
            return k;
    return -1;
}

double crtr_path_length(const crtr_graph *g, const int32_t *path, int32_t len)
{
    double total = 0.0;
    int32_t i;
    if (!path || len < 2)
        return 0.0;
    for (i = 0; i + 1 < len; i++) {
        int32_t a = path[i], b = path[i + 1];
        int32_t k = crtr_find_edge(g, a, b);
        if (k >= 0)
            total += g->ew[k];
        else
            total += crtr_haversine(g->lat[a], g->lon[a], g->lat[b], g->lon[b]);
    }
    return total;
}

int32_t crtr_station_index(const crtr_graph *g, const char *code)
{
    return map_get((const crtr_map *)g->st_hash, code);
}

const char *crtr_name_of(const crtr_graph *g, const char *code)
{
    int32_t i = map_get((const crtr_map *)g->nm_hash, code);
    return i >= 0 ? g->nm_name[i] : NULL;
}

const char *crtr_lane_alias(const crtr_graph *g, const char *code)
{
    int32_t i = map_get((const crtr_map *)g->ln_hash, code);
    /* 空 aliasOf 视为无别名（同 Python 的 falsy 判断） */
    return (i >= 0 && g->ln_alias[i][0] != '\0') ? g->ln_alias[i] : NULL;
}
