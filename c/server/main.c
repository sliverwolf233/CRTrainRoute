/* crtr_server —— 中国铁路算路 API 的 C 版服务入口
 *
 * 用法：crtr_server [--graph <path>] [--port <n>] [--threads <n>] [--help]
 *   --graph   二进制图路径（默认 data/rail_graph.bin，相对工作目录）
 *   --port    监听端口（默认 8889，同 server.py）
 *   --threads 线程池大小（默认硬件线程数，上限 16；环境变量 CRTR_THREADS 同效）
 *
 * 静态页面（index.html / config.js）从图文件所在目录的上一级提供，
 * 等价于 Python 版以 server.py 所在目录为根（默认布局下即仓库根）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "http.h"
#include "threadpool.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

/* Windows 控制台 argv 按 ANSI 代码页传入；统一转成 UTF-8 再解析。 */
static char **utf8_argv(int *argc_out) {
    int wargc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    char **argv = calloc((size_t)wargc + 1, sizeof *argv);
    if (!argv) { LocalFree(wargv); exit(1); }
    for (int i = 0; i < wargc; i++) {
        int n = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        argv[i] = malloc((size_t)(n > 0 ? n : 1));
        if (!argv[i] || n <= 0 ||
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], n,
                                NULL, NULL) != n)
            argv[i][0] = '\0';
    }
    LocalFree(wargv);
    *argc_out = wargc;
    return argv;
}
#endif

static void usage(FILE *out) {
    fputs("crtr_server —— 中国铁路算路 API（C 版）\n"
          "\n"
          "用法: crtr_server [选项]\n"
          "  --graph <path>    二进制图路径（默认 data/rail_graph.bin）\n"
          "  --port <n>        监听端口（默认 8889）\n"
          "  --threads <n>     线程数（默认硬件线程数，上限 16，可用 CRTR_THREADS）\n"
          "  --help, -h        显示本说明\n",
          out);
}

/* 由图路径推 Web 根：取图所在目录的上一级，
 * data/rail_graph.bin -> "data" 的上级 -> "."（仓库根，同 Python BASE） */
static void webroot_from_graph(const char *graph_path, char *out, size_t outsz) {
    size_t n = strlen(graph_path);
    while (n > 1 && (graph_path[n - 1] == '/' || graph_path[n - 1] == '\\')) n--;

    /* 第一层：图文件的目录部分（不含分隔符） */
    size_t cut = n;
    while (cut > 0 && graph_path[cut - 1] != '/' && graph_path[cut - 1] != '\\') cut--;
    if (cut == 0) { snprintf(out, outsz, "."); return; }        /* 无目录部分 */
    size_t dlen = cut - 1;
    while (dlen > 0 && (graph_path[dlen - 1] == '/' || graph_path[dlen - 1] == '\\')) dlen--;

    /* 第二层：再上一级（静态页面与图数据同仓库布局） */
    size_t cut2 = dlen;
    while (cut2 > 0 && graph_path[cut2 - 1] != '/' && graph_path[cut2 - 1] != '\\') cut2--;
    if (cut2 == 0) { snprintf(out, outsz, "."); return; }       /* 上级即当前目录 */
    size_t wlen = cut2 - 1;
    while (wlen > 0 && (graph_path[wlen - 1] == '/' || graph_path[wlen - 1] == '\\')) wlen--;
    if (wlen == 0) { snprintf(out, outsz, "/"); return; }
    if (wlen >= outsz) wlen = outsz - 1;
    memcpy(out, graph_path, wlen);
    out[wlen] = '\0';
}

int main(int argc_, char **argv_) {
#ifdef _WIN32
    SetConsoleOutputCP(65001);        /* 让 UTF-8 日志在控制台可读 */
    int argc = 0;
    char **argv = utf8_argv(&argc);
#else
    int argc = argc_;
    char **argv = argv_;
#endif
    (void)argc_; (void)argv_;          /* Windows 下已用 UTF-8 副本 */
    const char *graph_path = "data/rail_graph.bin";
    int port = 8889;
    int threads = 0;

    const char *env_t = getenv("CRTR_THREADS");
    if (env_t && *env_t) {
        char *end = NULL;
        long v = strtol(env_t, &end, 10);
        if (end && *end == '\0' && v > 0) threads = (int)v;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(argv[i], "--graph") == 0 && i + 1 < argc) {
            graph_path = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            threads = atoi(argv[++i]);
        } else {
            fprintf(stderr, "未知参数: %s\n", argv[i]);
            usage(stderr);
            return 1;
        }
    }
    if (port <= 0 || port > 65535) {
        fprintf(stderr, "端口非法: %d\n", port);
        return 1;
    }

    /* 加载图 */
    printf("加载铁路图...\n");
    fflush(stdout);
    double t0 = crtr_monotonic_ms();
    crtr_graph *g = calloc(1, sizeof *g);
    if (!g) { fprintf(stderr, "内存不足\n"); return 1; }
    char err[256];
    if (crtr_graph_load(g, graph_path, err, sizeof err) != 0) {
        fprintf(stderr, "加载 %s 失败: %s\n（请先生成：python build_graph.py && "
                "python tools/export_graph.py）\n", graph_path, err);
        return 1;
    }
    printf("图就绪：节点 %d，边 %d，用时 %.1fs\n",
           (int)g->n_nodes, (int)(g->n_edges / 2),
           (crtr_monotonic_ms() - t0) / 1000.0);
    fflush(stdout);

    crtr_threadpool *tp = crtr_tp_create(threads);
    if (!tp) { fprintf(stderr, "线程池创建失败\n"); return 1; }
    printf("线程池：%d 个 worker\n", crtr_tp_threads(tp));

    char webroot[512];
    webroot_from_graph(graph_path, webroot, sizeof webroot);
    crtr_service *svc = crtr_service_create(g, tp, webroot, port);
    if (!svc) { fprintf(stderr, "服务初始化失败\n"); return 1; }

    if (crtr_net_init() != 0) {
        fprintf(stderr, "网络栈初始化失败\n");
        return 1;
    }

    printf("服务已启动: http://127.0.0.1:%d/api/v3/stationRoute?from=IZQ&to=BXP\n",
           port);
    fflush(stdout);

    int rc = crtr_http_serve(tp, port, crtr_service_handle, svc);
    /* 正常情况下 serve 永不返回 */
    crtr_net_cleanup();
    return rc == 0 ? 0 : 1;
}
