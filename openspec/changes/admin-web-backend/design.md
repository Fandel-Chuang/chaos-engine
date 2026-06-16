# Design: Web 管理后台（admin-web-backend）

## Context

ChaosEngine v0.1 是一个游戏服务器引擎，运行时完全不可观测。开发者需要一种轻量级的方式来实时查看引擎内部状态（ECS 实体数、AOI 视野、Cell 负载、网络流量、内存使用、日志等），以便调试性能问题和监控生产环境。

v0.2 的设计目标是提供一个独立的 Web 管理后台，通过浏览器即可查看全部子系统的运行状态，同时不增加引擎核心的运行时开销。

## Goals / Non-Goals

**Goals:**

1. **独立进程**：chaos_admin 与 chaos_server 分离，崩溃互不影响。
2. **零开销默认**：不带 `--admin` 参数时，server 行为完全不变。
3. **全子系统覆盖**：ECS/AOI/Cell/Network/Memory/CPU/Log/Render/System/Health 全部可观测。
4. **浏览器访问**：单文件 HTML 仪表盘，无需安装任何客户端。
5. **一键启动**：单个脚本完成构建、启动、清理。

**Non-Goals:**

- ❌ 不做用户认证（内网部署）
- ❌ 不做数据库持久化（仅内存快照）
- ❌ 不做多语言（仅中文）
- ❌ 不做移动端适配
- ❌ 不做远程管理（仅 localhost Unix Socket）
- ❌ 不做 WebSocket 实时推送（当前版本使用 HTTP 轮询，延后到后续版本）

## Decisions

### Decision 1: 进程模型 — 独立进程 vs 嵌入进程

**选择**：独立 chaos_admin 进程。

**理由**：
- 崩溃隔离：admin 进程崩溃不影响 server 运行，server 崩溃不影响 admin 的 HTTP 服务（可显示错误状态）。
- 技术栈隔离：admin 使用 Lua + Lapis，server 使用纯 C，独立进程避免 Lua VM 嵌入 C 进程的复杂性。
- 部署灵活：可以单独重启 admin 而不影响游戏服务。

**替代方案**：
- ❌ 嵌入 chaos_server：v0.1 的做法（`ce_http.c`），但 admin 崩溃会拖垮 server，且 Lua VM 嵌入 C 进程增加复杂度。

### Decision 2: IPC 通信 — Unix Domain Socket vs TCP

**选择**：Unix Domain Socket（SOCK_STREAM）。

**理由**：
- 零网络开销：本机通信，无需经过 TCP/IP 协议栈。
- 延迟极低：< 1ms，适合高频数据查询。
- 安全性：仅本机可访问，无需担心网络暴露。
- 简单：无需处理端口冲突、防火墙等问题。

**替代方案**：
- ❌ TCP localhost：多一层协议栈开销，端口可能冲突。
- ❌ 共享内存：实现复杂，需要同步机制，过度设计。

### Decision 3: 应用层协议 — JSON-RPC 2.0 vs 自定义二进制

**选择**：JSON-RPC 2.0，换行分隔的 JSON 行。

**理由**：
- 人类可读：可用 `nc -U /tmp/chaos_admin.sock` 直接调试。
- 标准化：JSON-RPC 2.0 是成熟协议，有明确的请求/响应/错误格式。
- 零依赖：手写 JSON 构造/解析，不依赖 cJSON 等第三方库。
- 换行分隔：天然的消息边界，无需处理长度前缀。

**替代方案**：
- ❌ 自定义二进制协议：不可调试，需要专门的序列化/反序列化代码。
- ❌ Protocol Buffers：增加构建依赖，过度设计。
- ❌ HTTP over Unix Socket：增加 HTTP 解析复杂度，JSON-RPC 更轻量。

### Decision 4: Web 框架 — Lapis (Lua) vs 其他

**选择**：Lapis（OpenResty 风格 Lua Web 框架）。

**理由**：
- 与引擎 Lua 生态一致：引擎已有 Lua 脚本绑定，使用相同的 Lua 运行时。
- 轻量：Lapis 本身很小，可通过 `lapis server` 独立运行（cqueues 模式），也可部署到 OpenResty。
- 路由简洁：`app:get("/api/stats", handler)` 风格的路由定义直观。
- 内嵌 HTML：Lua 多行字符串 `[[...]]` 天然适合内嵌 HTML 仪表盘。

**替代方案**：
- ❌ 纯 C HTTP 服务器（v0.1 做法）：开发效率低，HTML 模板处理困难。
- ❌ Python Flask/FastAPI：引入新语言运行时，增加部署复杂度。
- ❌ Node.js Express：同上，且与引擎技术栈不一致。

### Decision 5: 前端仪表盘 — 单文件 HTML vs SPA 框架

**选择**：单文件 HTML（内嵌 CSS + JS）。

**理由**：
- 零构建：无需 webpack/vite 等构建工具，直接编辑即可。
- 零依赖：不需要 npm install，不需要 node_modules。
- 部署简单：一个文件，Lapis 直接返回字符串。
- 体积可控：~50KB，加载极快。

**替代方案**：
- ❌ React/Vue SPA：需要构建工具链，增加项目复杂度，对 ~50KB 的仪表盘过度设计。

### Decision 6: 数据刷新 — HTTP 轮询 vs WebSocket 推送

**选择**：当前版本使用 HTTP 轮询（前端 `setInterval`），WebSocket 延后。

**理由**：
- 实现简单：前端 `setInterval` + `fetch()` 即可，无需 Lapis WebSocket 支持。
- 足够用：500ms-2000ms 的轮询间隔对监控仪表盘完全足够。
- WebSocket 复杂度：Lapis 的 WebSocket 支持需要 OpenResty 或额外的事件循环配置，当前 cqueues 模式下实现较复杂。

**替代方案**：
- ❌ WebSocket 实时推送：spec 中的理想方案，但需要 OpenResty 部署或 Lapis 的 WebSocket 协程支持，延后到后续版本。

### Decision 7: JSON 处理 — 手写 vs cJSON

**选择**：手写 JSON 构造/解析函数。

**理由**：
- 零外部依赖：不需要引入 cJSON 或其他 JSON 库。
- 控制力强：只处理已知的 JSON 结构，不需要完整的 JSON 解析器。
- 代码量可控：~300 行辅助函数（json_append_string/int/double/bool/kv_*）。

**替代方案**：
- ❌ cJSON：增加依赖，且对嵌入式场景的 JSON-RPC 处理过度。

## Architecture

### 整体架构

```
┌──────────────────────────────────────────────────────────────────┐
│                       chaos_server 进程                           │
│                                                                  │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────────┐ │
│  │ TCP Echo │  │ AOI/Cell │  │   ECS    │  │  ce_admin_ipc    │ │
│  │ :7777    │  │ Manager  │  │  Systems │  │  (新增模块)       │ │
│  └──────────┘  └──────────┘  └──────────┘  │                   │ │
│        │            │             │        │  Unix Socket      │ │
│        │            │             │        │  /tmp/chaos_      │ │
│        ▼            ▼             ▼        │  admin.sock       │ │
│  ┌──────────────────────────────────────┐  │  (仅 --admin 时   │ │
│  │           engine_core                │  │   创建)           │ │
│  │  (ECS/AOI/Cell/Network/Memory/Log)   │  └────────┬─────────┘ │
│  └──────────────────────────────────────┘           │            │
└─────────────────────────────────────────────────────┼────────────┘
                                                      │
                                          Unix Domain Socket
                                          (JSON-RPC 2.0)
                                                      │
┌─────────────────────────────────────────────────────┼────────────┐
│                       chaos_admin 进程               │            │
│                                                     │            │
│  ┌──────────────────────────────────────────────────▼──────────┐ │
│  │                    ipc_client.lua                            │ │
│  │              (ngx.socket.connect + JSON-RPC 2.0)             │ │
│  └──────────────────────────┬──────────────────────────────────┘ │
│                             │                                    │
│  ┌──────────────────────────▼──────────────────────────────────┐ │
│  │                    Lapis HTTP Server                         │ │
│  │                   监听 :9090                                  │ │
│  │                                                              │ │
│  │  GET  /                  → 仪表盘 HTML                        │ │
│  │  GET  /api/stats         → JSON 快照                         │ │
│  │  GET  /api/aoi           → AOI 状态                          │ │
│  │  GET  /api/cell          → Cell 网格                         │ │
│  │  GET  /api/network       → 网络统计                          │ │
│  │  GET  /api/memory        → 内存使用                          │ │
│  │  GET  /api/cpu           → CPU 统计                          │ │
│  │  GET  /api/log           → 最近日志                          │ │
│  │  GET  /api/render        → 渲染统计                          │ │
│  │  GET  /api/system        → 系统信息                          │ │
│  │  GET  /api/health        → 健康检查                          │ │
│  └──────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────┘
```

### 数据流

```
浏览器                  chaos_admin                chaos_server
  │                         │                          │
  │── GET /api/stats ──────→│                          │
  │                         │── JSON-RPC: stats ──────→│
  │                         │←─ JSON-RPC response ─────│
  │←─ 200 JSON ─────────────│                          │
  │                         │                          │
  │  (前端 setInterval)      │                          │
  │── GET /api/aoi ────────→│── JSON-RPC: aoi ────────→│
  │←─ 200 JSON ─────────────│←─ JSON-RPC response ─────│
  │                         │                          │
  │── GET /api/log?lines=50→│── JSON-RPC: log(50) ────→│
  │←─ 200 JSON ─────────────│←─ JSON-RPC response ─────│
```

### 文件结构

```
chaos-engine/
│
├── src_c/admin_ipc/                  # IPC 服务端（C，嵌入 chaos_server）
│   ├── ce_admin_ipc.h                #   IPC 服务端接口
│   ├── ce_admin_ipc.c                #   Unix Socket 监听 + JSON-RPC 2.0 处理
│   └── CMakeLists.txt                #   构建配置
│
├── src_lua/admin/                    # Web 管理后台（Lua，独立进程）
│   ├── init.lua                      #   Lapis 应用入口
│   ├── app.lua                       #   路由定义（HTTP REST API）
│   ├── ipc_client.lua                #   Unix Socket 客户端（JSON-RPC 2.0）
│   ├── dashboard_html.lua            #   内嵌仪表盘 HTML（Lua 字符串常量）
│   ├── config.lua                    #   配置（端口、socket 路径、推送间隔）
│   └── nginx.conf                    #   OpenResty 配置（可选）
│
├── scripts/
│   └── start_with_admin.sh           # 一键启动脚本
│
└── docs/spec/
    └── chaos-engine-admin-web-spec-v0.2.md  # 规格书
```

### IPC 服务端内部设计

```
ce_admin_ipc_start(socket_path)
  │
  ├── create_listen_socket()
  │     ├── socket(AF_UNIX, SOCK_STREAM)
  │     ├── bind() + listen(1)
  │     └── chmod 0666
  │
  ├── pthread_create(ipc_thread_func)
  │     │
  │     ├── accept() [阻塞等待连接]
  │     │
  │     └── handle_client(client_fd)
  │           │
  │           ├── recv_line() [逐字符读取直到 \n]
  │           │
  │           ├── process_request()
  │           │     ├── json_extract_string("method")
  │           │     ├── json_extract_string("id")
  │           │     ├── json_extract_string("params")
  │           │     └── 路由:
  │           │           stats   → handle_stats()
  │           │           aoi     → handle_aoi()
  │           │           cell    → handle_cell()
  │           │           network → handle_network()
  │           │           memory  → handle_memory()
  │           │           cpu     → handle_cpu()
  │           │           log     → handle_log()
  │           │           render  → handle_render()
  │           │           system  → handle_system()
  │           │           health  → handle_health()
  │           │
  │           └── send_response() [写回 socket]
  │
  └── ce_admin_ipc_stop()
        ├── close(client_fd)
        ├── close(listen_fd)
        ├── unlink(socket_path)
        └── pthread_join()
```

### 前端仪表盘面板设计

```
┌──────────────────────────────────────────────────────────────────┐
│  🔥 ChaosEngine 管理后台                          v0.2  运行 1h23m │
├──────────┬──────────┬──────────┬──────────┬──────────────────────┤
│ 实体数    │ 连接数    │  FPS     │ 内存     │  网络速率             │
│  1,024   │   128    │   60     │  50 MB   │  ↑2MB/s ↓1MB/s       │
├──────────┴──────────┴──────────┴──────────┴──────────────────────┤
│                                                                  │
│  ┌─────────────────────┐  ┌─────────────────────────────────┐   │
│  │   AOI 十字链表       │  │   Cell 网格热力图                 │   │
│  │   实体: 512          │  │   网格: uniform                  │   │
│  │   半径: 100          │  │   🟢🟢🟡🟢🟢                    │   │
│  │                     │  │   🟢🟡🔴🟡🟢                    │   │
│  │   [实体散点图]       │  │   🟢🟢🟡🟢🟢                    │   │
│  └─────────────────────┘  └─────────────────────────────────┘   │
│                                                                  │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  最近日志 (HTTP 轮询)                                      │   │
│  │  [INFO] AOI: Entity 42 entered Cell 12                   │   │
│  │  [WARN] CELL: Cell 42 overloaded (180 > 150)             │   │
│  └──────────────────────────────────────────────────────────┘   │
│                                                                  │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  系统信息                                                  │   │
│  │  后端: io_uring | eBPF: ✅ | ZCRX: ✅ | 编译: gcc        │   │
│  │  模式: debug | PID: 12345 | 平台: linux                   │   │
│  └──────────────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────────┘
```

## Risks / Trade-offs

### Risk 1: 手写 JSON 解析的健壮性

- **风险**：手写的 `json_extract_string`/`json_extract_int` 是简易解析器，可能对非标准 JSON 格式处理不当。
- **缓解**：IPC 通信双方都是自己控制的代码，JSON 格式固定且可预测；实际测试中工作良好。
- **影响**：低。如果未来需要更复杂的 JSON 处理，可以引入 cJSON。

### Risk 2: 单连接模式的并发限制

- **风险**：IPC 服务端只接受一个连接（listen backlog=1），多个浏览器标签页同时访问时，Lapis 的每次 HTTP 请求会创建新的 IPC 连接（短连接模式），但并发请求可能导致连接竞争。
- **缓解**：当前 ipc_client.lua 使用短连接模式（每次 API 调用 connect → call → close），避免了长连接的管理复杂性。单用户场景下不存在并发问题。
- **影响**：低。管理后台通常只有一个用户使用。

### Risk 3: HTTP 轮询的延迟

- **风险**：前端使用 `setInterval` 轮询（500ms-2000ms），数据不是实时推送，可能有最多一个轮询间隔的延迟。
- **缓解**：监控场景对 500ms 延迟不敏感；日志面板使用较短的轮询间隔（1s）。
- **影响**：低。后续版本可升级为 WebSocket 推送。

### Risk 4: /proc 文件系统依赖

- **风险**：network（/proc/net/dev、/proc/net/tcp）、memory（/proc/self/status）、cpu（/proc/stat）依赖 Linux /proc 文件系统，非 Linux 平台不可用。
- **缓解**：ChaosEngine 服务端仅支持 Linux，此限制可接受。
- **影响**：低。

### Trade-off: 短连接 IPC vs 长连接 IPC

- **选择**：短连接模式（每次 API 调用 connect → call → close）。
- **代价**：每次 API 调用需要建立和断开 Unix Socket 连接，有微小的连接开销。
- **收益**：无需管理连接池、重连逻辑、连接状态；代码简单；天然支持多标签页并发。
- **影响**：Unix Socket 连接开销 < 0.1ms，可忽略。
