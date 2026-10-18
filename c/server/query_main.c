/* crtr_query —— 命令行单查询（基准测试用）
 *
 * 用法：crtr_query --from IZQ --to BXP [--km 2800] [--json] [--graph <path>]
 *   --json  原样输出与 HTTP 接口一致的 JSON 响应体
 * 默认输出人类可读摘要；退出码 0 = 查询成功。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "threadpool.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

/* Windows 控制台 argv 按 ANSI 代码页传入；统一转成 UTF-8 再解析，
 * 使中文站名与含中文的路径在任意代码页下可用。 */
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
    fputs("crtr_query —— 单次算路查询\n"
          "\n"
          "用法: crtr_query --from <站> --to <站> [选项]\n"
          "  --from <code|站名>  起点（电报码或中文名）\n"
          "  --to   <code|站名>  终点\n"
          "  --km <n>            指定目标里程（公里）\n"
          "  --json              输出完整 JSON 响应（同 HTTP 接口）\n"
          "  --graph <path>      二进制图路径（默认 data/rail_graph.bin）\n"
          "  --help, -h          显示本说明\n",
          out);
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
    const char *from = NULL, *to = NULL, *graph_path = "data/rail_graph.bin";
    double km = 0.0;
    int has_km = 0, as_json = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(argv[i], "--from") == 0 && i + 1 < argc) {
            from = argv[++i];
        } else if (strcmp(argv[i], "--to") == 0 && i + 1 < argc) {
            to = argv[++i];
        } else if (strcmp(argv[i], "--km") == 0 && i + 1 < argc) {
            has_km = 1;
            km = atof(argv[++i]);
        } else if (strcmp(argv[i], "--json") == 0) {
            as_json = 1;
        } else if (strcmp(argv[i], "--graph") == 0 && i + 1 < argc) {
            graph_path = argv[++i];
        } else {
            fprintf(stderr, "未知参数: %s\n", argv[i]);
            usage(stderr);
            return 1;
        }
    }
    if (!from || !to) {
        usage(stderr);
        return 1;
    }

    crtr_graph *g = calloc(1, sizeof *g);
    if (!g) return 1;
    char err[256];
    if (crtr_graph_load(g, graph_path, err, sizeof err) != 0) {
        fprintf(stderr, "加载 %s 失败: %s\n", graph_path, err);
        return 1;
    }

    crtr_threadpool *tp = crtr_tp_create(0);
    crtr_service *svc = crtr_service_create(g, tp, ".", 0);
    if (!svc) { fprintf(stderr, "服务初始化失败\n"); return 1; }

    double t0 = crtr_monotonic_ms();
    crtr_route_response resp;
    int ok = crtr_service_route(svc, from, to, has_km, km, &resp);
    double wall = crtr_monotonic_ms() - t0;

    int exit_code = 0;
    if (as_json) {
        crtr_jsonw jw;
        crtr_jw_init(&jw);
        crtr_route_response_json(&resp, &jw);
        const char *body = crtr_jw_finish(&jw);
        fwrite(body, 1, jw.len, stdout);
        fputc('\n', stdout);
        crtr_jw_free(&jw);
    } else if (ok) {
        printf("%s(%s) -> %s(%s)\n",
               resp.ep_from.name ? resp.ep_from.name : "?", resp.ep_from.code,
               resp.ep_to.name ? resp.ep_to.name : "?", resp.ep_to.code);
        printf("里程: %.3f km（%.0f m）\n",
               resp.rr.length / 1000.0, resp.rr.length);
        printf("骨架节点: %d，几何点: %d，途经车站: %d\n",
               resp.rr.n_nodes, resp.rr.n_coords, resp.rr.n_passed);
        printf("吸附偏移: 起点 %.1f m / 终点 %.1f m\n",
               resp.ep_from.snap_offset_m, resp.ep_to.snap_offset_m);
        if (resp.has_km) {
            printf("里程规划: 请求 %.1f km，实际 %.2f km，偏差 %.2f km，%s\n",
                   resp.km_requested, resp.km_actual, resp.km_diff,
                   resp.km_reached ? "达标" : "未达标");
        }
        if (resp.rr.n_ferries > 0) {
            double s = 0;
            for (int i = 0; i < resp.rr.n_ferries; i++) s += resp.rr.ferries[i].km;
            printf("轮渡: %d 段，共 %.2f km\n", resp.rr.n_ferries, s);
        }
        printf("服务耗时: %ld ms\n", resp.elapsed_ms);
        printf("总耗时: %.1f ms\n", wall);
    } else {
        fprintf(stderr, "查询失败（HTTP %d）: %s\n", resp.http_status, resp.error);
        if (resp.has_hint) fprintf(stderr, "提示: %s\n", resp.hint);
        exit_code = 1;
    }

    crtr_service_release_held(svc, &resp);
    crtr_route_response_free(&resp);
    crtr_service_destroy(svc);
    crtr_tp_destroy(tp);
    return exit_code;
}
