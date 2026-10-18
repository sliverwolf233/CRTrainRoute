#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""预下载全部 12306 车站坐标（20 并发）到 data/stations.json。"""
import json, os, time, urllib.parse, urllib.request
import concurrent.futures

BASE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(BASE, 'data')
URL = 'https://mobile.12306.cn/wxxcx/wechat/bigScreen/getStationAddress'
UA = 'Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) AppleWebKit/605.1.15 Mobile/15E148'
OUT = os.path.join(DATA, 'stations.json')


def fetch(item):
    code, name = item
    body = urllib.parse.urlencode({'stationCode': code}).encode()
    req = urllib.request.Request(URL, data=body, headers={
        'User-Agent': UA,
        'Content-Type': 'application/x-www-form-urlencoded',
        'Referer': 'https://servicewechat.com/'})
    for attempt in range(4):
        try:
            with urllib.request.urlopen(req, timeout=15) as r:
                j = json.loads(r.read().decode('utf-8'))
            d = j.get('data')
            if d and d.get('latitude'):
                return code, {'name': d.get('stationName') or name,
                              'lat': float(d['latitude']), 'lon': float(d['longitute']),
                              'city': d.get('cityName'), 'addr': d.get('stationAddress')}
            return code, None
        except Exception:
            time.sleep(0.5 * (attempt + 1))
    return code, None


def main():
    m = json.load(open(os.path.join(DATA, 'station_names.json'), encoding='utf-8'))
    codes = sorted(m.items())
    out = {}
    if os.path.exists(OUT):
        try:
            out = json.load(open(OUT, encoding='utf-8'))
            print('已有 %d 条，增量续传' % len(out))
        except Exception:
            out = {}
    todo = [c for c in codes if c[0] not in out]
    print('待下载 %d / 共 %d' % (len(todo), len(codes)))
    t0 = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=50) as ex:
        futs = [ex.submit(fetch, c) for c in todo]
        for i, fut in enumerate(concurrent.futures.as_completed(futs), 1):
            code, res = fut.result()
            if res:
                out[code] = res
            if i % 200 == 0:
                print('  %d/%d 已获取 %d  %.0fs' % (i, len(todo), len(out), time.time() - t0))
                json.dump(out, open(OUT, 'w'), ensure_ascii=False)
    json.dump(out, open(OUT, 'w'), ensure_ascii=False)
    print('完成：%d 个车站，用时 %.0fs' % (len(out), time.time() - t0))


if __name__ == '__main__':
    main()
