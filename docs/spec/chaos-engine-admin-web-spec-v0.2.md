# ChaosEngine Web 管理后台规格书 v0.2

> **状态：** 草案 | **日期：** 2026-06-15 | **作者：** zhongfangdao
>
> **主题：** 独立进程 + Lapis(Lua) + WebSocket 实时推送 + 全子系统覆盖

---

## 目录

1. [设计目标](#1-设计目标)
2. [架构设计](#2-架构设计)
3. [进程间通信协议](#3-进程间通信协议)
4. [Admin API 设计](#4-admin-api-设计)
5. [WebSocket 实时推送](#5-websocket-实时推送)
6. [前端仪表盘](#6-前端仪表盘)
7. [模块设计](#7-模块设计)
8. [启动与配置](#8-启动与配置)
9. [验收标准](#9-验收标准)
10. [实现路线图](#10-实现路线图)

---

## 1. 设计目标

### 1.1 核心原则

| 原则 | 说明 |
|------|------|
| **独立进程** | chaos_admin 与 chaos_server 分离，崩溃不互相影响 |
| **Lapis + Lua** | 用 Lapis（OpenResty 风格 Lua Web 框架）实现 HTTP/WebSocket 服务 |
| **WebSocket 推送** | 实时推送引擎状态变更，替代轮询 |
| **Unix Socket IPC** | 通过 Unix Domain Socket 与 chaos_server 通信，零网络开销 |
| **开关控制** | `chaos_server --admin` 启动时才开启 IPC 服务端，默认关闭 |
| **全子系统覆盖** | ECS / AOI / Cell / Network / Memory / Log / Render 全部可观测 |
| **单文件前端** | 一个 HTML 文件内嵌 CSS/JS，浏览器直接打开 |

### 1.2 与 v0.1 的关键差异

| 维度 | v0.1（废弃） | v0.2（当前） |
|------|-------------|-------------|
| **部署模型** | 嵌入 chaos_server 进程 | 独立 chaos_admin 进程 |
| **实现语言** | 纯 C（ce_http.c） | Lua + Lapis 框架 |
| **通信方式** | 同进程内存读取 | Unix Socket IPC |
| **前端刷新** | HTTP 轮询（1s） | WebSocket 实时推送 |
| **启动方式** | 始终启动 | `--admin` 开关控制 |
| **子系统覆盖** | ECS/AOI/Cell/Network/Memory/Log | +Render +eBPF +AsyncIO |
| **依赖** | 零外部依赖 | Lapis + OpenResty/lua-http |

### 1.3 不做什么

- ❌ 不做用户认证（内网部署）
- ❌ 不做数据库持久化（仅内存快照）
- ❌ 不做多语言（仅中文）
- ❌ 不做移动端适配
- ❌ 不做远程管理（仅 localhost Unix Socket）

---

## 2. 架构设计

### 2.1 整体架构

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
│  │                    ce_admin_ipc_client.lua                   │ │
│  │              (Unix Socket 客户端，JSON-RPC 2.0)              │ │
│  └──────────────────────────┬──────────────────────────────────┘ │
│                             │                                    │
│  ┌──────────────────────────▼──────────────────────────────────┐ │
│  │                    Lapis HTTP/WS Server                      │ │
│  │                   监听 :9090                                  │ │
│  │                                                              │ │
│  │  GET  /                  → 仪表盘 HTML                        │ │
│  │  GET  /api/stats         → JSON 快照                         │ │
│  │  GET  /api/aoi           → AOI 状态                          │ │
│  │  GET  /api/cell          → Cell 网格                         │ │
│  │  GET  /api/network       → 网络统计                          │ │
│  │  GET  /api/memory        → 内存使用                          │ │
│  │  GET  /api/log           → 最近日志                          │ │
│  │  GET  /api/render        → 渲染统计                          │ │
│  │  GET  /api/health        → 健康检查                          │ │
│  │  WS   /ws                → WebSocket 实时推送                │ │
│  └──────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────┘
                                                      │
                                                      ▼
                                          ┌──────────────────────┐
                                          │   浏览器 :9090        │
                                          │   📊 实时仪表盘       │
                                          │   (WebSocket 推送)    │
                                          └──────────────────────┘
```

### 2.2 数据流

```
浏览器                  chaos_admin                chaos_server
  │                         │                          │
  │── WS /ws ──────────────→│                          │
  │←─ WS OPEN ──────────────│                          │
  │                         │── JSON-RPC: stats ──────→│
  │                         │←─ JSON-RPC response ─────│
  │←─ WS push: stats ───────│                          │
  │                         │                          │
  │  (每 500ms 自动推送)     │                          │
  │←─ WS push: stats ───────│── JSON-RPC: stats ──────→│
  │←─ WS push: aoi ─────────│── JSON-RPC: aoi ────────→│
  │←─ WS push: cell ────────│── JSON-RPC: cell ───────→│
  │←─ WS push: network ─────│── JSON-RPC: network ────→│
  │                         │                          │
  │── GET /api/log?n=50 ───→│── JSON-RPC: log(50) ────→│
  │←─ 200 JSON ─────────────│←─ JSON-RPC response ─────│
```

---

## 3. 进程间通信协议

### 3.1 传输层

- **类型：** Unix Domain Socket (SOCK_STREAM)
- **路径：** `/tmp/chaos_admin.sock`
- **格式：** 换行分隔的 JSON（每行一个完整的 JSON 对象）
- **协议：** JSON-RPC 2.0

### 3.2 JSON-RPC 2.0 请求格式

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "method": "stats",
  "params": {}
}
```

### 3.3 JSON-RPC 2.0 响应格式

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "ecs": { "entities": 1024, "archetypes": 12, "systems": 8, "components": 45 },
    "fps": 60,
    "frame_time_us": 16667,
    "uptime": 3600
  }
}
```

### 3.4 支持的方法

| 方法 | 参数 | 说明 |
|------|------|------|
| `stats` | `{}` | 引擎总览（ECS/帧率/运行时间） |
| `aoi` | `{}` | AOI 十字链表状态 |
| `cell` | `{}` | Cell 网格状态 |
| `network` | `{}` | 网络统计（连接/流量/重传） |
| `memory` | `{}` | 内存使用详情 |
| `log` | `{"lines": 50}` | 最近 N 条日志 |
| `render` | `{}` | 渲染统计（仅客户端模式有数据） |
| `system` | `{}` | 系统信息（后端/eBPF/编译） |
| `health` | `{}` | 健康检查 |

### 3.5 chaos_server 侧实现（ce_admin_ipc）

```
src_c/admin_ipc/                   # 新增 IPC 服务端模块
├── ce_admin_ipc.h                 # IPC 服务端接口
├── ce_admin_ipc.c                 # Unix Socket 监听 + JSON-RPC 处理
└── CMakeLists.txt

ce_admin_ipc.c 实现要点：
1. 仅当 --admin 参数传入时初始化
2. 创建 Unix Domain Socket，绑定 /tmp/chaos_admin.sock
3. 在独立线程中 accept 连接（非阻塞）
4. 读取换行分隔的 JSON-RPC 请求
5. 调用对应的引擎内部 API 获取数据
6. 构造 JSON-RPC 响应，写回 socket
7. 支持单连接（管理后台只需一个连接）
```

### 3.6 chaos_admin 侧实现（Lua IPC 客户端）

```lua
-- src_lua/admin/ipc_client.lua
local json = require("cjson")
local socket = require("socket")

local IPC_PATH = "/tmp/chaos_admin.sock"

local M = {}

function M.connect()
    local sock = socket.tcp()
    sock:settimeout(5)
    local ok, err = sock:connect(IPC_PATH)
    if not ok then return nil, err end
    sock:settimeout(0)  -- 非阻塞
    return sock
end

function M.call(sock, method, params)
    local req = json.encode({
        jsonrpc = "2.0",
        id = os.time(),
        method = method,
        params = params or {}
    })
    sock:send(req .. "\n")
    -- 读取响应（行缓冲）
    local line, err = sock:receive("*l")
    if not line then return nil, err end
    return json.decode(line)
end

return M
```

---

## 4. Admin API 设计

### 4.1 通用响应格式

所有 HTTP API 返回 JSON：

```json
{
  "ok": true,
  "timestamp": 1718400000,
  "data": { ... }
}
```

### 4.2 API 列表

| 方法 | 路径 | 说明 | 数据来源 |
|------|------|------|----------|
| GET | `/` | 内置 HTML 仪表盘 | 静态文件 |
| GET | `/api/stats` | 引擎总览 | `stats` RPC |
| GET | `/api/aoi` | AOI 十字链表状态 | `aoi` RPC |
| GET | `/api/cell` | Cell 网格状态 | `cell` RPC |
| GET | `/api/network` | 网络统计 | `network` RPC |
| GET | `/api/memory` | 内存使用详情 | `memory` RPC |
| GET | `/api/log?lines=50` | 最近日志 | `log` RPC |
| GET | `/api/render` | 渲染统计 | `render` RPC |
| GET | `/api/system` | 系统信息 | `system` RPC |
| GET | `/api/health` | 健康检查 | `health` RPC |

### 4.3 响应示例

#### GET /api/stats

```json
{
  "ok": true,
  "timestamp": 1718400000,
  "data": {
    "ecs": {
      "entities": 1024,
      "archetypes": 12,
      "systems": 8,
      "components": 45
    },
    "fps": 60,
    "frame_time_us": 16667,
    "uptime": 3600
  }
}
```

#### GET /api/aoi

```json
{
  "ok": true,
  "data": {
    "entity_count": 512,
    "aoi_radius": 50.0,
    "events": {
      "enter": 120,
      "leave": 95,
      "move": 3400
    },
    "top_entities": [
      {"id": 42, "nearby": 15, "x": 100.0, "y": 200.0},
      {"id": 128, "nearby": 12, "x": 300.0, "y": 150.0}
    ]
  }
}
```

#### GET /api/cell

```json
{
  "ok": true,
  "data": {
    "grid": "20x20",
    "cell_size": "500x500",
    "world_size": "10000x10000",
    "total_cells": 400,
    "active_cells": 380,
    "splitting": 2,
    "merging": 1,
    "migrating": 0,
    "overloaded_cells": [
      {"id": 42, "entities": 180, "max": 150, "bounds": [0, 0, 500, 500]}
    ],
    "cells": [
      {"id": 0, "x": 0, "y": 0, "entities": 45, "process": 1, "state": "active"},
      {"id": 1, "x": 1, "y": 0, "entities": 120, "process": 2, "state": "active"}
    ]
  }
}
```

#### GET /api/network

```json
{
  "ok": true,
  "data": {
    "connections": 128,
    "bytes_in": 1048576,
    "bytes_out": 2097152,
    "retransmits": 3,
    "backend": "io_uring",
    "zcrx": true
  }
}
```

#### GET /api/memory

```json
{
  "ok": true,
  "data": {
    "used": 52428800,
    "peak": 67108864,
    "allocations": 15234,
    "pools": {
      "count": 4,
      "free_blocks": 2048
    }
  }
}
```

#### GET /api/render

```json
{
  "ok": true,
  "data": {
    "draw_calls": 128,
    "triangles": 45678,
    "vertices": 23456,
    "frame_time_ms": 8.3,
    "gpu_time_ms": 6.1,
    "backend": "opengl"
  }
}
```

#### GET /api/system

```json
{
  "ok": true,
  "data": {
    "engine_version": "0.1.0",
    "build_mode": "server",
    "io_backend": "io_uring",
    "ebpf_available": true,
    "zcrx_supported": true,
    "compiler": "GCC 15.0",
    "platform": "Linux 7.0.0-22-generic",
    "pid": 12345
  }
}
```

---

## 5. WebSocket 实时推送

### 5.1 连接

```
ws://localhost:9090/ws
```

### 5.2 推送协议

服务端主动推送 JSON 消息，客户端无需请求：

```json
{
  "type": "stats",
  "timestamp": 1718400000,
  "data": { ... }
}
```

### 5.3 推送类型与频率

| 类型 | 频率 | 内容 |
|------|------|------|
| `stats` | 500ms | ECS 实体数/组件数/系统数、FPS、帧耗时、运行时间 |
| `aoi` | 500ms | AOI 实体数、事件统计、Top-N 热点实体 |
| `cell` | 1000ms | Cell 网格状态、过载 Cell 列表 |
| `network` | 1000ms | 连接数、流量、重传次数 |
| `memory` | 2000ms | 内存使用量、峰值、分配次数 |
| `log` | 实时 | 新日志条目（逐条推送） |
| `render` | 500ms | 渲染统计（仅客户端模式） |

### 5.4 Lapis WebSocket 实现

```lua
-- src_lua/admin/app.lua (Lapis 应用)
local lapis = require("lapis")
local json = require("cjson")
local ipc = require("admin.ipc_client")

local app = lapis.Application()

-- WebSocket 路由
app:match("/ws", function(self)
    local ws = lapis.WebSocket(self)
    
    local sock, err = ipc.connect()
    if not sock then
        return { status = 503, json = { ok = false, error = "Cannot connect to chaos_server" } }
    end
    
    -- 定时推送循环
    local timers = {
        { interval = 0.5, method = "stats",  type = "stats" },
        { interval = 0.5, method = "aoi",    type = "aoi" },
        { interval = 1.0, method = "cell",   type = "cell" },
        { interval = 1.0, method = "network",type = "network" },
        { interval = 2.0, method = "memory", type = "memory" },
        { interval = 0.5, method = "render", type = "render" },
    }
    
    local last_send = {}
    for _, t in ipairs(timers) do
        last_send[t.type] = 0
    end
    
    -- 使用 Lapis 的 timer 或协程实现定时推送
    -- （具体实现取决于 Lapis 版本和事件循环）
    
    return ws
end)

return app
```

### 5.5 客户端 WebSocket 处理

```javascript
// 前端 JS（内嵌在 HTML 中）
const ws = new WebSocket('ws://' + location.host + '/ws');

ws.onmessage = function(event) {
    const msg = JSON.parse(event.data);
    switch (msg.type) {
        case 'stats':   updateStatsPanel(msg.data);   break;
        case 'aoi':     updateAoiPanel(msg.data);     break;
        case 'cell':    updateCellPanel(msg.data);    break;
        case 'network': updateNetworkPanel(msg.data); break;
        case 'memory':  updateMemoryPanel(msg.data);  break;
        case 'log':     appendLog(msg.data);          break;
        case 'render':  updateRenderPanel(msg.data);  break;
    }
};

ws.onclose = function() {
    // 自动重连
    setTimeout(function() { location.reload(); }, 3000);
};
```

---

## 6. 前端仪表盘

### 6.1 页面布局

```
┌──────────────────────────────────────────────────────────────────┐
│  🔥 ChaosEngine 管理后台                          v0.2  运行 1h23m │
├──────────┬──────────┬──────────┬──────────┬──────────────────────┤
│ 实体数    │ 连接数    │  FPS     │ 内存     │  网络流量             │
│  1,024   │   128    │   60     │  50 MB   │  ↑2MB ↓1MB           │
├──────────┴──────────┴──────────┴──────────┴──────────────────────┤
│                                                                  │
│  ┌─────────────────────┐  ┌─────────────────────────────────┐   │
│  │   AOI 十字链表       │  │   Cell 网格热力图                 │   │
│  │   实体: 512          │  │   20×20 网格                     │   │
│  │   半径: 50           │  │   🟢🟢🟡🟢🟢                    │   │
│  │   事件/秒: 1200      │  │   🟢🟡🔴🟡🟢                    │   │
│  │                     │  │   🟢🟢🟡🟢🟢                    │   │
│  │   [实体散点图]       │  │   过载: Cell 42 (180/150)        │   │
│  └─────────────────────┘  └─────────────────────────────────┘   │
│                                                                  │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  最近日志 (WebSocket 实时)                                 │   │
│  │  [INFO] AOI: Entity 42 entered Cell 12                   │   │
│  │  [WARN] CELL: Cell 42 overloaded (180 > 150)             │   │
│  │  [INFO] NET: New connection from 192.168.1.5:54321       │   │
│  └──────────────────────────────────────────────────────────┘   │
│                                                                  │
│  ┌──────────────────────────────────────────────────────────┐   │
│  │  系统信息                                                  │   │
│  │  后端: io_uring | eBPF: ✅ | ZCRX: ✅ | 编译: GCC 15     │   │
│  │  模式: server | PID: 12345 | 平台: Linux                  │   │
│  └──────────────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────────┘
```

### 6.2 技术实现

- 纯 HTML + 内嵌 CSS + 内嵌 JS（单文件，~20KB）
- Canvas 绘制 AOI 散点图和 Cell 热力图
- WebSocket 实时接收推送数据（替代 v0.1 的 setInterval 轮询）
- 响应式数字动画（计数跳动效果）
- WebSocket 断开自动重连（3 秒后退避重连）

### 6.3 面板详情

#### 顶部指标卡

| 指标 | 数据来源 | 更新方式 |
|------|----------|----------|
| 实体数 | `stats.ecs.entities` | WebSocket push |
| 连接数 | `network.connections` | WebSocket push |
| FPS | `stats.fps` | WebSocket push |
| 内存 | `memory.used` | WebSocket push |
| 网络流量 | `network.bytes_in/out` | WebSocket push |

#### AOI 面板

- 实体总数 + AOI 半径
- 事件统计（enter/leave/move 计数）
- Canvas 散点图：以服务器坐标绘制实体位置
- Top-N 热点实体列表（周围实体最多的前 5 个）

#### Cell 面板

- 网格信息（尺寸、Cell 大小、世界大小）
- Canvas 热力图：颜色编码每个 Cell 的负载
  - 🟢 绿色：负载 < 50%
  - 🟡 黄色：负载 50-80%
  - 🔴 红色：负载 > 80%
- 过载 Cell 警告列表
- Cell 状态分布（active/splitting/merging/migrating）

#### 渲染面板（仅客户端模式显示）

- Draw Calls / Triangles / Vertices
- Frame Time (CPU) / GPU Time
- 渲染后端（OpenGL/Vulkan/Metal）

#### 日志面板

- WebSocket 实时逐条推送
- 颜色编码日志级别（TRACE=灰, DEBUG=蓝, INFO=绿, WARN=黄, ERROR=红, FATAL=紫）
- 显示时间戳、类别、消息
- 保留最近 200 条在浏览器内存中

#### 系统信息面板

- 引擎版本、编译模式
- I/O 后端（io_uring/POSIX）
- eBPF 可用性
- ZCRX 支持
- 编译器、平台、进程 PID

---

## 7. 模块设计

### 7.1 文件结构

```
chaos-engine/
│
├── src_c/admin_ipc/                  # 【新增】IPC 服务端（C，嵌入 chaos_server）
│   ├── ce_admin_ipc.h                #   IPC 服务端接口
│   ├── ce_admin_ipc.c                #   Unix Socket 监听 + JSON-RPC 2.0 处理
│   └── CMakeLists.txt                #   构建配置
│
├── src_lua/admin/                    # 【新增】Web 管理后台（Lua，独立进程）
│   ├── init.lua                      #   Lapis 应用入口
│   ├── app.lua                       #   路由定义（HTTP + WebSocket）
│   ├── ipc_client.lua                #   Unix Socket 客户端（JSON-RPC 2.0）
│   ├── dashboard_html.lua            #   内嵌仪表盘 HTML（Lua 字符串常量）
│   ├── config.lua                    #   配置（端口、socket 路径、推送间隔）
│   └── nginx.conf                    #   OpenResty 配置（可选，Lapis 也可独立运行）
│
└── docs/spec/
    └── chaos-engine-admin-web-spec-v0.2.md  # 本文件
```

### 7.2 ce_admin_ipc.h — IPC 服务端接口

```c
/*
 * ChaosEngine 管理 IPC 服务端
 * 通过 Unix Domain Socket 暴露引擎内部状态
 * 仅在 --admin 参数传入时启动
 */

#ifndef CE_ADMIN_IPC_H
#define CE_ADMIN_IPC_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CeAdminIpc CeAdminIpc;

/** 初始化 IPC 服务端
 *  @param socket_path  Unix Socket 路径（如 "/tmp/chaos_admin.sock"）
 *  @return 服务端句柄，NULL 表示失败 */
CeAdminIpc* ce_admin_ipc_start(const char* socket_path);

/** 停止 IPC 服务端 */
void ce_admin_ipc_stop(CeAdminIpc* ipc);

/** 检查 IPC 是否在运行 */
CeBool ce_admin_ipc_is_running(CeAdminIpc* ipc);

#ifdef __cplusplus
}
#endif

#endif /* CE_ADMIN_IPC_H */
```

### 7.3 ce_admin_ipc.c 实现要点

```
1. 创建 Unix Domain Socket (SOCK_STREAM)
2. 绑定到指定路径（如 /tmp/chaos_admin.sock）
3. listen + 在独立线程中 accept
4. 单连接模式：同时只接受一个 admin 客户端
5. 读取换行分隔的 JSON 行
6. 解析 JSON-RPC 2.0 请求（method + params）
7. 分发到对应处理函数：

   handle_stats()    → ce_ecs_get_entity_count(), ce_ecs_get_component_count(),
                       ce_time_get_total(), ce_time_get_delta()
   handle_aoi()      → ce_aoi_entity_count(), ce_aoi_query_nearby()
   handle_cell()     → ce_cell_count(), ce_cell_get(), ce_cell_debug_print()
   handle_network()  → 连接计数、流量统计、ce_ebpf_get_retransmit_count()
   handle_memory()   → ce_mempool_free_count(), 内存使用统计
   handle_log(n)     → ce_log_get_recent()
   handle_render()   → ce_render_get_stats()
   handle_system()   → ce_async_backend_name(), ce_ebpf_available(),
                       ce_async_has_zcrx()
   handle_health()   → ce_get_state()

8. 构造 JSON-RPC 2.0 响应，写回 socket
9. 客户端断开后等待重连
```

### 7.4 Lapis 应用结构

```
src_lua/admin/
│
├── init.lua                 # 应用入口
│   └── require("admin.app")
│
├── app.lua                  # Lapis 路由
│   ├── GET  /               → 返回 dashboard_html
│   ├── GET  /api/stats      → ipc.call("stats")
│   ├── GET  /api/aoi        → ipc.call("aoi")
│   ├── GET  /api/cell       → ipc.call("cell")
│   ├── GET  /api/network    → ipc.call("network")
│   ├── GET  /api/memory     → ipc.call("memory")
│   ├── GET  /api/log        → ipc.call("log", {lines=n})
│   ├── GET  /api/render     → ipc.call("render")
│   ├── GET  /api/system     → ipc.call("system")
│   ├── GET  /api/health     → ipc.call("health")
│   └── WS   /ws             → WebSocket 推送循环
│
├── ipc_client.lua           # Unix Socket JSON-RPC 客户端
│   ├── connect()            #   连接到 /tmp/chaos_admin.sock
│   ├── call(method, params) #   发送 JSON-RPC 请求，返回结果
│   └── close()              #   关闭连接
│
├── dashboard_html.lua       # 内嵌 HTML 仪表盘
│   └── 单文件 HTML + CSS + JS（~20KB）
│
└── config.lua               # 配置常量
    ├── ADMIN_PORT = 9090
    ├── IPC_PATH = "/tmp/chaos_admin.sock"
    └── PUSH_INTERVALS = { stats=0.5, aoi=0.5, cell=1.0, ... }
```

---

## 8. 启动与配置

### 8.1 chaos_server 启动

```bash
# 不带管理后台（默认）
./chaos_server

# 带管理后台
./chaos_server --admin

# 自定义 socket 路径
./chaos_server --admin --admin-sock /var/run/chaos/admin.sock
```

### 8.2 chaos_admin 启动

```bash
# 使用 Lapis 启动
cd src_lua/admin
lapis server production

# 或指定端口
lapis server --port 9090

# 或使用 OpenResty
openresty -p . -c nginx.conf
```

### 8.3 一键启动脚本

```bash
#!/bin/bash
# scripts/start_with_admin.sh

# 启动 chaos_server（带管理 IPC）
./build/chaos_server --admin &
SERVER_PID=$!

# 等待 socket 就绪
while [ ! -S /tmp/chaos_admin.sock ]; do
    sleep 0.1
done

# 启动 chaos_admin
cd src_lua/admin
lapis server production &
ADMIN_PID=$!

echo "chaos_server PID: $SERVER_PID"
echo "chaos_admin PID: $ADMIN_PID"
echo "Dashboard: http://localhost:9090"

# 等待退出
trap "kill $SERVER_PID $ADMIN_PID 2>/dev/null" EXIT
wait
```

### 8.4 配置项

| 配置项 | 默认值 | 说明 |
|--------|--------|------|
| `--admin` | false | 启用管理 IPC |
| `--admin-sock` | `/tmp/chaos_admin.sock` | Unix Socket 路径 |
| `ADMIN_PORT` | 9090 | HTTP/WS 监听端口 |
| `PUSH_INTERVAL_STATS` | 500ms | 统计推送间隔 |
| `PUSH_INTERVAL_AOI` | 500ms | AOI 推送间隔 |
| `PUSH_INTERVAL_CELL` | 1000ms | Cell 推送间隔 |
| `PUSH_INTERVAL_NETWORK` | 1000ms | 网络推送间隔 |
| `PUSH_INTERVAL_MEMORY` | 2000ms | 内存推送间隔 |
| `PUSH_INTERVAL_RENDER` | 500ms | 渲染推送间隔 |

---

## 9. 验收标准

### 9.1 功能验收

- [ ] `chaos_server --admin` 启动后创建 `/tmp/chaos_admin.sock`
- [ ] `chaos_server`（不带 --admin）不创建 socket，无额外开销
- [ ] `chaos_admin` 启动后连接 socket 成功
- [ ] 浏览器访问 `http://localhost:9090` 显示仪表盘
- [ ] 仪表盘通过 WebSocket 实时接收数据推送（非轮询）
- [ ] 顶部指标卡显示实时数据（实体数/连接数/FPS/内存/流量）
- [ ] AOI 面板显示十字链表状态（实体数/事件统计/散点图）
- [ ] Cell 面板显示网格热力图（颜色编码负载/过载警告）
- [ ] 网络面板显示连接数/流量/重传/后端信息
- [ ] 内存面板显示使用量/峰值/分配次数/内存池状态
- [ ] 日志面板实时逐条推送新日志（颜色编码级别）
- [ ] 渲染面板显示渲染统计（客户端模式）
- [ ] 系统信息面板显示后端/eBPF/ZCRX/编译信息
- [ ] WebSocket 断开后自动重连
- [ ] chaos_admin 崩溃不影响 chaos_server 运行
- [ ] chaos_server 重启后 chaos_admin 自动重连

### 9.2 性能验收

- [ ] IPC 通信延迟 < 1ms（Unix Socket 本机）
- [ ] WebSocket 推送延迟 < 10ms（本地网络）
- [ ] chaos_admin 内存占用 < 50MB
- [ ] chaos_server 在 --admin 模式下额外 CPU 开销 < 1%
- [ ] 仪表盘页面加载时间 < 500ms

### 9.3 兼容性验收

- [ ] 全部现有单元测试仍通过
- [ ] TCP echo 服务不受影响
- [ ] AOI/Cell 功能不受影响

---

## 10. 实现路线图

```
Phase 1: IPC 服务端 (~6h)
  ├─ 1.1 ce_admin_ipc.h 接口定义
  ├─ 1.2 ce_admin_ipc.c 实现（Unix Socket + JSON-RPC 2.0）
  ├─ 1.3 集成到 ce_server_main.c（--admin 开关）
  ├─ 1.4 单元测试：JSON-RPC 请求/响应
  └─ 1.5 提交: [feat](admin): IPC 服务端（Unix Socket + JSON-RPC 2.0）

Phase 2: Lapis Web 服务 (~6h)
  ├─ 2.1 项目骨架（init.lua / config.lua / nginx.conf）
  ├─ 2.2 ipc_client.lua（Unix Socket JSON-RPC 客户端）
  ├─ 2.3 app.lua（HTTP 路由：/api/stats, /api/aoi, ...）
  ├─ 2.4 WebSocket /ws 路由 + 定时推送循环
  ├─ 2.5 健康检查 + 错误处理
  └─ 2.6 提交: [feat](admin): Lapis Web 服务 + WebSocket 推送

Phase 3: 前端仪表盘 (~4h)
  ├─ 3.1 单文件 HTML + CSS + JS 仪表盘
  ├─ 3.2 WebSocket 客户端（自动重连）
  ├─ 3.3 Canvas 散点图（AOI）+ 热力图（Cell）
  ├─ 3.4 实时日志面板（颜色编码）
  ├─ 3.5 数字动画 + 响应式布局
  └─ 3.6 提交: [feat](admin): Web 仪表盘（WebSocket 实时推送）

Phase 4: 全子系统数据对接 (~4h)
  ├─ 4.1 ECS 统计（entity/archetype/system/component count）
  ├─ 4.2 AOI 统计（entity count, events, nearby query）
  ├─ 4.3 Cell 统计（grid state, overload detection, heatmap data）
  ├─ 4.4 Network 统计（connections, bytes, retransmits, backend info）
  ├─ 4.5 Memory 统计（usage, peak, allocations, pool state）
  ├─ 4.6 Log 对接（ce_log_get_recent + 实时回调推送）
  ├─ 4.7 Render 统计（draw calls, triangles, frame/gpu time）
  ├─ 4.8 System 信息（backend, eBPF, ZCRX, compiler, platform）
  └─ 4.8 提交: [feat](admin): 全子系统数据对接

Phase 5: 集成测试 + 文档 (~3h)
  ├─ 5.1 端到端测试（启动 server + admin + 浏览器验证）
  ├─ 5.2 异常场景测试（server 重启、admin 崩溃恢复）
  ├─ 5.3 性能基准测试（IPC 延迟、CPU 开销）
  ├─ 5.4 全部单元测试通过
  ├─ 5.5 一键启动脚本
  └─ 5.6 提交: [feat](admin): 集成测试 + 启动脚本
```

**总预估工时：~23h**

---

## 附录 A：子系统数据源映射

### A.1 ECS 子系统

| 指标 | C API | 说明 |
|------|-------|------|
| 实体数 | `ce_ecs_get_entity_count()` | 当前存活实体总数 |
| 组件类型数 | `ce_ecs_get_component_count()` | 已注册的组件类型数 |
| 原型数 | `ce_ecs_get_archetype_count()` | 活跃的 Archetype 数量 |
| 系统数 | `ce_ecs_get_system_count()` | 已注册的系统数量 |

### A.2 AOI 子系统

| 指标 | C API | 说明 |
|------|-------|------|
| AOI 实体数 | `ce_aoi_entity_count()` | 挂载在 AOI 中的实体数 |
| 周围实体数 | `ce_aoi_count_nearby(id)` | 指定实体周围的实体数 |
| 周围实体列表 | `ce_aoi_query_nearby(id, buf, max)` | 指定实体周围的实体列表 |
| 调试打印 | `ce_aoi_debug_print()` | 打印 AOI 链表结构 |

### A.3 Cell 子系统

| 指标 | C API | 说明 |
|------|-------|------|
| Cell 总数 | `ce_cell_count()` | 当前 Cell 总数 |
| Cell 信息 | `ce_cell_get(id)` | 获取指定 Cell 的详细信息 |
| 位置查找 | `ce_cell_find_by_position(x, y)` | 根据坐标查找 Cell |
| 进程分配 | `ce_cell_get_process(id)` | 获取 Cell 的进程 ID |
| 调试打印 | `ce_cell_debug_print()` | 打印 Cell 网格状态 |

### A.4 Network 子系统

| 指标 | C API | 说明 |
|------|-------|------|
| 连接数 | 内部计数器 | 当前活跃连接数 |
| 接收字节 | 内部计数器 | 累计接收字节数 |
| 发送字节 | 内部计数器 | 累计发送字节数 |
| TCP 重传 | `ce_ebpf_get_retransmit_count(ctx)` | eBPF 追踪的 TCP 重传次数 |
| I/O 后端 | `ce_async_backend_name()` | 当前使用的 I/O 后端名称 |
| ZCRX 支持 | `ce_async_has_zcrx()` | 是否支持零拷贝接收 |

### A.5 Memory 子系统

| 指标 | C API | 说明 |
|------|-------|------|
| 内存使用 | 内部追踪 | 当前内存使用量 |
| 峰值内存 | 内部追踪 | 历史峰值内存使用量 |
| 分配次数 | 内部追踪 | 累计内存分配次数 |
| 内存池空闲块 | `ce_mempool_free_count(pool)` | 内存池空闲块数量 |

### A.6 Log 子系统

| 指标 | C API | 说明 |
|------|-------|------|
| 最近日志 | `ce_log_get_recent(buf, max)` | 获取环形缓冲区中的最近日志 |
| 实时日志 | `ce_log_add_callback(cb, data)` | 注册回调接收实时日志 |

### A.7 Render 子系统

| 指标 | C API | 说明 |
|------|-------|------|
| 渲染统计 | `ce_render_get_stats()` | Draw Calls / Triangles / Vertices / Frame Time / GPU Time |
| 渲染后端 | 编译时宏 | OpenGL / Vulkan / Metal / None |

### A.8 System 信息

| 指标 | 来源 | 说明 |
|------|------|------|
| 引擎版本 | 编译时常量 | 版本号 |
| 编译模式 | 编译时宏 | Server / Client / Headless / Editor |
| 运行时间 | `ce_time_get_total()` | 引擎运行总秒数 |
| 帧耗时 | `ce_time_get_delta()` | 上一帧耗时（秒） |
| 引擎状态 | `ce_get_state()` | 当前运行状态 |
| eBPF 可用 | `ce_ebpf_available()` | eBPF 子系统是否可用 |

---

## 附录 B：与 v0.1 的迁移对照

| v0.1 组件 | v0.2 对应 | 变化 |
|-----------|-----------|------|
| `src_c/admin/ce_http.h` | `src_c/admin_ipc/ce_admin_ipc.h` | HTTP 服务器 → IPC 服务端 |
| `src_c/admin/ce_http.c` | `src_c/admin_ipc/ce_admin_ipc.c` | 纯 C HTTP → Unix Socket JSON-RPC |
| `src_c/admin/ce_admin_api.h` | `src_lua/admin/app.lua` | C API → Lapis 路由 |
| `src_c/admin/ce_admin_api.c` | `src_lua/admin/ipc_client.lua` | C JSON 拼接 → Lua IPC 客户端 |
| `src_c/admin/ce_admin_dashboard_html.h` | `src_lua/admin/dashboard_html.lua` | C 字符串 → Lua 字符串 |
| 嵌入进程 | 独立进程 | 同进程 → 跨进程 IPC |
| HTTP 轮询 | WebSocket 推送 | 1s 轮询 → 实时推送 |
| 6 个 API | 10 个 API | +render +system +WebSocket |
| 始终启动 | `--admin` 开关 | 按需启动 |

---

> **下一步：** 确认后按 Phase 1 开始实现 IPC 服务端。
