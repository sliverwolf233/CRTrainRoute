# CRTrainRoute · C 语言重写实施计划 (c-rewrite 分支)

> 本文件是团队唯一事实来源（single source of truth）。要改契约，先改这里并消息 lead。
> 远端：origin = https://github.com/sliverwolf233/CRTrainRoute（fork，由 lead 统一 push）。

## 1. 目标

- 用 C17 重写 server.py 的全部服务能力：同 API、同 JSON 结构、默认同端口 8889。
- 多线程并发：
  - 固定大小线程池处理 HTTP 连接（每连接一个任务，不再每请求一线程）。
  - 每线程独立 A* 搜索状态（dist/prev/done/堆），搜索过程零锁。
  - 指定里程规划（km=）阶段二的区间替换搜索并行化。
- 速度目标：冷启动单查询（广州南→北京西）< 50 ms（Python 首查约 540 ms）；
  并发吞吐随线程数近线性扩展；图加载 < 1 s（二进制直读）。
- 数据管道（下载 geojson / 建图）仍用 Python；新增导出器把 pickle 转成 C 直读二进制。

## 2. 非目标

- 不改 index.html 前端（保持 API 兼容即可）。
- 不在 C 里实现 12306 实时坐标请求（避免 TLS 栈）；车站坐标用本地
  data/stations.json，响应里 metaSource 固定为 "local"。
- 不删除 Python 版；server.py 保留为参照实现与回退。

## 3. 目录与写权限（越界修改必须先消息 lead）

    c/PLAN.md                  本计划          (lead)
    c/core/geom.h  graph.h  astar.h            核心契约     (lead 定义，core-c 可提修改)
    c/core/*.c                图加载/A*/几何     (core-c)
    c/core/core_test.c        核心自测          (core-c)
    c/CMakeLists.txt          构建脚本          (core-c)
    c/server/*.c *.h          服务端全部        (parallel-c)
    tools/export_graph.py     pkl→bin 导出器    (qa-bench)
    tests/test_c_api.py       API 对照测试      (qa-bench)
    tests/bench_compare.py    基准对比          (qa-bench)
    README_C.md               C 版说明          (qa-bench)
    .gitignore                追加 bin 忽略     (qa-bench)
    data/rail_graph.bin       生成产物（不入库）

Python 现有文件一律不改。所有人不得 git commit / push / checkout（lead 统一提交）。
各自用独立构建目录：c/build-core、c/build-par、c/build-qa，避免互相覆盖。

## 4. 二进制图格式 data/rail_graph.bin（v1，小端）

字符串 str = u16 字节长度 + UTF-8 字节 + 一个 NUL（加载后指针直接可用）。
车站电报码 code 为 2..4 位 ASCII。

    [头部]
      char  magic[4] = "CRTR"
      u32   version = 1
      u32   n_nodes        N    骨架节点数（压缩索引 0..N-1）
      u32   n_dir_edges    E    有向边槽数（无向图双向存储，无自环）
      u32   n_geom         G    几何折线条目数（无向）
      u32   n_stations     S    有坐标车站数（来自 data/stations.json）
      u32   n_names        K    电报码→站名表条目数（station_names.json 全量）
      u32   n_lanes        L    同址异名标注条目数
      u32   n_ferry        F    跨海轮渡边数
      u32   reserved       0
      i32   big_comp            最大连通分量 id
    [节点]
      f64 lat[N]; f64 lon[N]; i32 comp[N]
    [CSR 邻接]
      i32 head[N+1]; i32 eto[E]; f64 ew[E]      ew 单位米
      i32 egeom[E]    指向几何表条目下标，-1 = 无中间点（直线/轮渡边）
    [几何表（无向）]
      u32 gcount[G]
      其后按条目顺序存点：每点 f64 lat, f64 lon 交错
      点列 = pickle edge_geom[(a,b)]（a<b）中链的密集坐标（不含 a、含 b），
      去向判别由运行时完成（见 §6 _path_coords 移植）
    [车站 S 条]
      str code; i32 node; f64 snap_m; f64 lat; f64 lon; str name; str city; str addr
      node = pickle station_nodes 映射到压缩索引；snap_m 取 stations_meta
    [站名表 K 条]  str code; str name
    [同址标注 L 条]  str code; str name; str lane_type; str alias_of; str note
    [轮渡 F 条]
      i64 from_lat_u6; i64 from_lon_u6; i64 to_lat_u6; i64 to_lon_u6
      f64 km; u8 n_names; str names[n_names]
      u6 = round(x*1e6) 微度，与 pkl ferry_edges 中 round(...,6) 一致

导出器 tools/export_graph.py 读取 data/rail_graph.pkl + stations.json +
station_names.json + station_lanes.json，按上表写出，并打印各段统计。
生成物约 25-40 MB。

## 5. 核心契约（c/core/ 头文件，已由 lead 写好）

- geom.h：crtr_haversine / crtr_seg_point_dist（与 server.py 逐行等价）。
- graph.h：crtr_graph 结构 + load/free + crtr_nearest_node（网格索引，
  main_only 只认 big_comp，语义同 server.py nearest_node）+ crtr_find_edge +
  crtr_path_length + 车站/站名/同址/轮渡查询辅助函数。
- astar.h：crtr_astar 每线程上下文；crtr_astar_run 支持 crtr_edge_filter
  （位图标记有向边槽：ban=1 完全避开，否则 w*=factor）——覆盖 dijkstra()
  与 _shortest_avoiding() 两个调用形态。堆为二叉最小堆 + 惰性删除。

## 6. 语义移植要求（与 server.py 对齐）

- A*：启发式 = 到终点 haversine 大圆距离（可采纳且一致）；done 惰性删除。
- _path_coords：链方向按「链首/链尾谁离边起点 a 近」判定；跳过等于 a 的首点；
  链尾不是 b 则补 b；相邻重复点（微度坐标相等）去重；无链退化为直线。
  轮渡边判定：路径相邻两点 (a,b) 的微度坐标对命中轮渡表（双向）。
- _stations_along：tol 800 m；路径折线 0.05 度网格分桶；包围盒 pad =
  tol/90000+0.02；按 name 去重保 offset 最小；同址别名剔除（present 集含起终点）。
- plan_by_km：阶段1 迭代 ≤12 次、factor 6.0、整路径惩罚；阶段2 取最接近目标
  的 ≤3 个候选、seg ∈ {30,60,120,200}、step=seg/2、ban 双向、差距 >1500 km 跳过、
  best≤500 m 提前退出；ok 容差 max(5000, target*5%)。阶段2 可并行化，
  结果须仍满足 tests 的 ±5 km 容差。
- resolve() 站名解析：电报码 → 精确名(name, name+站) → 去尾站字 → 唯一包含
  匹配，多命中列出候选。searchStation：精确/前缀/包含三级排序，limit 默认 10 上限 50。
- HTTP：GET /、/index.html、/config.js（白名单静态）、/health、/api/v3、
  /api/v3/health、/api/v3/searchStation、/api/v3/stationRoute。
  错误码 400/404/422/500 与错误 JSON 结构同 Python。
  响应头 Content-Type: application/json; charset=utf-8、
  Access-Control-Allow-Origin: *、Content-Length；支持 keep-alive。
- 数值：distance.meters=round(米,0)、kilometers=round(/1000,3)、
  km 偏差 round(...,2)、坐标输出 round(...,6)。C 的 round 与 Python 的银行家舍入
  在 .5 边界可能差 1ulp，允许（测试用区间断言）。

## 7. 并发设计

- threadpool：固定 worker（默认 = 硬件线程数，上限 16，--threads / CRTR_THREADS
  可调），Win32 线程 + CONDITION_VARIABLE（_WIN32），否则 pthreads。
- 每 worker 一个 crtr_astar 上下文，线程局部持有。
- 路由缓存：key (src,dst) → (length, coords, passed, n_nodes, ferries)，
  互斥锁保护，上限 512 清空（同 Python 语义）。
- km-plan 阶段2：候选固定后各 (i, i+seg) 替换搜索相互独立，投入线程池并行，
  取最优改进后迭代。
- 服务器健壮性：请求行/头部解析防御式、超时断连、大请求 414/400。

## 8. 构建与工具链（本机已验证）

    cmake -S c -B c/build-core -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=gcc
    cmake --build c/build-core -j

注意：本机不指定 CMAKE_C_COMPILER 时 cmake 会选中 clang(MSVC 目标)，
链接缺 kernel32.lib 等系统库而失败；文档与脚本一律带 -DCMAKE_C_COMPILER=gcc。

- gcc (MinGW-w64)，C17，-O2 -Wall -Wextra 零警告；源码 UTF-8 无 BOM。
- 产物目标：crtr_core（静态库）、crtr_server（服务）、crtr_query（CLI 单查询，
  --from IZQ --to BXP [--km 2800] [--json]，便于基准测试）、crtr_core_test。
- crtr_server 参数：--graph <path>（默认 data/rail_graph.bin）、--port（默认
  8889）、--threads、--help。

## 9. 测试与验收

- c/core/core_test.c：按 §4 规范在内存/临时文件构造微型图（含轮渡边、双分量、
  惩罚/禁用过滤），验证 load/A*/nearest/find_edge/path_length 全绿。
- tools/export_graph.py 产出真实 bin 后：check 项 —— 节点/边数与 pkl 一致、
  抽样 100 条边的 ew 与几何链长度相对误差 <2%（对齐 check_graph.py）。
- tests/test_c_api.py：移植 test_api.py 全部 281 项断言到 C 服务（端口 8890，
  环境变量 CR_PORT），metaSource 断言放宽为 "local"；全部通过。
- tests/bench_compare.py：同一批 30+ 查询对比 Python server.py 与 crtr_server
  的冷/热时延与并发吞吐，输出 Markdown 表（README_C.md 引用）。
- 验收：A* 首查 <50ms；8 并发下吞吐 ≥ Python 8 倍；全部测试绿。

## 10. 里程碑

    M1  core-c   [完成] 83 项检查全绿；加载 48-53ms、A* 首查 27-59ms
    M1' qa-bench [完成] bin 40.9MB，导出/读回校验全绿
    M2  parallel-c [完成] 174 项 HTTP 冒烟 + Python 对拍逐位一致
    M3  qa-bench [完成] test_c_api 281/281；基准 4-29x（README_C.md）
    M4  lead     集成审查、README 更新、提交并 push fork

## 11. 团队规则

- 禁止 git commit / push / checkout / branch；lead 统一提交。
- 只写自己范围内的文件；需要改 c/core/*.h 契约或本计划时，先消息 lead。
- 完成里程碑或受阻时 send_message 给 lead，并更新 team task。
- 所有源码 UTF-8 无 BOM，LF 行尾；注释用中文。
