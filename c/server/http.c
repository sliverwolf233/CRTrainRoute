#include "http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- 套接字 */

int crtr_net_init(void) {
#ifdef _WIN32
    WSADATA d;
    return WSAStartup(MAKEWORD(2, 2), &d) == 0 ? 0 : -1;
#else
    signal(SIGPIPE, SIG_IGN);   /* send 对端断开不杀进程 */
    return 0;
#endif
}

void crtr_net_cleanup(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

static void sock_close(crtr_sock s) {
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

static void sock_set_timeout(crtr_sock s, long ms) {
#ifdef _WIN32
    DWORD v = (DWORD)ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&v, sizeof v);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&v, sizeof v);
#else
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

static int sock_send_all(crtr_sock s, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    while (n > 0) {
#ifdef _WIN32
        int k = send(s, p, (int)n, 0);
        if (k == SOCKET_ERROR || k == 0) return -1;
#else
        ssize_t k = send(s, p, n, MSG_NOSIGNAL);
        if (k <= 0) return -1;
#endif
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

/* ASCII 大小写不敏感前缀比较（避免依赖非标准 strncasecmp） */
static int ci_starts(const char *s, size_t slen, const char *prefix) {
    size_t i = 0;
    for (; prefix[i]; i++) {
        if (i >= slen) return 0;
        char a = s[i], b = prefix[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
        if (a != b) return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------- 动态接收缓冲 */

typedef struct {
    char  *buf;
    size_t len, cap;
} rcvbuf;

static int rcvbuf_reserve(rcvbuf *b, size_t need) {
    if (b->len + need <= b->cap) return 0;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < b->len + need) cap *= 2;
    char *nb = realloc(b->buf, cap);
    if (!nb) return -1;
    b->buf = nb;
    b->cap = cap;
    return 0;
}

/* ---------------------------------------------------------------- URL 解码
 *
 * urllib.parse.parse_qs(..., encoding='utf-8', errors='replace') 语义：
 * '+' -> 空格；%XX 合法十六进制才解码；随后做 UTF-8 校验，非法序列逐
 * 字节替换为 U+FFFD（EF BF BD）。输出保证 NUL 结尾。
 */

static int hexval(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* 解码 %XX 与 '+'，返回写入 dst 的字节数（不含 NUL；dst 满则截断）。 */
static size_t url_decode_raw(const char *s, size_t n, char *dst, size_t dstsz) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '+') { if (o < dstsz) dst[o++] = ' '; }
        else if (c == '%' && i + 2 < n) {
            int h = hexval((unsigned char)s[i + 1]);
            int l = hexval((unsigned char)s[i + 2]);
            if (h >= 0 && l >= 0) {
                if (o < dstsz) dst[o++] = (char)((h << 4) | l);
                i += 2;
            } else if (o < dstsz) dst[o++] = c;
        } else if (o < dstsz) dst[o++] = c;
    }
    return o;
}

/* UTF-8 校验 + 替换；返回写入 dst 的字节数（不含 NUL）。 */
static size_t utf8_replace(const char *s, size_t n, char *dst, size_t dstsz) {
    size_t o = 0, i = 0;
#define PUT(ch) do { if (o < dstsz) dst[o++] = (char)(ch); } while (0)
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        int need = 0;
        unsigned char lo = 0x80, hi = 0xBF;   /* 首个续字节的合法区间 */
        if (c < 0x80) { PUT(c); i++; continue; }
        else if (c >= 0xC2 && c <= 0xDF) { need = 1; }
        else if (c == 0xE0) { need = 2; lo = 0xA0; }              /* 防过短 */
        else if (c >= 0xE1 && c <= 0xEC) { need = 2; }
        else if (c == 0xED) { need = 2; hi = 0x9F; }              /* 防代理区 */
        else if (c >= 0xEE && c <= 0xEF) { need = 2; }
        else if (c == 0xF0) { need = 3; lo = 0x90; }
        else if (c >= 0xF1 && c <= 0xF3) { need = 3; }
        else if (c == 0xF4) { need = 3; hi = 0x8F; }
        else { PUT(0xEF); PUT(0xBF); PUT(0xBD); i++; continue; }  /* 非法首字节 */

        if (i + (size_t)need >= n) {                              /* 序列不完整 */
            PUT(0xEF); PUT(0xBF); PUT(0xBD); i++; continue;
        }
        int bad = 0;
        for (int k = 1; k <= need; k++) {
            unsigned char cc = (unsigned char)s[i + (size_t)k];
            if (k == 1 && (cc < lo || cc > hi)) { bad = 1; break; }
            if (k > 1 && (cc < 0x80 || cc > 0xBF)) { bad = 1; break; }
        }
        if (bad) { PUT(0xEF); PUT(0xBF); PUT(0xBD); i++; continue; }
        for (int k = 0; k <= need; k++) PUT(s[i + (size_t)k]);
        i += (size_t)need + 1;
    }
#undef PUT
    return o;
}

int crtr_http_query_get(const crtr_http_req *req, const char *name,
                        char *out, size_t outsz) {
    if (outsz == 0) return 0;
    out[0] = '\0';
    const char *qs = req->query;
    size_t qslen = strlen(qs);
    size_t pos = 0;

    char raw[CRTR_HTTP_MAX_LINE];
    char key[128];
    while (pos <= qslen) {
        size_t end = pos;
        while (end < qslen && qs[end] != '&') end++;
        size_t eq = pos;
        while (eq < end && qs[eq] != '=') eq++;
        size_t klen = (eq < end) ? (eq - pos) : (end - pos);
        size_t vlen = (eq < end) ? (end - eq - 1) : 0;

        if (klen == 0 || klen >= sizeof key) {
            if (end >= qslen) break;
            pos = end + 1;
            continue;
        }
        size_t kd = url_decode_raw(qs + pos, klen, key, sizeof key - 1);
        key[kd] = '\0';
        if (strcmp(key, name) == 0) {
            if (vlen == 0) return 0;          /* parse_qs 丢弃空值 */
            if (vlen >= sizeof raw) vlen = sizeof raw - 1;
            size_t vd = url_decode_raw(qs + eq + 1, vlen, raw, sizeof raw - 1);
            size_t fo = utf8_replace(raw, vd, out, outsz - 1);
            out[fo] = '\0';
            return 1;
        }
        if (end >= qslen) break;
        pos = end + 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- 请求解析
 *
 * hdr 里是完整头部块（请求行 + 头部，含结尾 CRLFCRLF 的前半）。
 * 返回：1 = 解析成功；-1 = 协议错误（*err_status 给出 400/414）。
 */

static int parse_request(crtr_http_req *req, const char *line, size_t linelen,
                         const char *hdr, size_t hdrlen, int *err_status) {
    memset(req, 0, sizeof *req);

    /* 请求行：METHOD SP TARGET SP VERSION */
    const char *sp1 = memchr(line, ' ', linelen);
    if (!sp1) { *err_status = 400; return -1; }
    size_t mlen = (size_t)(sp1 - line);
    if (mlen == 0 || mlen >= sizeof req->method) { *err_status = 400; return -1; }
    memcpy(req->method, line, mlen);

    const char *rest = sp1 + 1;
    size_t rlen = linelen - mlen - 1;
    const char *sp2 = memchr(rest, ' ', rlen);
    if (!sp2 || rlen - (size_t)(sp2 - rest) - 1 < 8) { *err_status = 400; return -1; }
    size_t tlen = (size_t)(sp2 - rest);
    if (tlen == 0) { *err_status = 400; return -1; }
    if (tlen >= sizeof req->path) { *err_status = 414; return -1; }
    if (rest[0] != '/') { *err_status = 400; return -1; }

    /* 拆出查询串 */
    const char *qm = memchr(rest, '?', tlen);
    size_t plen = qm ? (size_t)(qm - rest) : tlen;
    size_t qlen = qm ? (tlen - plen - 1) : 0;
    if (qlen >= sizeof req->query) { *err_status = 414; return -1; }
    memcpy(req->path, rest, plen);
    req->path[plen] = '\0';
    if (qm) memcpy(req->query, qm + 1, qlen);
    req->query[qlen] = '\0';

    /* 版本：HTTP/1.1 及以上按 keep-alive 处理 */
    req->http11 = (strncmp(sp2 + 1, "HTTP/1.", 7) == 0 && sp2[8] != '0'
                  && sp2[8] >= '1' && sp2[8] <= '9');

    /* 头部：只关心 Connection（大小写不敏感） */
    int hdr_close = 0, hdr_ka = 0;
    size_t pos = 0;
    while (pos < hdrlen) {
        size_t eol = pos;
        while (eol < hdrlen && hdr[eol] != '\n') eol++;
        size_t ll = eol - pos;
        if (eol < hdrlen) eol++;
        if (ll && hdr[pos + ll - 1] == '\r') ll--;
        if (ll > 12 && ci_starts(hdr + pos, ll, "connection:")) {
            const char *v = hdr + pos + 11;
            size_t vl = ll - 11;
            while (vl && (*v == ' ' || *v == '\t')) { v++; vl--; }
            if (vl >= 5 && ci_starts(v, vl, "close")) hdr_close = 1;
            else if (vl >= 10 && ci_starts(v, vl, "keep-alive")) hdr_ka = 1;
        }
        pos = eol;
    }

    req->keep_alive = req->http11 ? !hdr_close : hdr_ka;
    return 1;
}

/* 读取一个完整请求（到头部结束符为止）。
 * 返回：1 = 成功；0 = 对端关闭/超时（静默断开）；-1 = 协议错误。 */
static int conn_read_request(crtr_conn *c, rcvbuf *b, crtr_http_req *req,
                             int *err_status) {
    b->len = 0;
    *err_status = 0;
    for (;;) {
        if (b->len >= 4) {
            for (size_t i = 3; i < b->len; i++) {
                if (b->buf[i - 3] == '\r' && b->buf[i - 2] == '\n' &&
                    b->buf[i - 1] == '\r' && b->buf[i] == '\n') {
                    /* 请求行 = 头部块的第一行 */
                    size_t rl = 0;
                    while (rl < b->len && b->buf[rl] != '\n') rl++;
                    if (rl == 0 || rl >= CRTR_HTTP_MAX_LINE ||
                        b->buf[rl - 1] != '\r') {
                        *err_status = (rl >= CRTR_HTTP_MAX_LINE) ? 414 : 400;
                        return -1;
                    }
                    b->buf[rl - 1] = '\0';      /* 临时截断出请求行字符串 */
                    int rc = parse_request(req, b->buf, rl - 1,
                                           b->buf, b->len, err_status);
                    b->buf[rl - 1] = '\r';      /* 还原 */
                    return rc;
                }
            }
        }
        if (b->len >= CRTR_HTTP_MAX_HEADERS) { *err_status = 400; return -1; }
        if (rcvbuf_reserve(b, 8192) != 0) { *err_status = 500; return -1; }

#ifdef _WIN32
        int k = recv(c->fd, b->buf + b->len, (int)(b->cap - b->len), 0);
        if (k == 0 || k == SOCKET_ERROR) return 0;   /* 关闭或超时：静默断开 */
        b->len += (size_t)k;
#else
        ssize_t k = recv(c->fd, b->buf + b->len, b->cap - b->len, 0);
        if (k <= 0) return 0;
        b->len += (size_t)k;
#endif
    }
}

/* ---------------------------------------------------------------- 响应 */

static const char *status_reason(int s) {
    switch (s) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 414: return "URI Too Long";
    case 422: return "Unprocessable Entity";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    default:  return "OK";
    }
}

static void send_response(crtr_conn *c, int status, const char *ctype,
                          const void *body, size_t blen, int no_cache) {
    char hdr[512];
    int hn = snprintf(hdr, sizeof hdr,
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %llu\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "%s"
        "Connection: %s\r\n\r\n",
        status, status_reason(status), ctype,
        (unsigned long long)blen,
        no_cache ? "Cache-Control: no-cache\r\n" : "",
        c->want_close ? "close" : "keep-alive");
    if (hn < 0 || (size_t)hn >= sizeof hdr) { c->want_close = 1; return; }
    if (sock_send_all(c->fd, hdr, (size_t)hn) != 0 ||
        sock_send_all(c->fd, body, blen) != 0)
        c->want_close = 1;
}

void crtr_http_send_json(crtr_conn *c, int status, const char *body, size_t blen) {
    send_response(c, status, "application/json; charset=utf-8", body, blen, 0);
}

int crtr_http_send_static(crtr_conn *c, const char *webroot,
                          const char *fname, const char *ctype) {
    char full[1024];
    size_t rl = strlen(webroot);
    int sep = (rl > 0 && webroot[rl - 1] != '/' && webroot[rl - 1] != '\\');
    if (snprintf(full, sizeof full, "%s%s%s", webroot, sep ? "/" : "", fname)
        >= (int)sizeof full) {
        c->want_close = 1;
        return 0;
    }

    FILE *f = fopen(full, "rb");
    if (!f) {
        char msg[256];
        int n = snprintf(msg, sizeof msg,
            "{\n  \"ok\": false,\n  \"error\": \"文件不存在: %s\"\n}", fname);
        if (n > 0) crtr_http_send_json(c, 404, msg, (size_t)n);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); c->want_close = 1; return 0; }
    char *data = malloc((size_t)sz + 1);
    if (!data) { fclose(f); c->want_close = 1; return 0; }
    size_t got = fread(data, 1, (size_t)sz, f);
    fclose(f);
    send_response(c, 200, ctype, data, got, 1);
    free(data);
    return 1;
}

/* ---------------------------------------------------------------- 连接任务 */

static const char METHOD_ERR_JSON[] =
    "{\n  \"ok\": false,\n  \"error\": \"仅支持 GET\"\n}";

typedef struct {
    crtr_conn         conn;
    crtr_http_handler handler;
    void             *ctx;
} conn_task;

static void conn_task_run(void *arg) {
    conn_task *t = (conn_task *)arg;
    crtr_conn *c = &t->conn;
    rcvbuf b = { NULL, 0, 0 };

    for (int served = 0; served < CRTR_HTTP_MAX_REQS; served++) {
        crtr_http_req req;
        int err = 0;
        int rc = conn_read_request(c, &b, &req, &err);
        if (rc == 0) break;                       /* 对端关闭 / 超时 */
        if (rc < 0) {
            /* 协议错误：尽力回一个 JSON 错误再断开 */
            if (err == 501) {
                crtr_http_send_json(c, 501, METHOD_ERR_JSON,
                                    sizeof METHOD_ERR_JSON - 1);
            } else {
                char msg[160];
                int n = snprintf(msg, sizeof msg,
                    "{\n  \"ok\": false,\n  \"error\": \"请求格式错误 (%d)\"\n}", err);
                if (n > 0) crtr_http_send_json(c, err, msg, (size_t)n);
            }
            break;
        }
        if (strcmp(req.method, "GET") != 0) {
            c->want_close = 1;
            crtr_http_send_json(c, 501, METHOD_ERR_JSON, sizeof METHOD_ERR_JSON - 1);
            break;
        }
        t->handler(t->ctx, c, &req);
        if (c->want_close || !req.keep_alive) break;
    }

    sock_close(c->fd);
    free(b.buf);
    free(t);
}

/* ---------------------------------------------------------------- 监听循环 */

int crtr_http_serve(crtr_threadpool *tp, int port,
                    crtr_http_handler handler, void *ctx) {
    crtr_sock ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls == CRTR_BAD_SOCK) return -1;

    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons((unsigned short)port);
    if (bind(ls, (struct sockaddr *)&sa, sizeof sa) != 0) {
        sock_close(ls);
        return -1;
    }
    if (listen(ls, 128) != 0) {
        sock_close(ls);
        return -1;
    }

    for (;;) {
        struct sockaddr_in ca;
        socklen_t clen = (socklen_t)sizeof ca;
        crtr_sock cs = accept(ls, (struct sockaddr *)&ca, &clen);
        if (cs == CRTR_BAD_SOCK) continue;        /* 瞬时错误：继续 accept */

        sock_set_timeout(cs, CRTR_HTTP_TIMEOUT_MS);

        conn_task *t = malloc(sizeof *t);
        if (!t) { sock_close(cs); continue; }
        t->conn.fd = cs;
        t->conn.want_close = 0;
        t->handler = handler;
        t->ctx = ctx;
        crtr_tp_submit(tp, conn_task_run, t);
    }
}
