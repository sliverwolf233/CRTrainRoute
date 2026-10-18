#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
下载 32 个省级铁路 geojson 到 data/geojson/。

该接口在并发下载时容易返回被截断的响应，因此逐个下载并做 JSON 校验，
失败自动重试。
"""
import json
import os
import sys
import time
import urllib.request

BASE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(BASE, 'data', 'geojson')
INDEX_URL = 'https://next.laide.net.cn/api/getTrainTimeTable/railway/index.php'
FILE_URL = 'https://next.laide.net.cn/api/getTrainTimeTable/railway/%s'
UA = 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36'

# 兜底列表：index.php 不可用时仍能下载
FALLBACK = [
    '110000.geojson', '120000.geojson', '130000.geojson', '140000.geojson',
    '150000.geojson', '210000.geojson', '220000.geojson', '230000.geojson',
    '310000.geojson', '320000.geojson', '330000.geojson', '340000.geojson',
    '350000.geojson', '360000.geojson', '370000.geojson', '410000.geojson',
    '420000.geojson', '430000.geojson', '440000.geojson', '450000.geojson',
    '460000.geojson', '500000.geojson', '510000.geojson', '520000.geojson',
    '530000.geojson', '540000.geojson', '610000.geojson', '620000.geojson',
    '630000.geojson', '640000.geojson', '650000.geojson', '810000.geojson',
    '820000.geojson',
]

MAX_RETRY = 5


def get_file_list():
    try:
        req = urllib.request.Request(INDEX_URL, headers={'User-Agent': UA})
        with urllib.request.urlopen(req, timeout=30) as r:
            j = json.loads(r.read().decode('utf-8'))
        files = j.get('file_list') or []
        if files:
            print('索引返回 %d 个文件' % len(files))
            return files
    except Exception as e:
        print('索引获取失败(%s)，使用内置列表' % e)
    return FALLBACK


def valid_json(path):
    try:
        json.load(open(path, encoding='utf-8'))
        return True
    except Exception:
        return False


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    files = get_file_list()
    ok = 0
    for name in files:
        path = os.path.join(OUT_DIR, name)
        if os.path.exists(path) and valid_json(path) and os.path.getsize(path) > 1000:
            print('跳过 %s（已存在）' % name)
            ok += 1
            continue
        for attempt in range(1, MAX_RETRY + 1):
            try:
                req = urllib.request.Request(FILE_URL % name, headers={'User-Agent': UA})
                with urllib.request.urlopen(req, timeout=180) as r:
                    data = r.read()
                with open(path, 'wb') as f:
                    f.write(data)
                if valid_json(path):
                    print('OK   %s  %.1f MB' % (name, len(data) / 1e6))
                    ok += 1
                    break
                print('损坏 %s 第%d次，重试' % (name, attempt))
            except Exception as e:
                print('失败 %s 第%d次: %s' % (name, attempt, e))
            time.sleep(2)
        else:
            print('放弃 %s' % name)

    print('\n完成：%d/%d 个文件 -> %s' % (ok, len(files), OUT_DIR))
    return 0 if ok == len(files) else 1


if __name__ == '__main__':
    sys.exit(main())
