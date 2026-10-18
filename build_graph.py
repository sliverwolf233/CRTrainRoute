#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
构建中国铁路拓扑图。

输入：
  data/geojson/*.geojson      OSM 铁路线几何（32 省）

输出：
  data/rail_graph.pkl         图结构：节点坐标数组 + 邻接表

说明：车站坐标在 API 查询时实时向 12306 请求，建图阶段不预取。

算法：
  1. 读取所有线段，按坐标哈希去重节点；坐标量化到 1e-6 度以合并重复点。
  2. 相邻图节点之间建立双向边，权重 = 大圆距离（米）。
  3. 精简：去掉度数为 2 的中间点，压缩为骨架图，查询时再按车站坐标就近吸附。
"""
import json
import glob
import math
import os
import pickle
import sys

BASE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(BASE, 'data')

EARTH_R = 6371008.8  # 平均地球半径（米）


def haversine(lat1, lon1, lat2, lon2):
    """两点大圆距离，单位米。"""
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp = p2 - p1
    dl = math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * EARTH_R * math.asin(math.sqrt(a))


def main():
    files = sorted(glob.glob(os.path.join(DATA, 'geojson', '*.geojson')))
    if not files:
        print('未找到 geojson 数据', file=sys.stderr)
        return 1
    print('读取 %d 个省级铁路文件...' % len(files))

    # 坐标量化：1e-6 度约 0.11 米，足够合并重复点
    SCALE = 1e6
    node_map = {}       # (qx, qy) -> node_id
    lats = []           # node_id -> lat
    lons = []           # node_id -> lon
    adj = {}            # node_id -> {nbr: weight}


    def get_node(lat, lon):
        key = (int(round(lon * SCALE)), int(round(lat * SCALE)))
        nid = node_map.get(key)
        if nid is None:
            nid = len(lats)
            node_map[key] = nid
            lats.append(lat)
            lons.append(lon)
            adj[nid] = {}
        return nid

    def add_edge(a, b):
        if a == b:
            return
        w = haversine(lats[a], lons[a], lats[b], lons[b])
        if w <= 0:
            return
        # 同一条几何可能被多个省文件重复收录，取较短权重
        old = adj[a].get(b)
        if old is None or w < old:
            adj[a][b] = w
            adj[b][a] = w

    # 只保留铁路正线（OSM fclass=rail）
    KEEP_FCLASS = {'rail'}
    skipped = 0
    total_feat = 0
    for path in files:
        try:
            d = json.load(open(path, encoding='utf-8'))
        except Exception as e:
            print('  跳过损坏文件 %s: %s' % (os.path.basename(path), e), file=sys.stderr)
            continue
        feats = d.get('features') or []
        total_feat += len(feats)
        for ft in feats:
            props = ft.get('properties') or {}
            # 只收国铁干线：排除地铁(subway)、有轨电车(tram)、单轨(monorail)、
            # 轻轨(light_rail)、缆车(funicular)、窄轨(narrow_gauge) 等城市轨道。
            # 这些要素会与国铁线在空间上交织，混入后会破坏网络拓扑
            # （例如中山轨道交通1号线曾把中山北站的吸附点带偏 5.4km）。
            if KEEP_FCLASS and props.get('fclass') not in KEEP_FCLASS:
                skipped += 1
                continue
            geom = ft.get('geometry') or {}
            gtype = geom.get('type')
            coords = geom.get('coordinates') or []
            if gtype == 'LineString':
                lines = [coords]
            elif gtype == 'MultiLineString':
                lines = coords
            else:
                continue
            for line in lines:
                prev = None
                for pt in line:
                    if len(pt) < 2:
                        continue
                    try:
                        lon, lat = float(pt[0]), float(pt[1])
                    except (TypeError, ValueError):
                        continue
                    nid = get_node(lat, lon)
                    if prev is not None:
                        add_edge(prev, nid)
                    prev = nid
        print('  %s: %d 要素，累计节点 %d' % (os.path.basename(path), len(feats), len(lats)))

    print('几何总计 %d 要素（跳过非铁路 %d），节点 %d，边约 %d' % (
        total_feat, skipped, len(lats), sum(len(v) for v in adj.values()) // 2))

    # ---------- 车站吸附 ----------
    # 车站坐标来自预下载的 data/stations.json（12306 电报码 -> 经纬度）。
    # 车站吸附到的节点在精简时必须保留，否则无法反查经过车站。
    station_nodes = {}
    stations_meta = {}
    station_file = os.path.join(DATA, 'stations.json')
    if os.path.exists(station_file):
        stations = json.load(open(station_file, encoding='utf-8'))

        def nearest_node(lat, lon):
            best, bd = None, 1e18
            for i in range(len(lats)):
                d = haversine(lat, lon, lats[i], lons[i])
                if d < bd:
                    best, bd = i, d
            return best, bd

        # 用网格索引加速最近节点查找
        cell = 0.05
        grid = {}
        for i in range(len(lats)):
            grid.setdefault((int(lats[i] / cell), int(lons[i] / cell)), []).append(i)

        def nearest_fast(lat, lon):
            ci, cj = int(lat / cell), int(lon / cell)
            best, bd = None, 1e18
            for rad in range(0, 24):
                for di in range(-rad, rad + 1):
                    for dj in range(-rad, rad + 1):
                        if rad > 0 and abs(di) != rad and abs(dj) != rad:
                            continue
                        for i in grid.get((ci + di, cj + dj), ()):
                            d = haversine(lat, lon, lats[i], lons[i])
                            if d < bd:
                                best, bd = i, d
                if best is not None and bd < rad * cell * 110000 * 0.6:
                    break
            if best is None:
                return nearest_node(lat, lon)
            return best, bd

        far = []
        for code, s in stations.items():
            nid, dm = nearest_fast(s['lat'], s['lon'])
            station_nodes[code] = nid
            stations_meta[code] = {
                'name': s.get('name'), 'lat': s['lat'], 'lon': s['lon'],
                'city': s.get('city'), 'addr': s.get('addr'),
                'node': nid, 'snap_m': round(dm, 1),
            }
            if dm > 5000:
                far.append((code, s.get('name'), dm))
        print('车站吸附完成：%d 个' % len(station_nodes))
        print('吸附距离 >5km：%d 个' % len(far))
        for c, n, d in sorted(far, key=lambda x: -x[2])[:8]:
            print('   %s %s 偏 %.1f km' % (c, n, d / 1000))

    # ---------- 精简图：去掉度数为 2 的中间点 ----------
    # 保留度数 != 2 的节点（岔点/端点），以及车站所在节点
    keep = set(station_nodes.values())
    for nid, nbrs in adj.items():
        if len(nbrs) != 2:
            keep.add(nid)

    print('原始节点 %d，保留骨架节点 %d' % (len(lats), len(keep)))

    out_adj = {n: {} for n in keep}
    # 骨架边 -> 中间几何点序列（用于还原真实线路形状）
    edge_geom = {}
    # 从每个保留节点沿链走到下一个保留节点，权重累加
    visited_edge = set()
    for start in keep:
        for first, w0 in adj[start].items():
            ek = (min(start, first), max(start, first))
            if ek in visited_edge:
                continue
            total = w0
            prev, cur = start, first
            chain = [first]
            visited_edge.add(ek)
            guard = 0
            while cur not in keep:
                guard += 1
                if guard > 100000:
                    total = None
                    break
                nbrs = adj[cur]
                nxt = None
                for cand, w in nbrs.items():
                    if cand != prev:
                        nxt = cand
                        break
                if nxt is None:
                    total = None
                    break
                ek2 = (min(cur, nxt), max(cur, nxt))
                visited_edge.add(ek2)
                total += nbrs[nxt]
                prev, cur = cur, nxt
                chain.append(cur)
            if total is None:
                continue
            if cur not in out_adj:
                out_adj[cur] = {}
            a, b = start, cur
            if a == b:
                continue
            old = out_adj[a].get(b)
            if old is None or total < old:
                out_adj[a][b] = total
                out_adj.setdefault(b, {})[a] = total
                # 几何链：chain 是「从 a 出发、到 b 结束」的中间点（含 b，不含 a）。
                # 先写正向；反向键只在缺失时反向补齐，避免方向被覆盖。
                if (a, b) not in edge_geom:
                    edge_geom[(a, b)] = chain
                if (b, a) not in edge_geom:
                    edge_geom[(b, a)] = chain[::-1]
            visited_edge.add((min(a, b), max(a, b)))

    # 上面用无向 visited_edge 去重，导致反向链常常缺失（反向遍历被跳过）。
    # 统一补齐：(b,a) 缺失时用 (a,b) 反转填充。
    for (a, b), ch in list(edge_geom.items()):
        if (b, a) not in edge_geom:
            edge_geom[(b, a)] = ch[::-1]

    print('精简后：节点 %d，边 %d，几何边 %d' % (
        len(out_adj), sum(len(v) for v in out_adj.values()) // 2, len(edge_geom)))

    # ---------- 连通分量 ----------
    # 车站吸附时必须避开孤立小网（站内孤岛未与干线互通），
    # 因此预先算好每个节点所属分量，并记录最大分量。
    comp = {}
    sizes = {}
    cid = 0
    for s in out_adj:
        if s in comp:
            continue
        stack = [s]
        comp[s] = cid
        n = 0
        while stack:
            u = stack.pop()
            n += 1
            for v in out_adj[u]:
                if v not in comp:
                    comp[v] = cid
                    stack.append(v)
        sizes[cid] = n
        cid += 1
    big_comp = max(sizes, key=sizes.get)
    print('连通分量 %d 个，最大分量含 %d 节点 (%.1f%%)' % (
        cid, sizes[big_comp], 100.0 * sizes[big_comp] / len(out_adj)))

    # ---------- 跨海连接（轮渡） ----------
    # OSM 只记录轨道、不含轮渡航段，海南岛铁路因此自成孤立分量、无法与大陆
    # 联网。这里为「含车站且规模足够」的孤立分量，找一条距主网最近的点对，
    # 按直线距离建一条跨海边（视为轮渡），使寻路可穿越琼州海峡。
    FERRY_MAX_KM = 60.0      # 跨海直线距离上限
    FERRY_MIN_KM = 5.0       # 低于此距离视为站场内部断链，直接补齐、不算轮渡
    FERRY_MIN_COMP = 20      # 孤立分量至少这么多节点才连
    ferry_edges = []
    patched = 0

    station_comp = {}
    for code, nid in station_nodes.items():
        c = comp.get(nid)
        if c is not None and c != big_comp:
            station_comp.setdefault(c, []).append(code)

    # 主网节点按 0.1 度网格分桶，便于就近查找
    GC = 0.1
    main_grid = {}
    for n in out_adj:
        if comp.get(n) == big_comp:
            main_grid.setdefault((int(lats[n] / GC), int(lons[n] / GC)), []).append(n)

    def nearest_main(lat, lon):
        bi, bj = int(lat / GC), int(lon / GC)
        best, bd = None, 1e18
        for r in range(0, 12):
            for di in range(-r, r + 1):
                for dj in range(-r, r + 1):
                    if r > 0 and abs(di) != r and abs(dj) != r:
                        continue
                    for m in main_grid.get((bi + di, bj + dj), ()):
                        d = haversine(lat, lon, lats[m], lons[m])
                        if d < bd:
                            best, bd = m, d
            if best is not None and bd < r * GC * 110000 * 0.7:
                break
        return best, bd

    for c, codes in sorted(station_comp.items(), key=lambda kv: -sizes.get(kv[0], 0)):
        if sizes.get(c, 0) < FERRY_MIN_COMP:
            continue
        members = [n for n in out_adj if comp.get(n) == c]
        best = None
        for n in members:
            m, d = nearest_main(lats[n], lons[n])
            if m is not None and (best is None or d < best[0]):
                best = (d, n, m)
        if best is None or best[0] > FERRY_MAX_KM * 1000:
            continue
        d, hn, mn = best
        out_adj[hn][mn] = d
        out_adj.setdefault(mn, {})[hn] = d
        # 几何：较短距离视为站内断链，用直线补齐即可；较远才记为跨海轮渡
        edge_geom[(hn, mn)] = [mn]
        edge_geom[(mn, hn)] = [hn]
        names = [stations_meta[x].get('name') or x for x in codes]
        for n in members:
            comp[n] = big_comp
        if d < FERRY_MIN_KM * 1000:
            patched += 1
            continue
        ferry_edges.append({
            'from': [round(lons[mn], 6), round(lats[mn], 6)],
            'to': [round(lons[hn], 6), round(lats[hn], 6)],
            'km': round(d / 1000.0, 2),
            'stations': names,
        })
        print('  跨海连接：主网(%.4f,%.4f) <-> 孤岛(%.4f,%.4f) 直线 %.2f km，'
              '连接 %d 节点 / 车站 %s' % (
                  lons[mn], lats[mn], lons[hn], lats[hn], d / 1000.0,
                  sizes.get(c, 0), '、'.join(names[:4])))

    if patched:
        print('  站内断链补齐：%d 处（距离 < %.0f km，不计为轮渡）' % (patched, FERRY_MIN_KM))
    if ferry_edges:
        ns = {}
        for v in comp.values():
            ns[v] = ns.get(v, 0) + 1
        big_comp = max(ns, key=ns.get)
        print('跨海连接后：最大分量含 %d 节点 (%.1f%%)' % (
            ns[big_comp], 100.0 * ns[big_comp] / len(out_adj)))

    graph = {
        'lats': lats, 'lons': lons,
        'adj': out_adj,
        'edge_geom': edge_geom,
        'station_nodes': station_nodes,
        'stations': stations_meta,
        'comp': comp,
        'big_comp': big_comp,
        'ferry_edges': ferry_edges,
    }
    out = os.path.join(DATA, 'rail_graph.pkl')
    with open(out, 'wb') as f:
        pickle.dump(graph, f, protocol=4)
    print('已写出 %s (%.1f MB)' % (out, os.path.getsize(out) / 1e6))
    return 0


if __name__ == '__main__':
    sys.exit(main())
