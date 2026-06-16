# Admin — Web 管理后台（v0.2 Delta）

> 来源: chaos-engine-admin-web-spec-v0.2.md | 状态: 已实现 | 变更类型: ADDED

## 概述

新增独立的 Web 管理后台子系统，包含：IPC 服务端（嵌入 chaos_server）、Lapis HTTP/WebSocket 服务（独立 chaos_admin 进程）、内嵌单文件 HTML 仪表盘。通过 Unix Domain Socket + JSON-RPC 2.0 实现跨进程通信，覆盖 ECS/AOI/Cell/Network/Memory/CPU/Log/Render/System/Health 全部子系统。

---

## ADDED Requirements

### Requirement: 独立进程架构

chaos_admin SHALL 作为独立进程运行，与 chaos_server 通过 Unix Domain Socket 通信。两个进程崩溃互不影响。

chaos_server 仅在 `--admin` 参数传入时启动 IPC 服务端，默认不启动，零额外开销。

#### Scenario: 默认启动不带管理后台

- **WHEN** 执行 `./chaos_server`（不带 `--admin` 参数）
- **THEN** 不创建 `/tmp/chaos_admin.sock`
- **AND** 不启动 IPC 监听线程
- **AND** 引擎正常运行，无额外 CPU/内存开销

#### Scenario: 带管理后台启动

- **WHEN** 执行 `./chaos_server --admin`
- **THEN** 创建 Unix Domain Socket 文件 `/tmp/chaos_admin.sock`
- **AND** 在独立线程中监听连接
- **AND** 引擎正常运行

#### Scenario: 自定义 socket 路径

- **WHEN** 执行 `./chaos_server --admin --admin-sock /var/run/chaos/admin.sock`
- **THEN** 在指定路径创建 Unix Domain Socket
- **AND** IPC 服务端正常工作

#### Scenario: admin 崩溃不影响 server

- **WHEN** chaos_admin 进程崩溃或被杀掉
- **THEN** chaos_server 继续正常运行
- **AND** IPC 服务端等待新客户端重连
- **AND** 所有游戏逻辑不受影响

#### Scenario: server 重启后 admin 自动重连

- **WHEN** chaos_server 重启
- **THEN** chaos_admin 检测到连接断开
- **AND** 自动尝试重连（通过 HTTP 请求触发新连接）
- **AND** 重连成功后恢复正常服务

---

### Requirement: IPC 通信协议

chaos_server 与 chaos_admin 之间的通信 SHALL 使用 JSON-RPC 2.0 协议，通过 Unix Domain Socket（SOCK_STREAM）传输，格式为换行分隔的 JSON。

#### Scenario: JSON-RPC 请求

- **WHEN** chaos_admin 发送 `{"jsonrpc":"2.0","id":1,"method":"stats","params":{}}` 后跟换行符
- **THEN** chaos_server 解析请求，调用对应处理函数
- **AND** 返回 `{"jsonrpc":"2.0","id":1,"result":{...}}` 后跟换行符

#### Scenario: 不支持的方法

- **WHEN** chaos_admin 发送 method 为 `"unknown_method"` 的请求
- **THEN** chaos_server 返回 JSON-RPC 错误：`{"jsonrpc":"2.0","id":...,"error":{"code":-32601,"message":"Method not found"}}`

#### Scenario: 单连接模式

- **WHEN** 已有一个 admin 客户端连接
- **THEN** IPC 服务端不接受第二个连接（listen backlog=1）
- **AND** 第一个连接断开后，服务端等待新连接

---

### Requirement: IPC 方法 — stats（引擎总览）

`stats` RPC 方法 SHALL 返回引擎核心运行指标：ECS 实体数、组件类型数、FPS、帧耗时、运行时间。

#### Scenario: 获取引擎统计

- **WHEN** 调用 `stats` RPC 方法
- **THEN** 返回 JSON 包含 `entity_count`（当前存活实体总数）、`component_count`（已注册组件类型数）
- **AND** 包含 `fps`（基于 delta_time 计算）、`frame_time_us`（帧耗时微秒）、`uptime`（运行总秒数）

---

### Requirement: IPC 方法 — aoi（AOI 状态）

`aoi` RPC 方法 SHALL 返回 AOI 十字链表状态：AOI 实体数、AOI 半径、Top-N 实体列表。

#### Scenario: 获取 AOI 状态

- **WHEN** 调用 `aoi` RPC 方法
- **THEN** 返回 JSON 包含 `entity_count`（挂载在 AOI 中的实体数）、`aoi_radius`
- **AND** 包含 `top_entities` 数组（前 10 个实体 ID）

---

### Requirement: IPC 方法 — cell（Cell 网格状态）

`cell` RPC 方法 SHALL 返回 Cell 大地图管理状态：网格信息、Cell 列表、每个 Cell 的边界/实体数/状态/进程分配。

#### Scenario: 获取 Cell 网格状态

- **WHEN** 调用 `cell` RPC 方法
- **THEN** 返回 JSON 包含 `total_cells`（Cell 总数）、`active_cells`（活跃 Cell 数）
- **AND** 包含 `cells` 数组，每个元素含 `id`、`x`、`y`、`w`、`h`、`entity_count`、`state`、`process_id`

---

### Requirement: IPC 方法 — network（网络统计）

`network` RPC 方法 SHALL 返回网络统计：活跃连接数（7777 端口）、累计收发字节、实时速率、峰值速率、I/O 后端、ZCRX 支持。

#### Scenario: 获取网络统计

- **WHEN** 调用 `network` RPC 方法
- **THEN** 返回 JSON 包含 `connections`（从 /proc/net/tcp 统计 7777 端口 ESTABLISHED 连接数）
- **AND** 包含 `bytes_in`、`bytes_out`（从 /proc/net/dev 读取累计流量）
- **AND** 包含 `rate_in`、`rate_out`（两次调用间的实时速率）
- **AND** 包含 `peak_in_rate`、`peak_out_rate`（历史峰值速率）
- **AND** 包含 `backend`（I/O 后端名称）、`zcrx`（零拷贝接收支持）

---

### Requirement: IPC 方法 — memory（内存使用）

`memory` RPC 方法 SHALL 返回进程内存使用详情：物理内存（RSS）、峰值、虚拟内存、堆内存、栈内存。

#### Scenario: 获取内存使用

- **WHEN** 调用 `memory` RPC 方法
- **THEN** 返回 JSON 包含 `used`（VmRSS，字节）、`peak`（VmPeak，字节）
- **AND** 包含 `virtual`（VmSize）、`heap`（VmData）、`stack`（VmStk）
- **AND** 所有数据从 `/proc/self/status` 实时读取

---

### Requirement: IPC 方法 — cpu（CPU 统计）

`cpu` RPC 方法 SHALL 返回机器级和进程级 CPU 使用率。

#### Scenario: 获取 CPU 统计

- **WHEN** 调用 `cpu` RPC 方法
- **THEN** 返回 JSON 包含 `machine_pct`（机器整体 CPU 使用率，从 /proc/stat 计算）
- **AND** 包含 `process_pct`（进程 CPU 使用率，通过 getrusage 计算 delta）
- **AND** 包含 `cpu_cores`（CPU 核心数）

---

### Requirement: IPC 方法 — log（日志查询）

`log` RPC 方法 SHALL 返回引擎最近日志条目，支持增量拉取（`since_us` 参数）。

#### Scenario: 获取最近日志

- **WHEN** 调用 `log` RPC 方法，参数 `{"lines": 50}`
- **THEN** 返回 JSON 包含 `max_timestamp_us`（最新日志时间戳）
- **AND** 包含 `entries` 数组，每个条目含 `level`、`timestamp_us`、`category`、`message`、`file`、`line`

#### Scenario: 增量拉取日志

- **WHEN** 调用 `log` RPC 方法，参数 `{"lines": 50, "since_us": 1718400000000}`
- **THEN** 仅返回时间戳大于 `since_us` 的日志条目
- **AND** 客户端可使用 `max_timestamp_us` 作为下次请求的 `since_us`

---

### Requirement: IPC 方法 — render（渲染统计）

`render` RPC 方法 SHALL 返回渲染统计：Draw Calls、三角形数、顶点数、帧耗时、GPU 耗时、渲染后端。

#### Scenario: 获取渲染统计

- **WHEN** 调用 `render` RPC 方法
- **THEN** 返回 JSON 包含 `draw_calls`、`triangles`、`vertices`
- **AND** 包含 `frame_time_ms`（CPU 帧耗时）、`gpu_time_ms`（GPU 耗时）
- **AND** 包含 `backend`（渲染后端名称）

---

### Requirement: IPC 方法 — system（系统信息）

`system` RPC 方法 SHALL 返回引擎编译和运行环境信息。

#### Scenario: 获取系统信息

- **WHEN** 调用 `system` RPC 方法
- **THEN** 返回 JSON 包含 `engine_version`、`build_mode`（debug/release）
- **AND** 包含 `io_backend`、`ebpf_available`、`zcrx_supported`
- **AND** 包含 `compiler`（gcc/clang/msvc）、`platform`（linux/macos/windows）、`pid`

---

### Requirement: IPC 方法 — health（健康检查）

`health` RPC 方法 SHALL 返回引擎健康状态。

#### Scenario: 引擎运行中

- **WHEN** 引擎状态为 `CE_STATE_RUNNING`
- **THEN** 返回 `{"ok": true, "state": "running", "uptime": ...}`

#### Scenario: 引擎异常

- **WHEN** 引擎状态为 `CE_STATE_ERROR`
- **THEN** 返回 `{"ok": false, "state": "error", "uptime": ...}`

---

### Requirement: Lapis HTTP 服务

chaos_admin SHALL 基于 Lapis 框架提供 HTTP 服务，监听 :9090 端口，提供 REST API 和内嵌仪表盘。

#### Scenario: 仪表盘首页

- **WHEN** 浏览器访问 `http://localhost:9090/`
- **THEN** 返回完整的 HTML 仪表盘页面（内嵌 CSS/JS）
- **AND** Content-Type 为 `text/html`

#### Scenario: REST API 端点

- **WHEN** 访问 `GET /api/stats`
- **THEN** 返回 `{"ok": true, "timestamp": ..., "data": {...}}` 格式的 JSON
- **AND** data 内容来自对应 IPC RPC 方法的 result

#### Scenario: API 端点列表

- **GIVEN** chaos_admin 正在运行
- **THEN** 以下端点全部可用：
  - `GET /` — 仪表盘 HTML
  - `GET /api/stats` — 引擎总览
  - `GET /api/aoi` — AOI 状态
  - `GET /api/cell` — Cell 网格
  - `GET /api/network` — 网络统计
  - `GET /api/memory` — 内存使用
  - `GET /api/cpu` — CPU 统计
  - `GET /api/log?lines=N&since_us=T` — 日志查询
  - `GET /api/render` — 渲染统计
  - `GET /api/system` — 系统信息
  - `GET /api/health` — 健康检查

#### Scenario: IPC 连接失败时的优雅降级

- **WHEN** chaos_server 未启动或 socket 不可用
- **THEN** API 返回 `{"ok": false, "error": "Cannot connect to chaos_server: ...", "timestamp": ...}`
- **AND** HTTP 状态码为 503

---

### Requirement: 前端仪表盘

仪表盘 SHALL 为单文件 HTML（内嵌 CSS + JS），深色主题，包含以下面板：

- 顶部指标卡：实体数、连接数、FPS、内存、网络速率
- AOI 面板：实体数、半径、Canvas 散点图
- Cell 面板：网格信息、Canvas 热力图（颜色编码负载）
- 网络面板：连接数、流量、速率、峰值、后端信息
- 内存面板：RSS、峰值、虚拟内存、堆、栈
- CPU 面板：机器级 CPU%、进程级 CPU%、核心数
- 日志面板：实时日志流（颜色编码级别：TRACE/DEBUG/INFO/WARN/ERROR/FATAL）
- 渲染面板：Draw Calls、三角形、顶点、帧耗时
- 系统信息面板：版本、编译模式、I/O 后端、eBPF、ZCRX、编译器、平台、PID

#### Scenario: 仪表盘加载

- **WHEN** 浏览器访问 `http://localhost:9090/`
- **THEN** 显示深色主题仪表盘
- **AND** 顶部显示 "ChaosEngine 管理后台 v0.2" 标题和运行时间
- **AND** 所有面板在 2 秒内完成首次数据加载

#### Scenario: 数据自动刷新

- **WHEN** 仪表盘页面保持打开
- **THEN** 各面板按配置间隔自动刷新数据（HTTP 轮询）
- **AND** 数字变化时有跳动动画效果

#### Scenario: 日志面板颜色编码

- **WHEN** 日志面板收到不同级别的日志
- **THEN** TRACE 显示为灰色、DEBUG 为蓝色、INFO 为绿色、WARN 为黄色、ERROR 为红色、FATAL 为紫色

#### Scenario: Cell 热力图颜色编码

- **WHEN** Cell 面板渲染热力图
- **THEN** 负载 < 50% 显示绿色、50-80% 显示黄色、> 80% 显示红色

---

### Requirement: 一键启动脚本

`scripts/start_with_admin.sh` SHALL 提供一键启动 chaos_server + chaos_admin 的能力，包括旧进程清理、构建、启动、健康检查。

#### Scenario: 一键启动

- **WHEN** 执行 `scripts/start_with_admin.sh`
- **THEN** 自动清理旧的 chaos_server 和 lapis 进程
- **AND** 清理残留的 socket 文件和端口占用
- **AND** 构建 chaos_server
- **AND** 启动 chaos_server（带 `--admin`）
- **AND** 等待 socket 就绪后启动 chaos_admin
- **AND** 打印 server PID、admin PID、Dashboard URL

#### Scenario: Ctrl+C 优雅退出

- **WHEN** 在启动脚本运行时按 Ctrl+C
- **THEN** 自动 kill chaos_server 和 chaos_admin 进程
- **AND** 清理 `/tmp/chaos_admin.sock`
- **AND** 打印 "所有服务已停止"
