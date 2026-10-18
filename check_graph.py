#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""图数据自检：验证骨架图与几何链的一致性。"""
import math
import os
import pickle
import sys

BASE = os.path.dirname(os.path.abspath(__file__))
GRAPH = os.path.join(BASE, 'data', 'rail_graph.pkl')
R = 6371008.8


def hav(a1, o1, a2, o2):
    p1, p2 = math.radians(a1), math.radians(a2)
    dp = p2 - p1
    dl = math.radians(o2 - o1)
    x = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * R * math.asin(math.sqrt(x))


def main():
    g = pickle.load(open(GRAPH, 'rb'))
    lats, lons, adj = g['lats'], g['lons'], g['adj']
    geom = g.get('edge_geom', {})
    ok = True

    print('节点 %d，边 %d，几何链 %d' % (len(adj), sum(len(v) for v in adj.values()) // 2, len(geom)))

    # 1. 存储约定检查（仅统计，不影响正确性）
    #    建图用无向边去重，部分边只有单方向链、或链的末端是途经点，
    #    server._path_coords 会按端点距离自动定方向并补齐终点，故这里只报告。
    bad_pairs = sum(1 for (a, b), ch in geom.items() if not ch or ch[-1] not in (a, b))
    print('[1] 末端非端点的链: %d / %d（由 _path_coords 自动纠正）' % (bad_pairs, len(geom)))


    # 2. 链长度累加应等于边权重（按端点对齐后计算，相对误差 <2%）
    mism = 0
    checked = 0
    for (a, b), ch in geom.items():
        if a > b:
            continue
        w = adj[a].get(b)
        if w is None:
            continue
        seq = list(ch)
        # 对齐：让序列从 a 出发、到 b 结束
        if seq and seq[0] == b:
            seq = seq[::-1]
        pts = [a] + [x for x in seq if x != a]
        if pts[-1] != b:
            pts = pts + [b]
        L = sum(hav(lats[x], lons[x], lats[y], lons[y]) for x, y in zip(pts, pts[1:]))
        checked += 1
        if abs(L - w) > max(50.0, 0.02 * w):
            mism += 1
            if mism <= 3:
                print('    权重不符 a=%s b=%s 边权 %.0fm 链长 %.0fm' % (a, b, w, L))
    print('[2] 抽查 %d 条边，权重与几何偏差超限: %d (%.2f%%)' % (
        checked, mism, 100.0 * mism / max(1, checked)))
    if mism > checked * 0.02:
        ok = False

    # 3. 连通分量
    comp = g.get('comp', {})
    big = g.get('big_comp')
    if comp and big is not None:
        big_size = sum(1 for v in comp.values() if v == big)
        print('[3] 最大连通分量 %d 个节点 (%.1f%%)' % (big_size, 100.0 * big_size / len(adj)))
    else:
        print('[3] 缺少连通分量信息')
        ok = False

    # 4. 车站吸附
    sn = g.get('station_nodes', {})
    if sn:
        far = [c for c, n in sn.items() if g['stations'].get(c, {}).get('snap_m', 0) > 10000]
        print('[4] 车站 %d 个，吸附偏移 >10km: %d' % (len(sn), len(far)))
    else:
        print('[4] 无车站数据')

    print('\n自检结果:', '通过' if ok else '存在问题')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
