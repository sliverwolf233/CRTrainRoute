#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
性能基准：python server.py（参照实现） vs C 版 crtr_server。

同一批 30+ 查询，对比三个维度：
  1. 冷启动：进程启动 -> /health 就绪耗时，以及就绪后的首查时延
     （广州南->北京西，PLAN §1 目标 C 版 < 50ms）；
  2. 热时延：预热（全部查询跑一遍）后，每查询两轮的均值 / P95；
  3. 并发吞吐：8 线程固定时长循环请求，统计 QPS 与均值时延。

两个服务由本脚本自行拉起与终止（冷启动需要独占端口），运行前请确保
8889 / 8890 端口空闲（可用 --py-port / --c-port 改端口）。

输出 Markdown 表到 stdout；--write README_C.md 时写入
<!-- bench:begin --> / <!-- bench:end --> 标记之间（可重复刷新）。

用法：
  python tests/bench_compare.py
  python tests/bench_compare.py --write README_C.md
  python tests/bench_compare.py --skip-cold          # 只测热态（需已自启服务? 否）
"""
import argparse
import json
import os
import shutil
import signal
import statistics
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

BASE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# 基准查询池：按线路走廊串成的长链 + 若干跨区干线和 km 规划。
# 代码有效性在运行时对 data/station_names.json 再过滤一遍，缺码自动剔除。
STATION_POOL = [
    'IZQ', 'BXP', 'AOH', 'NKH', 'HGH', 'IOQ', 'SEQ', 'VUQ',
    'WHN', 'CSQ', 'ZZF', 'SJP', 'JGK', 'BBH', 'TJP', 'BJP',
    'CDW', 'CQW', 'KMM', 'GYW', 'XAY', 'LAJ', 'XNO', 'YIJ',
    'LZZ', 'GLZ', 'FZS', 'XMS', 'NCG', 'HFH', 'SYT', 'TYV',
    'HHC', 'GGQ', 'LAJ',
]
EXTRA_PAIRS = [
    ('IZQ', 'BXP'), ('BXP', 'AOH'), ('AOH', 'CDW'), ('IZQ', 'IOQ'),
    ('BXP', 'KMM'), ('AOH', 'XAY'), ('IZQ', 'VUQ'), ('BXP', 'SYT'),
]


def build_queries():
    """生成 (from, to, km) 查询列表，过滤无效电报码。"""
    names_path = os.path.join(BASE_DIR, 'data', 'station_names.json')
    nm = json.load(open(names_path, encoding='utf-8'))
    pool = [c for c in STATION_POOL if c in nm]
    queries = []
    for a, b in zip(pool, pool[1:]):
        queries.append((a, b, None))
    for a, b in EXTRA_PAIRS:
        if a in nm and b in nm:
            queries.append((a, b, None))
    # 三个指定里程规划（计入热时延，不计入并发吞吐）
    queries.append(('IZQ', 'BXP', 2800))
    queries.append(('BXP', 'AOH', 1600))
    queries.append(('AOH', 'BXP', 2600))
    return queries


def route_url(frm, to, km=None):
    q = {'from': frm, 'to': to}
    if km is not None:
        q['km'] = km
    return '/api/v3/stationRoute?' + urllib.parse.urlencode(q)


def http_get(base, path, timeout=90):
    """GET，返回 (状态码, 耗时ms)；异常时状态码为 0。"""
    t0 = time.perf_counter()
    try:
        with urllib.request.urlopen(base + path, timeout=timeout) as r:
            r.read()
            return r.status, (time.perf_counter() - t0) * 1000.0
    except urllib.error.HTTPError as e:
        e.read()
        return e.code, (time.perf_counter() - t0) * 1000.0
    except Exception:
        return 0, (time.perf_counter() - t0) * 1000.0


def wait_health(base, deadline_s=120.0, interval=0.2):
    """轮询 /health 直到就绪，返回耗时秒；超时返回 None。"""
    t0 = time.perf_counter()
    while time.perf_counter() - t0 < deadline_s:
        code, _ = http_get(base, '/health', timeout=3)
        if code == 200:
            return time.perf_counter() - t0
        time.sleep(interval)
    return None


# ---------------------------------------------------------------- 服务管理

class Server:
    """被测服务进程：拉起 -> 就绪 -> 测试 -> 终止。"""

    def __init__(self, name, cmd, port, cwd=None):
        self.name = name
        self.cmd = cmd
        self.port = port
        self.cwd = cwd or BASE_DIR
        self.base = 'http://127.0.0.1:%d' % port
        self.proc = None

    def start(self):
        kwargs = {'cwd': self.cwd}
        if os.name == 'nt':
            kwargs['creationflags'] = subprocess.CREATE_NEW_PROCESS_GROUP
        self.proc = subprocess.Popen(self.cmd, **kwargs)
        return self.proc

    def stop(self):
        if self.proc is None:
            return
        try:
            if os.name == 'nt':
                self.proc.send_signal(signal.CTRL_BREAK_EVENT)
                try:
                    self.proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.proc.terminate()
            else:
                self.proc.terminate()
            self.proc.wait(timeout=5)
        except Exception:
            pass
        # 兜底：杀掉仍占端口的进程交给操作系统（端口自然释放）
        self.proc = None


# ---------------------------------------------------------------- 基准各阶段

def bench_one(server, queries, args):
    """对单个服务跑完整基准，返回指标字典。"""
    res = {'name': server.name, 'port': server.port}
    print('\n===== %s (port %d) =====' % (server.name, server.port))

    # 端口已被占用时，拉起的新进程会失败而轮询打到旧服务上，先拒绝
    code, _ = http_get(server.base, '/health', timeout=2)
    if code == 200:
        print('  [x] 端口 %d 已有服务在响应，先停掉它再跑冷启动基准' % server.port)
        return None

    proc = server.start()
    ready = wait_health(server.base, deadline_s=args.cold_timeout)
    if ready is None:
        print('  [x] 服务 %.1fs 内未就绪，跳过该服务' % args.cold_timeout)
        server.stop()
        return None
    res['ready_s'] = ready
    print('  就绪耗时 %.2fs (pid %s)' % (ready, proc.pid))

    # 冷态首查（广州南 -> 北京西，目标 < 50ms）
    code, ms = http_get(server.base, route_url('IZQ', 'BXP'))
    res['first_ms'] = ms
    res['first_ok'] = (code == 200)
    print('  首查 HTTP %d: %.1fms' % (code, ms))

    # 预热：全部查询跑一遍（填满路由缓存）
    for frm, to, km in queries:
        http_get(server.base, route_url(frm, to, km))

    # 热时延：两轮取全部样本
    samples = []
    for _round in range(2):
        for frm, to, km in queries:
            code, ms = http_get(server.base, route_url(frm, to, km))
            if code == 200:
                samples.append(ms)
    samples.sort()
    res['hot_n'] = len(samples)
    res['hot_mean'] = statistics.mean(samples) if samples else float('nan')
    res['hot_p50'] = samples[len(samples) // 2] if samples else float('nan')
    res['hot_p95'] = samples[min(len(samples) - 1, int(len(samples) * 0.95))] if samples else float('nan')
    res['hot_max'] = samples[-1] if samples else float('nan')
    print('  热查 %d 样本: mean %.1fms p95 %.1fms max %.1fms'
          % (res['hot_n'], res['hot_mean'], res['hot_p95'], res['hot_max']))

    # 并发吞吐：N 线程固定时长循环（仅普通查询）
    plain = [(f, t) for f, t, km in queries if km is None]
    lat = []
    lock = threading.Lock()
    stop_flag = threading.Event()

    def worker():
        local = []
        i = 0
        while not stop_flag.is_set():
            frm, to = plain[i % len(plain)]
            i += 1
            code, ms = http_get(server.base, route_url(frm, to), timeout=30)
            if code == 200:
                local.append(ms)
        with lock:
            lat.extend(local)

    threads = [threading.Thread(target=worker) for _ in range(args.threads)]
    t0 = time.perf_counter()
    for t in threads:
        t.start()
    time.sleep(args.duration)
    stop_flag.set()
    for t in threads:
        t.join()
    elapsed = time.perf_counter() - t0
    res['qps'] = len(lat) / elapsed if elapsed > 0 else 0.0
    res['conc_mean'] = statistics.mean(lat) if lat else float('nan')
    res['conc_n'] = len(lat)
    res['threads'] = args.threads
    print('  并发 %d 线程 %.0fs: %d 请求, QPS %.1f, 均值 %.1fms'
          % (args.threads, args.duration, len(lat), res['qps'], res['conc_mean']))

    server.stop()
    return res


# ---------------------------------------------------------------- 报告

def fmt(v, unit='', prec=1):
    if v is None:
        return '-'
    try:
        return ('%.' + str(prec) + 'f%s') % (v, unit)
    except (TypeError, ValueError):
        return str(v)


def render_table(rows, meta):
    """两个服务的结果 -> Markdown 对照表。rows[0]=Python，rows[1]=C。
    倍率列统一表示「C 相对 Python 的优势」：时延为 Py/C，QPS 为 C/Py。"""
    a, b = rows[0], rows[1] if len(rows) > 1 else None

    def speedup_lat(av, bv):
        """时延优势倍数 = av/bv（>1 表示 C 更快）。"""
        if not av or not bv:
            return '-'
        try:
            return '%.1fx' % (av / bv)
        except (TypeError, ZeroDivisionError):
            return '-'

    def speedup_qps(av, bv):
        """吞吐优势倍数 = bv/av。"""
        if not av or not bv:
            return '-'
        try:
            return '%.1fx' % (bv / av)
        except (TypeError, ZeroDivisionError):
            return '-'

    lines = []
    lines.append('| 指标 | %s | %s | C 优势 |' % (a['name'], b['name'] if b else '-'))
    lines.append('|---|---|---|---|')
    lines.append('| 冷启动就绪 | %s | %s | %s |'
                 % (fmt(a.get('ready_s'), 's', 2), fmt(b.get('ready_s'), 's', 2) if b else '-',
                    speedup_lat(a.get('ready_s'), b.get('ready_s')) if b else '-'))
    lines.append('| 首查时延 IZQ→BXP | %s | %s | %s |'
                 % (fmt(a.get('first_ms'), 'ms'), fmt(b.get('first_ms'), 'ms') if b else '-',
                    speedup_lat(a.get('first_ms'), b.get('first_ms')) if b else '-'))
    lines.append('| 热查均值 | %s | %s | %s |'
                 % (fmt(a.get('hot_mean'), 'ms'), fmt(b.get('hot_mean'), 'ms') if b else '-',
                    speedup_lat(a.get('hot_mean'), b.get('hot_mean')) if b else '-'))
    lines.append('| 热查 P95 | %s | %s | %s |'
                 % (fmt(a.get('hot_p95'), 'ms'), fmt(b.get('hot_p95'), 'ms') if b else '-',
                    speedup_lat(a.get('hot_p95'), b.get('hot_p95')) if b else '-'))
    lines.append('| 热查最大 | %s | %s | %s |'
                 % (fmt(a.get('hot_max'), 'ms'), fmt(b.get('hot_max'), 'ms') if b else '-',
                    speedup_lat(a.get('hot_max'), b.get('hot_max')) if b else '-'))
    nt = a.get('threads', 8)
    lines.append('| %d 并发 QPS | %s | %s | %s |'
                 % (nt, fmt(a.get('qps'), '', 1), fmt(b.get('qps'), '', 1) if b else '-',
                    speedup_qps(a.get('qps'), b.get('qps')) if b else '-'))
    lines.append('| %d 并发均值时延 | %s | %s | %s |'
                 % (nt, fmt(a.get('conc_mean'), 'ms'), fmt(b.get('conc_mean'), 'ms') if b else '-',
                    speedup_lat(a.get('conc_mean'), b.get('conc_mean')) if b else '-'))
    lines.append('')
    lines.append('样本：热查 %s / %s 条，并发 %s / %s 请求；%s'
                 % (a.get('hot_n'), b.get('hot_n') if b else '-',
                    a.get('conc_n'), b.get('conc_n') if b else '-', meta))
    return '\n'.join(lines)


BENCH_BEGIN = '<!-- bench:begin -->'
BENCH_END = '<!-- bench:end -->'


def write_readme_section(path, table):
    """把表写入 README 标记区间（不存在则追加到末尾）。"""
    if os.path.exists(path):
        text = open(path, encoding='utf-8').read()
    else:
        text = ''
    block = BENCH_BEGIN + '\n\n' + table + '\n\n' + BENCH_END
    if BENCH_BEGIN in text and BENCH_END in text:
        pre = text.split(BENCH_BEGIN)[0]
        post = text.split(BENCH_END)[-1]
        text = pre + block + post
    else:
        text = text.rstrip('\n') + '\n\n## 基准结果\n\n' + block + '\n'
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)


# ---------------------------------------------------------------- 入口

def main():
    ap = argparse.ArgumentParser(description='python server.py 与 crtr_server 性能对比')
    ap.add_argument('--py-cmd', default=sys.executable + ' server.py',
                    help='Python 参照服务启动命令')
    ap.add_argument('--py-port', type=int, default=8889)
    ap.add_argument('--c-bin', default=None,
                    help='crtr_server 路径（默认自动探测 c/build-par / c/build-core）')
    ap.add_argument('--c-port', type=int, default=8890)
    ap.add_argument('--threads', type=int, default=8, help='并发线程数')
    ap.add_argument('--duration', type=float, default=15.0, help='并发压测时长（秒）')
    ap.add_argument('--cold-timeout', type=float, default=120.0)
    ap.add_argument('--write', metavar='README_C.md', default=None,
                    help='把结果表写入该 Markdown 文件的标记区间')
    ap.add_argument('--only', choices=('py', 'c'), default=None,
                    help='只测其中一个服务（不生成对照表）')
    args = ap.parse_args()

    queries = build_queries()
    print('基准查询 %d 条（含 %d 条 km 规划）'
          % (len(queries), sum(1 for q in queries if q[2] is not None)))
    if len(queries) < 30:
        print('[!] 查询数不足 30，请检查 STATION_POOL', file=sys.stderr)

    servers = []
    if args.only in (None, 'py'):
        servers.append(Server('Python server.py', args.py_cmd, args.py_port))
    if args.only in (None, 'c'):
        c_bin = args.c_bin
        if c_bin is None:
            for cand in ('c/build-par/crtr_server.exe', 'c/build-par/crtr_server',
                         'c/build-core/crtr_server.exe', 'c/build-core/crtr_server'):
                p = os.path.join(BASE_DIR, cand)
                if os.path.exists(p):
                    c_bin = p
                    break
        if c_bin is None or not os.path.exists(c_bin):
            print('[x] 未找到 crtr_server（--c-bin 指定路径，或先完成 C 构建）', file=sys.stderr)
            return 1
        servers.append(Server('C crtr_server',
                              [c_bin, '--graph', os.path.join(BASE_DIR, 'data', 'rail_graph.bin'),
                               '--port', str(args.c_port), '--threads', str(args.threads)],
                              args.c_port))

    rows = []
    for s in servers:
        r = bench_one(s, queries, args)
        if r:
            rows.append(r)

    if not rows:
        print('[x] 无可用结果', file=sys.stderr)
        return 1
    if args.only is None and len(rows) < 2:
        # 对照模式但有服务失败：结果不完整，拒绝写 README
        print('[x] 对照不完整（仅 %d 个服务成功），不写入结果' % len(rows), file=sys.stderr)
        return 1

    meta = time.strftime('%Y-%m-%d %H:%M') + '，%d 线程并发 %.0fs' % (args.threads, args.duration)
    if len(rows) >= 2:
        table = render_table(rows, meta)
    else:
        r = rows[0]
        table = ('| 指标 | %s |\n|---|---|\n| 冷启动就绪 | %s |\n| 首查时延 | %s |\n'
                 '| 热查均值 | %s |\n| 热查 P95 | %s |\n| %d 并发 QPS | %s |\n'
                 '| %d 并发均值时延 | %s |\n'
                 % (r['name'], fmt(r.get('ready_s'), 's', 2), fmt(r.get('first_ms'), 'ms'),
                    fmt(r.get('hot_mean'), 'ms'), fmt(r.get('hot_p95'), 'ms'),
                    r.get('threads', 8), fmt(r.get('qps')),
                    r.get('threads', 8), fmt(r.get('conc_mean'), 'ms')))
        table += '\n' + meta
    print('\n' + '=' * 70)
    print(table)
    print('=' * 70)

    if args.write:
        write_readme_section(args.write, table)
        print('\n已写入 %s' % args.write)
    return 0


if __name__ == '__main__':
    sys.exit(main())
