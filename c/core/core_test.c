/* 核心自测 —— 按 c/PLAN.md §4 在临时文件构造微型图，验证：
 *   加载（含错误路径）/ A*（含位图 ban 与惩罚两种过滤形态）/ nearest_node
 *   （main_only 分量过滤与远距离回退）/ find_edge / path_length（含兜底）
 *   / 车站·站名·同址查询 / 位图辅助 / 上下文复用（代际失效）。
 * 若存在真实数据（data/rail_graph.bin），追加冒烟：规模断言 + 一条 A*。
 * 全部通过返回 0，任一失败返回 1。
 */
#include "geom.h"
#include "graph.h"
#include "astar.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static int g_checks = 0;
static int g_fails = 0;

#define CHECK(cond, ...)                                                    \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_fails++;                                                       \
            printf("FAIL(L%d): ", __LINE__);                                 \
            printf(__VA_ARGS__);                                             \
            printf("\n");                                                    \
        }                                                                    \
    } while (0)

static int feq(double a, double b, double tol)
{
    double d = a - b;
    if (d < 0)
        d = -d;
    return d <= tol;
}

static int path_eq(const int32_t *p, int32_t np, const int32_t *q, int32_t nq)
{
    int32_t i;
    if (np != nq)
        return 0;
    for (i = 0; i < np; i++)
        if (p[i] != q[i])
            return 0;
    return 1;
}

/* ---------------------------------------------------------------- 二进制写出 */

typedef struct {
    FILE *f;
} bw;

static void bw_raw(bw *w, const void *p, size_t n)
{
    if (fwrite(p, 1, n, w->f) != n) {
        fprintf(stderr, "写临时图文件失败\n");
        exit(2);
    }
}

static void bw_u8(bw *w, unsigned v)
{
    unsigned char b = (unsigned char)(v & 0xffu);
    bw_raw(w, &b, 1);
}

static void bw_u16(bw *w, unsigned v)
{
    bw_u8(w, v);
    bw_u8(w, v >> 8);
}

static void bw_u32(bw *w, uint32_t v)
{
    bw_u8(w, v);
    bw_u8(w, v >> 8);
    bw_u8(w, v >> 16);
    bw_u8(w, v >> 24);
}

static void bw_i32(bw *w, int32_t v)
{
    bw_u32(w, (uint32_t)v);
}

static void bw_i64(bw *w, int64_t v)
{
    bw_u32(w, (uint32_t)((uint64_t)v & 0xffffffffull));
    bw_u32(w, (uint32_t)((uint64_t)v >> 32));
}

static void bw_f64(bw *w, double v)
{
    uint64_t u;
    memcpy(&u, &v, sizeof u);
    bw_u32(w, (uint32_t)(u & 0xffffffffull));
    bw_u32(w, (uint32_t)(u >> 32));
}

static void bw_str(bw *w, const char *s)
{
    size_t n = strlen(s);
    if (n > 65535) {
        fprintf(stderr, "测试字符串超长\n");
        exit(2);
    }
    bw_u16(w, (unsigned)n);
    bw_raw(w, s, n);
    bw_u8(w, 0);   /* NUL */
}

/* --------------------------------------------------------- 微型图（§4 规范）
 *
 * 拓扑：两条并行路线 + 孤岛分量 + 轮渡边
 *
 *   0 --1-- 2 --3          上路（较短，带几何链）
 *    \        /
 *     4 ---- 5             下路（较长）
 *
 *   6 -- 7                 孤岛（分量 1）
 *   3 == 8                 轮渡边（egeom=-1，命中轮渡表）
 */
static const double M_LAT[9] = {
    30.00, 30.01, 30.02, 30.03,
    29.96, 29.92,
    28.00, 28.01,
    30.10
};
static const double M_LON[9] = {
    110.00, 110.06, 110.12, 110.18,
    110.06, 110.12,
    112.00, 112.01,
    110.20
};
static const int32_t M_COMP[9] = { 0, 0, 0, 0, 0, 0, 1, 1, 0 };

static const int32_t M_ADJ[9][3] = {
    { 1, 4, -1 }, { 0, 2, -1 }, { 1, 3, -1 }, { 2, 5, 8 },
    { 0, 5, -1 }, { 4, 3, -1 }, { 7, -1, -1 }, { 6, -1, -1 }, { 3, -1, -1 }
};
static const int32_t M_DEG[9] = { 2, 2, 2, 3, 2, 2, 1, 1, 1 };

static int m_geo(int32_t a, int32_t b)
{
    if ((a == 0 && b == 1) || (a == 1 && b == 0))
        return 0;
    if ((a == 1 && b == 2) || (a == 2 && b == 1))
        return 1;
    return -1;
}

static double m_hav(int32_t a, int32_t b)
{
    return crtr_haversine(M_LAT[a], M_LON[a], M_LAT[b], M_LON[b]);
}

static void build_micro(const char *path)
{
    bw w;
    int32_t head[10];
    int32_t i, k;
    FILE *f = fopen(path, "wb");

    if (!f) {
        fprintf(stderr, "无法创建临时图文件 %s\n", path);
        exit(2);
    }
    w.f = f;

    /* 头部 44 字节 */
    bw_raw(&w, "CRTR", 4);
    bw_u32(&w, 1);        /* version */
    bw_u32(&w, 9);        /* n_nodes */
    bw_u32(&w, 16);       /* n_dir_edges */
    bw_u32(&w, 2);        /* n_geom */
    bw_u32(&w, 2);        /* n_stations */
    bw_u32(&w, 3);        /* n_names */
    bw_u32(&w, 2);        /* n_lanes */
    bw_u32(&w, 1);        /* n_ferry */
    bw_u32(&w, 0);        /* reserved */
    bw_i32(&w, 0);        /* big_comp */

    /* 节点 */
    for (i = 0; i < 9; i++)
        bw_f64(&w, M_LAT[i]);
    for (i = 0; i < 9; i++)
        bw_f64(&w, M_LON[i]);
    for (i = 0; i < 9; i++)
        bw_i32(&w, M_COMP[i]);

    /* CSR */
    head[0] = 0;
    for (i = 0; i < 9; i++)
        head[i + 1] = head[i] + M_DEG[i];
    for (i = 0; i <= 9; i++)
        bw_i32(&w, head[i]);
    for (i = 0; i < 9; i++)
        for (k = 0; k < M_DEG[i]; k++)
            bw_i32(&w, M_ADJ[i][k]);
    for (i = 0; i < 9; i++)
        for (k = 0; k < M_DEG[i]; k++)
            bw_f64(&w, m_hav(i, M_ADJ[i][k]));
    for (i = 0; i < 9; i++)
        for (k = 0; k < M_DEG[i]; k++)
            bw_i32(&w, m_geo(i, M_ADJ[i][k]));

    /* 几何表：条目 0 三点（边 0-1），条目 1 一点（边 1-2） */
    bw_u32(&w, 3);
    bw_u32(&w, 1);
    bw_f64(&w, 30.003); bw_f64(&w, 110.02);
    bw_f64(&w, 30.007); bw_f64(&w, 110.04);
    bw_f64(&w, 30.010); bw_f64(&w, 110.06);
    bw_f64(&w, 30.020); bw_f64(&w, 110.12);

    /* 车站 2 条 */
    bw_str(&w, "AAA"); bw_i32(&w, 0); bw_f64(&w, 12.5);
    bw_f64(&w, 30.0001); bw_f64(&w, 110.0001);
    bw_str(&w, "甲站"); bw_str(&w, "城甲"); bw_str(&w, "甲址一号");
    bw_str(&w, "BBB"); bw_i32(&w, 3); bw_f64(&w, 30.0);
    bw_f64(&w, 30.0301); bw_f64(&w, 110.1801);
    bw_str(&w, "乙站"); bw_str(&w, "城乙"); bw_str(&w, "乙址二号");

    /* 站名表 3 条 */
    bw_str(&w, "AAA"); bw_str(&w, "甲站");
    bw_str(&w, "BBB"); bw_str(&w, "乙站");
    bw_str(&w, "CCC"); bw_str(&w, "丙站");

    /* 同址标注 2 条 */
    bw_str(&w, "DDD"); bw_str(&w, "丁站"); bw_str(&w, "同址");
    bw_str(&w, "AAA"); bw_str(&w, "与甲站同址");
    bw_str(&w, "EEE"); bw_str(&w, "戊站"); bw_str(&w, "同址");
    bw_str(&w, "");    bw_str(&w, "");

    /* 轮渡 1 条：3 -> 8 */
    bw_i64(&w, (int64_t)llround(30.03 * 1e6));
    bw_i64(&w, (int64_t)llround(110.18 * 1e6));
    bw_i64(&w, (int64_t)llround(30.10 * 1e6));
    bw_i64(&w, (int64_t)llround(110.20 * 1e6));
    bw_f64(&w, m_hav(3, 8) / 1000.0);
    bw_u8(&w, 1);
    bw_str(&w, "测试轮渡");

    fclose(f);
}

/* ---------------------------------------------------------------- 微图测试 */

static void test_micro(const char *path)
{
    crtr_graph g;
    crtr_astar *ctx;
    char err[256];
    double ltop, lbot, w38, d, tol;
    int32_t *p = NULL;
    int32_t npath = 0;
    int32_t expect_top[4] = { 0, 1, 2, 3 };
    int32_t expect_bot[4] = { 0, 4, 5, 3 };
    int32_t expect_bot_rev[4] = { 3, 5, 4, 0 };
    int32_t i;

    /* 期望值（与写盘同一公式） */
    ltop = m_hav(0, 1) + m_hav(1, 2) + m_hav(2, 3);
    lbot = m_hav(0, 4) + m_hav(4, 5) + m_hav(5, 3);
    w38 = m_hav(3, 8);
    tol = 1e-6;
    CHECK(ltop < lbot, "前提：上路应短于下路（%f < %f）", ltop, lbot);
    CHECK(1.05 * ltop < lbot, "前提：轻度惩罚后上路仍最优");
    CHECK(6.0 * ltop > lbot, "前提：重度惩罚后上路劣于下路");

    /* ---- 加载 ---- */
    CHECK(crtr_graph_load(&g, path, err, sizeof err) == 0, "加载失败：%s", err);
    if (g_fails)
        return;
    CHECK(g.n_nodes == 9 && g.n_edges == 16 && g.n_geom == 2 &&
          g.n_stations == 2 && g.n_names == 3 && g.n_lanes == 2 &&
          g.n_ferry == 1 && g.big_comp == 0,
          "头部字段不符（N=%d E=%d G=%d S=%d K=%d L=%d F=%d big=%d）",
          g.n_nodes, g.n_edges, g.n_geom, g.n_stations, g.n_names,
          g.n_lanes, g.n_ferry, g.big_comp);
    CHECK(feq(g.lat[4], 29.96, 0) && feq(g.lon[8], 110.20, 0), "节点坐标不符");
    CHECK(g.comp[6] == 1 && g.comp[0] == 0, "连通分量不符");
    CHECK(g.ferry[0][0] == (int64_t)llround(30.03 * 1e6) &&
          g.ferry[0][1] == (int64_t)llround(110.18 * 1e6) &&
          g.ferry[0][2] == (int64_t)llround(30.10 * 1e6) &&
          g.ferry[0][3] == (int64_t)llround(110.20 * 1e6),
          "轮渡微度坐标不符");
    CHECK(g.goff[0] == 0 && g.goff[1] == 3 && g.goff[2] == 4, "几何偏移不符");
    CHECK(feq(g.gpts[0], 30.003, 0) && feq(g.gpts[1], 110.02, 0) &&
          feq(g.gpts[6], 30.020, 0) && feq(g.gpts[7], 110.12, 0),
          "几何点不符");

    /* ---- find_edge / egeom ---- */
    {
        int32_t k01 = crtr_find_edge(&g, 0, 1);
        int32_t k10 = crtr_find_edge(&g, 1, 0);
        int32_t k12 = crtr_find_edge(&g, 1, 2);
        int32_t k38 = crtr_find_edge(&g, 3, 8);
        CHECK(k01 >= 0 && k10 >= 0 && k12 >= 0 && k38 >= 0, "应有边缺失");
        CHECK(k01 >= 0 && g.eto[k01] == 1, "边 0->1 目标不符");
        CHECK(k01 >= 0 && g.egeom[k01] == 0, "边 0->1 几何下标不符");
        CHECK(k12 >= 0 && g.egeom[k12] == 1, "边 1->2 几何下标不符");
        CHECK(crtr_find_edge(&g, 0, 4) >= 0 && g.egeom[crtr_find_edge(&g, 0, 4)] == -1,
              "边 0->4 应无几何");
        CHECK(crtr_find_edge(&g, 0, 2) == -1, "边 0->2 不应存在");
        CHECK(crtr_find_edge(&g, 0, 8) == -1, "边 0->8 不应存在");
        CHECK(crtr_find_edge(&g, 99, 0) == -1, "越界节点应返回 -1");
    }

    /* ---- path_length ---- */
    {
        int32_t top_path[4] = { 0, 1, 2, 3 };
        int32_t direct[2] = { 0, 3 };
        CHECK(feq(crtr_path_length(&g, top_path, 4), ltop, tol),
              "path_length 上路不符");
        CHECK(feq(crtr_path_length(&g, direct, 2), m_hav(0, 3), tol),
              "path_length 无边时应按 haversine 兜底");
    }

    /* ---- A* 基础 ---- */
    ctx = crtr_astar_new(&g);
    CHECK(ctx != NULL, "上下文分配失败");

    d = crtr_astar_run(&g, ctx, 0, 3, NULL, &p, &npath);
    CHECK(feq(d, ltop, tol), "A* 0->3 距离不符（%f vs %f）", d, ltop);
    CHECK(path_eq(p, npath, expect_top, 4), "A* 0->3 路径不符");
    CHECK(npath == 4, "A* 0->3 路径长度不符（%d）", npath);
    free(p); p = NULL;

    d = crtr_astar_run(&g, ctx, 3, 0, NULL, &p, &npath);
    CHECK(feq(d, ltop, tol), "A* 3->0 距离不符（无向对称）");
    free(p); p = NULL;

    /* 不可达：孤岛 */
    npath = 7;
    d = crtr_astar_run(&g, ctx, 0, 6, NULL, &p, &npath);
    CHECK(d == -1.0 && p == NULL && npath == 0, "跨分量应不可达");
    free(p); p = NULL;

    /* src == dst */
    d = crtr_astar_run(&g, ctx, 2, 2, NULL, &p, &npath);
    CHECK(d == 0.0 && npath == 1 && p != NULL && p[0] == 2, "src==dst 应返回单点");
    free(p); p = NULL;

    /* 越界防御 */
    CHECK(crtr_astar_run(&g, ctx, -1, 0, NULL, &p, &npath) == -1.0, "src 越界应拒绝");
    CHECK(crtr_astar_run(&g, ctx, 0, 100, NULL, &p, &npath) == -1.0, "dst 越界应拒绝");

    /* out_path 为 NULL 也能用 */
    d = crtr_astar_run(&g, ctx, 0, 3, NULL, NULL, NULL);
    CHECK(feq(d, ltop, tol), "out_path=NULL 距离不符");

    /* 含轮渡边的路径 */
    d = crtr_astar_run(&g, ctx, 0, 8, NULL, &p, &npath);
    CHECK(feq(d, ltop + w38, tol), "A* 0->8 距离不符（%f）", d);
    CHECK(npath == 5 && p[3] == 3 && p[4] == 8, "A* 0->8 路径不符");
    free(p); p = NULL;

    /* ---- 过滤：ban（_shortest_avoiding 形态） ---- */
    {
        uint64_t *bits = crtr_bits_alloc(&g);
        crtr_edge_filter flt;
        int32_t kf = crtr_find_edge(&g, 1, 2);
        memset(&flt, 0, sizeof flt);
        CHECK(kf >= 0, "边 1->2 应存在");
        CHECK(bits != NULL, "位图分配失败");
        CHECK(crtr_bits_bytes(&g) == 8, "位图字节数不符（%zu）", crtr_bits_bytes(&g));

        flt.bits = bits;
        flt.ban = 1;
        flt.factor = 1.0;

        /* 仅禁正向 1->2 */
        if (kf >= 0)
            crtr_bits_set(bits, kf);
        d = crtr_astar_run(&g, ctx, 0, 3, &flt, &p, &npath);
        CHECK(feq(d, lbot, tol) && path_eq(p, npath, expect_bot, 4),
              "禁 1->2 后应改走下路");
        free(p); p = NULL;

        /* 双向禁整条上路（反向查询应走下路的反序） */
        crtr_bits_set(bits, crtr_find_edge(&g, 2, 1));
        d = crtr_astar_run(&g, ctx, 3, 0, &flt, &p, &npath);
        CHECK(feq(d, lbot, tol) && path_eq(p, npath, expect_bot_rev, 4),
              "双向禁后反向也应走下路");
        free(p); p = NULL;

        /* 位图非零校验后清空复用 */
        CHECK(kf >= 0 && crtr_bits_get(bits, kf) == 1, "位图 get/set 不符");
        memset(bits, 0, crtr_bits_bytes(&g));
        CHECK(kf >= 0 && crtr_bits_get(bits, kf) == 0, "位图清零不符");

        /* ---- 过滤：惩罚（plan_by_km 形态） ---- */
        flt.ban = 0;
        flt.factor = 6.0;
        for (i = 0; i + 1 < 4; i++) {
            crtr_bits_set(bits, crtr_find_edge(&g, expect_top[i], expect_top[i + 1]));
            crtr_bits_set(bits, crtr_find_edge(&g, expect_top[i + 1], expect_top[i]));
        }
        d = crtr_astar_run(&g, ctx, 0, 3, &flt, &p, &npath);
        CHECK(feq(d, lbot, tol) && path_eq(p, npath, expect_bot, 4),
              "重惩罚后应走下路");
        free(p); p = NULL;

        memset(bits, 0, crtr_bits_bytes(&g));
        flt.factor = 1.05;
        for (i = 0; i + 1 < 4; i++) {
            crtr_bits_set(bits, crtr_find_edge(&g, expect_top[i], expect_top[i + 1]));
            crtr_bits_set(bits, crtr_find_edge(&g, expect_top[i + 1], expect_top[i]));
        }
        d = crtr_astar_run(&g, ctx, 0, 3, &flt, &p, &npath);
        CHECK(feq(d, 1.05 * ltop, tol) && path_eq(p, npath, expect_top, 4),
              "轻惩罚应保持上路且返回惩罚距离（%f）", d);
        free(p); p = NULL;

        /* filter 结构体存在但 bits=NULL：不过滤 */
        flt.bits = NULL;
        d = crtr_astar_run(&g, ctx, 0, 3, &flt, &p, &npath);
        CHECK(feq(d, ltop, tol), "bits=NULL 应不过滤");
        free(p); p = NULL;

        crtr_bits_free(bits);
    }

    /* ---- 上下文复用（代际失效） ---- */
    d = crtr_astar_run(&g, ctx, 0, 6, NULL, &p, &npath);
    CHECK(d == -1.0, "复用后跨分量仍应不可达");
    free(p); p = NULL;
    d = crtr_astar_run(&g, ctx, 0, 3, NULL, &p, &npath);
    CHECK(feq(d, ltop, tol) && path_eq(p, npath, expect_top, 4), "复用后结果应一致");
    free(p); p = NULL;

    crtr_astar_free(ctx);

    /* ---- nearest_node ---- */
    {
        double dd = -1.0;
        int32_t v = crtr_nearest_node(&g, 30.0101, 110.0601, 1, &dd);
        CHECK(v == 1 && dd < 200.0, "节点 1 附近应吸附到 1（v=%d d=%f）", v, dd);
        v = crtr_nearest_node(&g, 30.00, 110.00, 1, NULL);
        CHECK(v == 0, "节点 0 坐标应返回 0");
        v = crtr_nearest_node(&g, 28.002, 112.003, 0, &dd);
        CHECK(v == 6, "孤岛查询（含孤岛）应返回 6（v=%d）", v);
        v = crtr_nearest_node(&g, 28.002, 112.003, 1, &dd);
        CHECK(v >= 0 && g.comp[v] == 0 && dd > 100000.0,
              "孤岛查询（仅主网）应远距回退到主网（v=%d d=%f）", v, dd);
    }

    /* ---- 车站 / 站名 / 同址查询 ---- */
    CHECK(crtr_station_index(&g, "AAA") == 0 && crtr_station_index(&g, "BBB") == 1,
          "车站下标不符");
    CHECK(crtr_station_index(&g, "ZZZ") == -1, "不存在车站应返回 -1");
    CHECK(crtr_name_of(&g, "AAA") != NULL && strcmp(crtr_name_of(&g, "AAA"), "甲站") == 0,
          "name_of AAA 不符");
    CHECK(crtr_name_of(&g, "CCC") != NULL && strcmp(crtr_name_of(&g, "CCC"), "丙站") == 0,
          "name_of CCC 不符");
    CHECK(crtr_name_of(&g, "NOPE") == NULL, "name_of 应允许不存在");
    CHECK(crtr_lane_alias(&g, "DDD") != NULL && strcmp(crtr_lane_alias(&g, "DDD"), "AAA") == 0,
          "lane_alias DDD 不符");
    CHECK(crtr_lane_alias(&g, "EEE") == NULL, "空 aliasOf 应视为无别名");
    CHECK(crtr_lane_alias(&g, "AAA") == NULL, "非标注站应无别名");

    crtr_graph_free(&g);
    CHECK(g.n_nodes == 0 && g._arena == NULL && g.grid == NULL, "free 后应清零");
}

/* ------------------------------------------------------- 真实图冒烟测试 */

static const char *find_real_bin(void)
{
    static const char *cands[] = {
        "data/rail_graph.bin",
        "../data/rail_graph.bin",
        "../../data/rail_graph.bin",
        NULL
    };
    const char *env = getenv("CRTR_TEST_BIN");
    FILE *f;
    int i;
    if (env && *env) {
        f = fopen(env, "rb");
        if (f) {
            fclose(f);
            return env;
        }
    }
    for (i = 0; cands[i]; i++) {
        f = fopen(cands[i], "rb");
        if (f) {
            fclose(f);
            return cands[i];
        }
    }
    return NULL;
}

static void test_real(void)
{
    const char *path = find_real_bin();
    crtr_graph g;
    crtr_astar *ctx;
    char err[256];
    clock_t t0, t1;
    double d, pl, ms;
    int32_t *p = NULL;
    int32_t npath = 0, src = -1, dst = -1, i;

    if (!path) {
        printf("SKIP 真实图冒烟：未找到 data/rail_graph.bin\n");
        return;
    }
    printf("真实图冒烟：%s\n", path);
    t0 = clock();
    CHECK(crtr_graph_load(&g, path, err, sizeof err) == 0, "真实图加载失败：%s", err);
    if (g_fails)
        return;
    t1 = clock();
    ms = (double)(t1 - t0) * 1000.0 / CLOCKS_PER_SEC;
    printf("  加载用时 %.0f ms\n", ms);

    /* qa-bench 产出的规模（联调数据 2026-10-19） */
    CHECK(g.n_nodes == 196848, "N 不符（%d）", g.n_nodes);
    CHECK(g.n_edges == 490570, "E 不符（%d）", g.n_edges);
    CHECK(g.n_geom == 245285, "G 不符（%d）", g.n_geom);
    CHECK(g.n_stations == 2879 && g.n_names == 3404 &&
          g.n_lanes == 6 && g.n_ferry == 2, "S/K/L/F 不符");
    CHECK(g.big_comp == 0, "big_comp 不符");

    CHECK(crtr_station_index(&g, "IZQ") >= 0, "站表缺少 IZQ");
    CHECK(crtr_name_of(&g, "IZQ") != NULL, "站名表缺少 IZQ");

    /* 取主分量首末节点跑一条 A* */
    for (i = 0; i < g.n_nodes; i++) {
        if (g.comp[i] == g.big_comp) {
            if (src < 0)
                src = i;
            dst = i;
        }
    }
    CHECK(src >= 0 && dst >= 0 && src != dst, "主分量应有至少两个节点");
    ctx = crtr_astar_new(&g);
    CHECK(ctx != NULL, "真实图上下文分配失败");
    t0 = clock();
    d = crtr_astar_run(&g, ctx, src, dst, NULL, &p, &npath);
    t1 = clock();
    ms = (double)(t1 - t0) * 1000.0 / CLOCKS_PER_SEC;
    printf("  A* %d->%d：%.3f km，%d 节点，%.1f ms\n",
           src, dst, d / 1000.0, npath, ms);
    CHECK(d >= 0.0, "主分量内应可达");
    CHECK(p != NULL && npath >= 2, "路径应非空");
    if (p) {
        /* 路径连续性：相邻节点必须有边 */
        int32_t bad = 0;
        for (i = 0; i + 1 < npath; i++)
            if (crtr_find_edge(&g, p[i], p[i + 1]) < 0) {
                bad = 1;
                break;
            }
        CHECK(bad == 0, "路径在 %d 处不连续", i);
        pl = crtr_path_length(&g, p, npath);
        CHECK(feq(pl, d, d * 1e-9 + 1e-6), "path_length 与 A* 距离不一致（%f vs %f）", pl, d);
    }
    free(p); p = NULL;

    /* 同一上下文反向再查（代际复用 + 堆增长压力） */
    d = crtr_astar_run(&g, ctx, dst, src, NULL, &p, &npath);
    CHECK(d >= 0.0 && npath >= 2, "反向应可达");
    free(p); p = NULL;
    crtr_astar_free(ctx);

    /* 真实图位图 */
    {
        uint64_t *bits = crtr_bits_alloc(&g);
        CHECK(bits != NULL, "真实图位图分配失败");
        CHECK(crtr_bits_bytes(&g) == (((size_t)490570 + 63) / 64) * 8,
              "真实图位图字节数不符（%zu）", crtr_bits_bytes(&g));
        crtr_bits_set(bits, 100000);
        CHECK(crtr_bits_get(bits, 100000) == 1 && crtr_bits_get(bits, 100001) == 0,
              "真实图位图 get/set 不符");
        crtr_bits_free(bits);
    }
    crtr_graph_free(&g);
}

/* ------------------------------------------------------------- 错误路径 */

static void test_errors(const char *good_path)
{
    crtr_graph g;
    char err[256], tiny[8];
    unsigned char buf[64];
    FILE *f;
    size_t got;

    CHECK(crtr_graph_load(&g, "no_such_file_zzz.bin", err, sizeof err) != 0,
          "不存在的文件应失败");
    CHECK(err[0] != '\0', "失败应写入原因");
    crtr_graph_free(&g);   /* 失败后 free 应安全 */

    /* 极小 err 缓冲不越界 */
    memset(tiny, 'X', sizeof tiny);
    CHECK(crtr_graph_load(&g, "no_such_file_zzz.bin", tiny, sizeof tiny) != 0,
          "小缓冲也应失败");
    CHECK(tiny[sizeof tiny - 1] == '\0', "小缓冲应正确截断");

    /* magic 损坏 */
    f = fopen(good_path, "rb");
    CHECK(f != NULL, "重开微图失败");
    if (f) {
        got = fread(buf, 1, sizeof buf, f);
        fclose(f);
        CHECK(got == sizeof buf, "读取微图头部失败");
        buf[0] = 'X';
        f = fopen("crtr_bad_magic.bin", "wb");
        fwrite(buf, 1, sizeof buf, f);
        fclose(f);
        CHECK(crtr_graph_load(&g, "crtr_bad_magic.bin", err, sizeof err) != 0,
              "坏 magic 应失败");
        crtr_graph_free(&g);
        remove("crtr_bad_magic.bin");

        /* 截断文件 */
        f = fopen("crtr_trunc.bin", "wb");
        fwrite(buf, 1, 23, f);   /* 故意截在头部中间 */
        fclose(f);
        CHECK(crtr_graph_load(&g, "crtr_trunc.bin", err, sizeof err) != 0,
              "截断文件应失败");
        crtr_graph_free(&g);
        remove("crtr_trunc.bin");
    }
}

int main(void)
{
    const char *micro = "crtr_core_test_graph.bin";
    int rc;

#ifdef _WIN32
    SetConsoleOutputCP(65001);   /* 控制台按 UTF-8 显示中文 */
#endif

    build_micro(micro);
    test_micro(micro);
    test_errors(micro);
    if (g_fails == 0)
        remove(micro);   /* 失败时保留产物便于排查 */

    test_real();

    printf("==== 核心自测：%d 项检查，%d 失败 ====", g_checks, g_fails);
    if (g_fails)
        printf(" [FAIL]\n");
    else
        printf(" [PASS]\n");
    rc = g_fails ? 1 : 0;
    return rc;
}
