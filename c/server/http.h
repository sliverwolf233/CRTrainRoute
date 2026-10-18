#ifndef CRTR_HTTP_H
#define CRTR_HTTP_H

#include <stddef.h>
#include "threadpool.h"

/*
 * 极简 HTTP/1.1 服务层（PLAN §6/§7）
 *
 * - 只实现 GET；keep-alive 由协议版本 + Connection 头协商。
 * - 请求行/头部解析防御式：超长 414/400，套接字收发均带超时。
 * - 响应统一带 Content-Length、CORS（Access-Control-Allow-Origin: *）。
 */

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET crtr_sock;
#define CRTR_BAD_SOCK INVALID_SOCKET
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
typedef int crtr_sock;
#define CRTR_BAD_SOCK (-1)
#endif

#define CRTR_HTTP_MAX_LINE    8192    /* 请求行 / 查询串上限 */
#define CRTR_HTTP_MAX_HEADERS 32768   /* 头部块上限 */
#define CRTR_HTTP_MAX_PATH    2048    /* 目标 URI 上限 */
#define CRTR_HTTP_TIMEOUT_MS  10000   /* 收发超时（兼作 keep-alive 空闲超时） */
#define CRTR_HTTP_MAX_REQS    10000   /* 单连接最大请求数（防泄漏） */

typedef struct {
    char method[8];                        /* 原样（仅支持 GET） */
    char path[CRTR_HTTP_MAX_PATH];         /* 路径部分，未解码 */
    char query[CRTR_HTTP_MAX_LINE];        /* 查询串原始文本，未解码 */
    int  http11;                           /* HTTP/1.1 及以上 */
    int  keep_alive;                       /* 本次请求协商结果 */
} crtr_http_req;

typedef struct crtr_conn {
    crtr_sock fd;
    int  want_close;                       /* 响应后立刻断开 */
} crtr_conn;

typedef void (*crtr_http_handler)(void *ctx, crtr_conn *conn, crtr_http_req *req);

/* Winsock 初始化 / 清理（非 Windows 为空操作）。 */
int  crtr_net_init(void);
void crtr_net_cleanup(void);

/* 监听并派发连接到线程池；只在启动失败时返回非 0。 */
int crtr_http_serve(crtr_threadpool *tp, int port,
                    crtr_http_handler handler, void *ctx);

/* 取查询参数首个值：%XX 解码、'+'->空格、非法 UTF-8 替换为 U+FFFD。
 * 与 Python parse_qs 语义一致：空值（k=）视为不存在。返回 0 即未找到。 */
int crtr_http_query_get(const crtr_http_req *req, const char *name,
                        char *out, size_t outsz);

/* 发送 JSON 响应（Content-Type: application/json; charset=utf-8）。 */
void crtr_http_send_json(crtr_conn *c, int status, const char *body, size_t blen);

/* 发送白名单静态文件；文件缺失时回 404 JSON 并返回 0，成功返回 1。 */
int crtr_http_send_static(crtr_conn *c, const char *webroot,
                          const char *fname, const char *ctype);

#endif
