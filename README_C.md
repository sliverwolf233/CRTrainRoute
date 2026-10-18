# CRTrainRoute · C 语言版（c-rewrite 分支）

用 C17 重写 `server.py` 的全部服务能力：同 API、同 JSON 结构、默认同端口 8889。
实现计划与团队分工见 [c/PLAN.md](c/PLAN.md)（唯一事实来源）。

## 为什么要有 C 版

Python 版单次 A* 首查约 540 ms，长距离查询的字典/堆开销大；C 版目标：

- 冷启动单查询（广州南→北京西）< 50 ms；
- 并发吞吐随线程数近线性扩展；
- 图加载 < 1 s（二进制直读 `data/rail_graph.bin`）。

## 目录结构

```
c/core/     图加载 / A* 引擎 / 几何工具（契约在 *.h）
c/server/   线程池 / HTTP / JSON 输出 / API / km 规划 / CLI
tools/export_graph.py   pickle -> bin 导出器（见下）
data/rail_graph.bin     C 直读二进制图（生成产物，不入库）
```

## 构建

工具链：gcc (MinGW-w64) / CMake / Make。

> **注意（本机已验证）**：CMake 必须显式指定 C 编译器为 gcc，否则会选中
> clang-MSVC 目标并链接失败：
>
> ```
> cmake -S c -B <dir> -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc
> ```

```bash
# 1) 生成二进制图（需要先有 data/rail_graph.pkl，即先跑过 build_graph.py）
python tools/export_graph.py

# 2) 构建（独立构建目录，避免多人协作互相覆盖）
cmake -S c -B c/build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc
cmake --build c/build -j
```

> 测试与工具默认在**仓库根目录**运行（`crtr_core_test` 等按相对路径找
> `data/rail_graph.bin`）。

产物目标：

| 目标 | 说明 |
|---|---|
| `crtr_core` | 核心静态库（图加载 + A*） |
| `crtr_server` | HTTP 服务（对齐 server.py） |
| `crtr_query` | CLI 单查询：`--from IZQ --to BXP [--km 2800] [--json]` |
| `crtr_core_test` | 核心自测（微型图上验证 load/A*/nearest/find_edge） |

## 运行

```bash
./c/build/crtr_server.exe --graph data/rail_graph.bin --port 8889 [--threads N]
```

- `--graph`：二进制图路径，默认 `data/rail_graph.bin`；
- `--port`：默认 8889（与 Python 版一致）；
- `--threads`：工作线程数，默认硬件线程数（上限 16），也可用环境变量 `CRTR_THREADS`。

## 二进制图格式 v1

`tools/export_graph.py` 把 `data/rail_graph.pkl` + `stations.json` +
`station_names.json` + `station_lanes.json` 导出为小端二进制
`data/rail_graph.bin`（约 41 MB）。格式权威定义见
[c/PLAN.md §4](c/PLAN.md)。导出器自带读回校验：

- 节点 / 有向边数与 pkl 一致（N=196848，E=490570）；
- CSR head 单调、双向对称抽样 2000 条无异常；
- 车站吸附 node 在界内且 snap 与 haversine 距离一致；
- 段落边界全部对齐（站名 K、标注 L、轮渡 F 精确解析到文件末尾）；
- 抽样 100 条边 `ew` 与几何链长相对误差 < 2%（对齐 `check_graph.py` 判据）。

注意：pkl 里的节点 id 是建图期密集编号（含被精简的度-2 中间点），
`edge_geom` 的链与 `station_nodes` 的值都是密集 id，导出时分别映射为
几何坐标（lat/lon 交错）与骨架压缩索引。

## 测试

```bash
# 终端 1：启动 C 服务（测试默认端口 8890，避免与 Python 版冲突）
./c/build/crtr_server.exe --port 8890

# 终端 2：API 对照测试（test_api.py 全部断言的移植）
python tests/test_c_api.py

# 或用环境变量改端口
CR_PORT=9000 python tests/test_c_api.py
```

## API 差异（相对 Python 版）

| 项 | Python (server.py) | C (crtr_server) |
|---|---|---|
| 车站坐标来源 | 12306 实时请求（TTL 缓存） | 本地 `data/stations.json` |
| `metaSource` | `remote` / `cache` | 固定 `local` |
| 端点 / 错误码 / JSON 结构 | — | 完全一致（400/404/422/500） |
| 算路结果缓存 | (src,dst) 上限 512 清空 | 同语义 |

其余端点（`/health`、`/api/v3`、`/api/v3/health`、
`/api/v3/searchStation`、`/api/v3/stationRoute`）、响应头、
CORS、keep-alive 行为与 Python 版一致。

## 基准

`tests/bench_compare.py` 用同一批 30+ 查询对比两个服务的冷启动、
首查时延、热查时延与 8 线程并发吞吐；结果表由脚本写入本节
（`python tests/bench_compare.py --write README_C.md`）。

<!-- bench:begin -->

| 指标 | Python server.py | C crtr_server | C 优势 |
|---|---|---|---|
| 冷启动就绪 | 2.04s | 0.51s | 4.0x |
| 首查时延 IZQ→BXP | 717.7ms | 24.8ms | 28.9x |
| 热查均值 | 318.2ms | 33.8ms | 9.4x |
| 热查 P95 | 3259.7ms | 257.2ms | 12.7x |
| 热查最大 | 5563.2ms | 541.5ms | 10.3x |
| 8 并发 QPS | 56.8 | 575.0 | 10.1x |
| 8 并发均值时延 | 130.7ms | 13.8ms | 9.4x |

样本：热查 90 / 90 条，并发 881 / 8833 请求；2026-10-19 01:56，8 线程并发 15s

<!-- bench:end -->
