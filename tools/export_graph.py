#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把 data/rail_graph.pkl 导出为 C 直读二进制 data/rail_graph.bin（v1，小端）。

格式权威定义见 c/PLAN.md §4。要点：
  - pkl 中的节点 id 是建图期的密集编号（含被精简掉的度-2 中间点）；
    bin 只保留骨架节点，导出时建立 密集 id -> 压缩索引 映射。
  - pkl edge_geom 的链存的是密集节点 id，导出时映射为坐标（f64 lat/lon 交错）。
  - pkl station_nodes 的值是密集节点 id，导出时映射为压缩索引。

用法：
  python tools/export_graph.py             # 导出 + 读回结构校验 + 抽样校验
  python tools/export_graph.py --no-verify
  python tools/export_graph.py --sample 500
"""
import argparse
import array
import json
import math
import os
import pickle
import random
import struct
import sys

BASE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(BASE, 'data')

MAGIC = b'CRTR'
VERSION = 1
EARTH_R = 6371008.8
STR_MAX = 0xFFFF          # str 长度字段为 u16

SECTION_ORDER = ('header', 'node_lat', 'node_lon', 'node_comp',
                 'csr_head', 'csr_eto', 'csr_ew', 'csr_egeom',
                 'geom_count', 'geom_pts',
                 'stations', 'names', 'lanes', 'ferry')


# ---------------------------------------------------------------- 基础工具

def hav(a1, o1, a2, o2):
    """大圆距离（米），与 server.py haversine 逐行一致。"""
    p1, p2 = math.radians(a1), math.radians(a2)
    dp = p2 - p1
    dl = math.radians(o2 - o1)
    x = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * EARTH_R * math.asin(math.sqrt(x))


def _arr(typecode, values):
    a = array.array(typecode, values)
    if sys.byteorder != 'little':
        a.byteswap()
    return a.tobytes()


def arr_f64(values):
    return _arr('d', values)


def arr_i32(values):
    return _arr('i', values)


def arr_u32(values):
    return _arr('I', values)


def enc_str(s):
    """str = u16 字节长度 + UTF-8 字节 + 一个 NUL。None 视为空串。"""
    b = (s or '').encode('utf-8')
    if len(b) > STR_MAX:
        raise ValueError('字符串超过 u16 长度上限: %d bytes' % len(b))
    return struct.pack('<H', len(b)) + b + b'\x00'


# ---------------------------------------------------------------- 导出

def export(g, stations_json, names_json, lanes_json, out_path, verbose=True):
    lats, lons = g['lats'], g['lons']
    adj = g['adj']
    edge_geom = g['edge_geom']
    station_nodes = g.get('station_nodes', {})
    stations_meta = g.get('stations', {})

    # ---- 压缩索引：密集节点 id -> 0..N-1（升序，保证确定性） ----
    nodes = sorted(adj.keys())
    pos = {n: i for i, n in enumerate(nodes)}
    N = len(nodes)
    E = sum(len(v) for v in adj.values())

    # ---- CSR 邻接（邻居按压缩索引升序，确定性） ----
    head = [0] * (N + 1)
    for i, n in enumerate(nodes):
        head[i + 1] = head[i] + len(adj[n])
    eto = [0] * E
    ew = [0.0] * E
    k = 0
    for i, n in enumerate(nodes):
        for m, w in sorted(adj[n].items(), key=lambda kv: pos[kv[0]]):
            eto[k] = pos[m]
            ew[k] = w
            k += 1
    assert k == E

    # ---- 几何表（无向条目）：链的密集节点 id 映射为坐标 ----
    # 链键在 pkl 中按密集 id 存了双向；按 PLAN §4 取 (a<b) 方向的链。
    egeom = [-1] * E
    gcount = []
    gpts = array.array('d')
    geom_id_of = {}                       # (min(i,j), max(i,j)) 压缩索引对 -> 条目
    for i in range(N):
        for kk in range(head[i], head[i + 1]):
            j = eto[kk]
            if j < i:
                continue                 # 只处理一次无向边
            da, db = nodes[i], nodes[j]
            chain = edge_geom.get((da, db))
            if chain is None:
                rev = edge_geom.get((db, da))
                chain = None if rev is None else rev[::-1]
            if chain:
                gid = len(gcount)
                gcount.append(len(chain))
                for nd in chain:
                    gpts.append(lats[nd])
                    gpts.append(lons[nd])
                geom_id_of[(i, j)] = gid
            else:
                geom_id_of[(i, j)] = -1   # 无中间点：直线（含轮渡边）
    for i in range(N):
        for kk in range(head[i], head[i + 1]):
            j = eto[kk]
            key = (i, j) if i < j else (j, i)
            egeom[kk] = geom_id_of[key]
    G = len(gcount)
    n_chain_pts = len(gpts) // 2

    # ---- 车站（来自 stations.json，按电报码升序；node 映射为压缩索引） ----
    st_rows = []
    for code in sorted(stations_json):
        s = stations_json[code]
        if s.get('lat') is None or s.get('lon') is None:
            continue
        meta = stations_meta.get(code) or {}
        nid = station_nodes.get(code)
        node = pos[nid] if nid is not None else -1
        st_rows.append((code, node, float(meta.get('snap_m') or 0.0),
                        float(s['lat']), float(s['lon']),
                        s.get('name') or '', s.get('city') or '', s.get('addr') or ''))
    S = len(st_rows)

    # ---- 站名表（station_names.json 全量，保留 JSON 文件序） ----
    # searchStation 的同级内部顺序与 Python 版一致（dict 迭代 = 文件序）。
    nm_rows = [(c, names_json[c] or '') for c in names_json]
    K = len(nm_rows)

    # ---- 同址异名标注（station_lanes.json 的 stations 段，按电报码升序） ----
    ln_rows = []
    for code in sorted(lanes_json):
        v = lanes_json[code] or {}
        ln_rows.append((code, v.get('name') or '', v.get('laneType') or '',
                        v.get('aliasOf') or '', v.get('note') or ''))
    L = len(ln_rows)

    # ---- 轮渡（pkl ferry_edges；坐标 [lon, lat] 已 round 6，换算微度整数） ----
    ferries = []
    for fe in g.get('ferry_edges', []):
        flon, flat = fe['from']
        tlon, tlat = fe['to']
        names = list(fe.get('stations') or [])
        if len(names) > 255:              # n_names 为 u8
            names = names[:255]
        ferries.append((int(round(flat * 1e6)), int(round(flon * 1e6)),
                        int(round(tlat * 1e6)), int(round(tlon * 1e6)),
                        float(fe['km']), names))
    F = len(ferries)

    big_comp = int(g.get('big_comp') or 0)

    # ---- 组装各段 ----
    sec = {}
    sec['header'] = struct.pack('<4sIIIIIIIIIi',
                                MAGIC, VERSION, N, E, G, S, K, L, F, 0, big_comp)
    sec['node_lat'] = arr_f64(lats[n] for n in nodes)
    sec['node_lon'] = arr_f64(lons[n] for n in nodes)
    comp_dict = g.get('comp') or {}
    sec['node_comp'] = arr_i32((comp_dict.get(n, 0) for n in nodes))
    sec['csr_head'] = arr_i32(head)
    sec['csr_eto'] = arr_i32(eto)
    sec['csr_ew'] = arr_f64(ew)
    sec['csr_egeom'] = arr_i32(egeom)
    sec['geom_count'] = arr_u32(gcount)
    sec['geom_pts'] = arr_f64(gpts)

    b = bytearray()
    for code, node, snap, lat, lon, name, city, addr in st_rows:
        b += enc_str(code)
        b += struct.pack('<iddd', node, snap, lat, lon)
        b += enc_str(name) + enc_str(city) + enc_str(addr)
    sec['stations'] = bytes(b)

    b = bytearray()
    for code, name in nm_rows:
        b += enc_str(code) + enc_str(name)
    sec['names'] = bytes(b)

    b = bytearray()
    for code, name, ltype, alias, note in ln_rows:
        b += (enc_str(code) + enc_str(name) + enc_str(ltype)
              + enc_str(alias) + enc_str(note))
    sec['lanes'] = bytes(b)

    b = bytearray()
    for flat_u6, flon_u6, tlat_u6, tlon_u6, km, names in ferries:
        b += struct.pack('<qqqqd', flat_u6, flon_u6, tlat_u6, tlon_u6, km)
        b += struct.pack('<B', len(names))
        for nm in names:
            b += enc_str(nm)
    sec['ferry'] = bytes(b)

    with open(out_path, 'wb') as f:
        for name in SECTION_ORDER:
            f.write(sec[name])

    if verbose:
        print('已写出 %s (%.1f MB)' % (out_path, os.path.getsize(out_path) / 1e6))
        print('各段统计：')
        rows = [
            ('头部', len(sec['header']), 'N=%d E=%d G=%d' % (N, E, G)),
            ('节点 lat/lon/comp', len(sec['node_lat']) + len(sec['node_lon']) + len(sec['node_comp']),
             'N=%d' % N),
            ('CSR head', len(sec['csr_head']), 'N+1'),
            ('CSR eto', len(sec['csr_eto']), 'E=%d' % E),
            ('CSR ew', len(sec['csr_ew']), 'E'),
            ('CSR egeom', len(sec['csr_egeom']), 'E'),
            ('几何 gcount', len(sec['geom_count']), 'G=%d' % G),
            ('几何点列', len(sec['geom_pts']), '%d 点' % n_chain_pts),
            ('车站', len(sec['stations']), 'S=%d' % S),
            ('站名表', len(sec['names']), 'K=%d' % K),
            ('同址标注', len(sec['lanes']), 'L=%d' % L),
            ('轮渡', len(sec['ferry']), 'F=%d' % F),
        ]
        for nm, size, note in rows:
            print('  %-18s %10d B  %s' % (nm, size, note))
        print('  %-18s %10d B' % ('合计', sum(len(v) for v in sec.values())))
        print('无几何链边（egeom=-1）: %d' % sum(1 for x in egeom if x < 0))

    return {'N': N, 'E': E, 'G': G, 'S': S, 'K': K, 'L': L, 'F': F,
            'big_comp': big_comp}


# ---------------------------------------------------------------- 读回校验

def read_str(buf, off):
    """按 str 编码读回，返回 (字符串, 新偏移)。"""
    (n,) = struct.unpack_from('<H', buf, off)
    off += 2
    s = buf[off:off + n].decode('utf-8')
    off += n
    if buf[off] != 0:
        raise ValueError('字符串缺少 NUL 结尾')
    return s, off + 1


def verify(out_path, g, expect, sample=100):
    """独立读回 bin：结构校验 + 与 pkl 对照的数量/抽样校验。"""
    buf = open(out_path, 'rb').read()
    rnd = random.Random(20261019)
    ok = True

    # ---- 头部与数量一致 ----
    magic, ver, N, E, G, S, K, L, F, _reserved, big = struct.unpack_from('<4sIIIIIIIIIi', buf, 0)
    assert magic == MAGIC, 'magic 不符'
    assert ver == VERSION, 'version 不符'
    hdr = {'N': N, 'E': E, 'G': G, 'S': S, 'K': K, 'L': L, 'F': F}
    for k, v in hdr.items():
        if v != expect[k]:
            raise AssertionError('头部 %s=%d 与导出统计 %d 不符' % (k, v, expect[k]))
    off = 44

    if N != len(g['adj']) or E != sum(len(v) for v in g['adj'].values()):
        print('[x] 节点/边数与 pkl 不符: N=%d E=%d' % (N, E))
        ok = False
    else:
        print('[1] 节点/边数与 pkl 一致: N=%d E=%d' % (N, E))

    # ---- 节点数组 ----
    def rd(tc, cnt, sz):
        nonlocal off
        a = array.array(tc)
        a.frombytes(buf[off:off + sz * cnt])
        off += sz * cnt
        if sys.byteorder != 'little':
            a.byteswap()
        return a

    lat_a = rd('d', N, 8)
    lon_a = rd('d', N, 8)
    comp_a = rd('i', N, 4)

    # ---- CSR ----
    head = rd('i', N + 1, 4)
    eto = rd('i', E, 4)
    ew = rd('d', E, 8)
    egeom = rd('i', E, 4)
    assert head[0] == 0 and head[N] == E, 'head 边界不符'
    mono = all(head[i] <= head[i + 1] for i in range(N))
    bad_to = sum(1 for x in eto if x < 0 or x >= N)
    bad_gm = sum(1 for x in egeom if x < -1 or x >= G)
    print('[2] CSR: head 单调=%s head[N]=E；eto 越界 %d，egeom 越界 %d'
          % (mono, bad_to, bad_gm))
    if not mono or bad_to or bad_gm:
        ok = False

    # 双向对称性抽样：i->j 存在则 j->i 存在且权重一致
    sym_bad = 0
    for _ in range(2000):
        i = rnd.randrange(N)
        if head[i] == head[i + 1]:
            continue
        kk = rnd.randrange(head[i], head[i + 1])
        j = eto[kk]
        found = -1
        for k2 in range(head[j], head[j + 1]):
            if eto[k2] == i:
                found = k2
                break
        if found < 0 or abs(ew[found] - ew[kk]) > 1e-6:
            sym_bad += 1
    print('[3] 抽 2000 条边检查双向对称: 异常 %d' % sym_bad)
    if sym_bad:
        ok = False

    # ---- 几何表 ----
    gcnt = rd('I', G, 4)
    total_pts = sum(gcnt)
    pts = rd('d', total_pts * 2, 8)
    goff = [0] * (G + 1)
    for i in range(G):
        goff[i + 1] = goff[i] + gcnt[i]
    assert goff[G] == total_pts
    print('[4] 几何表: G=%d，共 %d 点 (%.1f MB)' % (G, total_pts, total_pts * 16 / 1e6))

    # ---- 车站 ----
    n_st_ok = 0
    snap_bad = 0
    for _ in range(S):
        code, off = read_str(buf, off)
        node, snap, lat, lon = struct.unpack_from('<iddd', buf, off)
        off += 4 + 8 * 3
        name, off = read_str(buf, off)
        city, off = read_str(buf, off)
        addr, off = read_str(buf, off)
        if 0 <= node < N:
            n_st_ok += 1
            # 吸附距离应与 haversine(站坐标, 吸附节点) 同量级（容差 1m + 0.1%）
            d = hav(lat, lon, lat_a[node], lon_a[node])
            if abs(d - snap) > 1.0 + 0.001 * max(d, snap):
                snap_bad += 1
        else:
            print('    车站 %s node=%d 越界' % (code, node))
    print('[5] 车站: S=%d，node 在界内 %d，snap 校验异常 %d' % (S, n_st_ok, snap_bad))
    if n_st_ok != S or snap_bad:
        ok = False

    # ---- 站名 / 标注 / 轮渡，段落边界对齐 ----
    for _ in range(K):
        _, off = read_str(buf, off)
        _, off = read_str(buf, off)
    for _ in range(L):
        for _f in range(5):
            _, off = read_str(buf, off)
    for _ in range(F):
        off += 32 + 8
        (nn,) = struct.unpack_from('<B', buf, off)
        off += 1
        for _i in range(nn):
            _, off = read_str(buf, off)
    if off != len(buf):
        print('[x] 文件末尾偏移不符: %d != %d' % (off, len(buf)))
        ok = False
    else:
        print('[6] 站名 K=%d、标注 L=%d、轮渡 F=%d，段落边界全部对齐' % (K, L, F))

    # ---- 抽样边：ew 与几何链长相对误差 <2%（对齐 check_graph.py） ----
    undirected = []
    seen = set()
    for i in range(N):
        for kk in range(head[i], head[i + 1]):
            j = eto[kk]
            key = (i, j) if i < j else (j, i)
            if key in seen:
                continue
            seen.add(key)
            undirected.append(kk)
    picks = rnd.sample(undirected, min(sample, len(undirected)))
    mism = 0
    for kk in picks:
        i = _slot_owner(head, kk)
        j = eto[kk]
        w = ew[kk]
        gid = egeom[kk]
        if gid < 0:
            # 无中间点：直线长度即链长
            L = hav(lat_a[i], lon_a[i], lat_a[j], lon_a[j])
        else:
            seq = [(pts[2 * t], pts[2 * t + 1])
                   for t in range(goff[gid], goff[gid] + gcnt[gid])]
            # 方向对齐：链首/链尾谁离边起点 i 近（同 _path_coords 判别）
            d_first = hav(lat_a[i], lon_a[i], seq[0][0], seq[0][1])
            d_last = hav(lat_a[i], lon_a[i], seq[-1][0], seq[-1][1])
            if d_first > d_last:
                seq = seq[::-1]
            chain = [(lat_a[i], lon_a[i])]
            for p in seq:
                if p == chain[-1]:
                    continue
                chain.append(p)
            if chain[-1] != (lat_a[j], lon_a[j]):
                chain.append((lat_a[j], lon_a[j]))
            L = sum(hav(a[0], a[1], b[0], b[1]) for a, b in zip(chain, chain[1:]))
        if abs(L - w) > max(50.0, 0.02 * w):
            mism += 1
            if mism <= 3:
                print('    权重不符 i=%d j=%d 边权 %.0fm 链长 %.0fm' % (i, j, w, L))
    print('[7] 抽样 %d 条边，权重与几何偏差超限: %d (%.2f%%)'
          % (len(picks), mism, 100.0 * mism / max(1, len(picks))))
    if mism > len(picks) * 0.02:
        ok = False

    # ---- big_comp 覆盖率（信息性，不做失败判据） ----
    big_n = sum(1 for c in comp_a if c == big)
    print('[8] 最大连通分量 %d 个节点 (%.1f%%)' % (big_n, 100.0 * big_n / max(1, N)))
    return ok


def _slot_owner(head, kk):
    """二分求边槽 kk 的源节点。"""
    lo, hi = 0, len(head) - 1
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if head[mid] <= kk:
            lo = mid
        else:
            hi = mid - 1
    return lo


# ---------------------------------------------------------------- 入口

def main():
    ap = argparse.ArgumentParser(description='导出 rail_graph.bin（C 直读格式 v1）')
    ap.add_argument('--pkl', default=os.path.join(DATA, 'rail_graph.pkl'))
    ap.add_argument('--stations', default=os.path.join(DATA, 'stations.json'))
    ap.add_argument('--names', default=os.path.join(DATA, 'station_names.json'))
    ap.add_argument('--lanes', default=os.path.join(DATA, 'station_lanes.json'))
    ap.add_argument('--out', default=os.path.join(DATA, 'rail_graph.bin'))
    ap.add_argument('--sample', type=int, default=100, help='抽样校验边数')
    ap.add_argument('--no-verify', action='store_true')
    args = ap.parse_args()

    for p in (args.pkl, args.stations, args.names, args.lanes):
        if not os.path.exists(p):
            print('缺少输入文件: %s' % p, file=sys.stderr)
            return 1

    print('读取 %s ...' % args.pkl)
    with open(args.pkl, 'rb') as f:
        g = pickle.load(f)
    with open(args.stations, encoding='utf-8') as f:
        stations_json = json.load(f)
    with open(args.names, encoding='utf-8') as f:
        names_json = json.load(f)
    with open(args.lanes, encoding='utf-8') as f:
        lanes_json = (json.load(f).get('stations')) or {}

    stats = export(g, stations_json, names_json, lanes_json, args.out)

    if args.no_verify:
        return 0
    print('\n读回校验 %s ...' % args.out)
    ok = verify(args.out, g, stats, sample=args.sample)
    print('\n校验结果:', '通过' if ok else '存在问题')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
