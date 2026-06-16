# ChaosEngine Web 管理后台规格书 v0.1

> **状态：** 草案 | **日期：** 2026-06-15 | **作者：** zhongfangdao
>
> **主题：** 嵌入引擎进程的零依赖 Web 管理后台

---

## 目录

1. [设计目标](#1-设计目标)
2. [架构设计](#2-架构设计)
3. [HTTP API 设计](#3-http-api-设计)
4. [前端仪表盘](#4-前端仪表盘)
5. [模块设计](#5-模块设计)
6. [验收标准](#6-验收标准)
7. [实现路线图](#7-实现路线图)

---

## 1. 设计目标

### 1.1 核心原则

| 原则 | 说明 |
|------|------|
| **零外部依赖** | 纯 C 实现 HTTP 服务器，不依赖 nginx/apache/node |
| **嵌入进程** | 与 chaos_server 同进程，共享内存读取引擎状态 |
| **单文件前端** | 一个 HTML 文件内嵌 CSS/JS，浏览器直接打开 |
| **实时刷新** | 页面自动轮询 API（1s 间隔），无需 WebSocket |
| **只读安全** | 仅暴露查询 API，不提供控制接口 |

### 1.2 不做什么

- ❌ 不做用户认证（内网部署）
- ❌ 不做 WebSocket 推送（增加复杂度）
- ❌ 不做数据库持久化（仅内存快照）
- ❌ 不做多语言（仅中文）
- ❌ 不做移动端适配

---

## 2. 架构设计

### 2.1 整体架构

```
┌─────────────────────────────────────────────────────────┐
│                    chaos_server 进程                     │
│                                                         │
│  ┌──────────┐  ┌──────────┐  ┌──────────────────────┐  │
│  │ TCP Echo │  │ AOI/Cell │  │  ce_http (新增)       │  │
│  │ Server   │  │ Manager  │  │  HTTP :9090           │  │
│  │ :7777    │  │          │  │                       │  │
│  └──────────┘  └──────────┘  │  GET /api/stats       │  │
│        │            │        │  GET /api/aoi         │  │
│        │            │        │  GET /api/cell        │  │
│        │            │        │  GET /api/network     │  │
│        │            │        │  GET /api/memory      │  │
│        │            │        │  GET /                │  │
│        ▼            ▼        │  (内置 HTML 仪表盘)    │  │
│  ┌──────────────────────┐    └──────────────────────┘  │
│  │      engine_core     │              │                │
│  │  (共享内存数据源)      │◄─────────────┘                │
│  └──────────────────────┘                               │
└─────────────────────────────────────────────────────────┘
                          │
                          ▼
              ┌──────────────────────┐
              │   浏览器 :9090        │
              │   📊 实时仪表盘       │
              └──────────────────────┘
```

### 2.2 数据流

```
浏览器                     chaos_server
  │                            │
  │── GET / ──────────────────→│  返回内嵌 HTML
  │←─ 200 HTML ───────────────│
  │                            │
  │── GET /api/stats ─────────→│  读取 g_ecs / g_aoi / g_cell
  │←─ 200 JSON ───────────────│
  │                            │
  │  (每 1 秒轮询)              │
  │── GET /api/aoi ───────────→│  读取 AOI 十字链表状态
  │←─ 200 JSON ───────────────│
  │                            │
  │── GET /api/cell ──────────→│  读取 Cell 网格状态
  │←─ 200 JSON ───────────────│
```

---

## 3. HTTP API 设计

### 3.1 通用格式

所有 API 返回 JSON，格式：

```json
{
  "ok": true,
  "timestamp": 1718400000,
  "uptime": 3600,
  "data": { ... }
}
```

### 3.2 API 列表

| 方法 | 路径 | 说明 |
|------|------|------|
| GET | `/` | 内置 HTML 仪表盘 |
| GET | `/api/stats` | 引擎总览（ECS/内存/帧率） |
| GET | `/api/aoi` | AOI 十字链表状态 |
| GET | `/api/cell` | Cell 网格状态 |
| GET | `/api/network` | 网络统计（连接数/流量/重传） |
| GET | `/api/memory` | 内存使用详情 |
| GET | `/api/log` | 最近日志（环形缓冲） |
| GET | `/api/health` | 健康检查 |

### 3.3 响应示例

#### GET /api/stats

```json
{
  "ok": true,
  "timestamp": 1718400000,
  "uptime": 3600,
  "data": {
    "ecs": {
      "entities": 1024,
      "archetypes": 12,
      "systems": 8,
      "components": 45
    },
    "aoi": {
      "entities": 512,
      "radius": 50.0,
      "events_per_sec": 1200
    },
    "cell": {
      "count": 400,
      "active": 380,
      "splitting": 2,
      "merging": 1
    },
    "network": {
      "connections": 128,
      "bytes_in": 1048576,
      "bytes_out": 2097152,
      "retransmits": 3
    },
    "memory": {
      "used": 52428800,
      "peak": 67108864,
      "allocations": 15234
    },
    "fps": 60,
    "frame_time_us": 16667
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
    "overloaded_cells": [
      {"id": 42, "entities": 180, "max": 150, "bounds": [0,0,500,500]}
    ],
    "cells": [
      {"id": 0, "x": 0, "y": 0, "entities": 45, "process": 1, "state": "active"},
      {"id": 1, "x": 1, "y": 0, "entities": 120, "process": 2, "state": "active"}
    ]
  }
}
```

---

## 4. 前端仪表盘

### 4.1 页面布局

```
┌──────────────────────────────────────────────────────────────┐
│  🔥 ChaosEngine 管理后台                      v0.2  运行 1h23m │
├──────────┬──────────┬──────────┬──────────┬──────────────────┤
│ 实体数    │ 连接数    │  FPS     │ 内存     │  网络流量         │
│  1,024   │   128    │   60     │  50 MB   │  ↑2MB ↓1MB       │
├──────────┴──────────┴──────────┴──────────┴──────────────────┤
│                                                              │
│  ┌─────────────────────┐  ┌─────────────────────────────┐   │
│  │   AOI 十字链表       │  │   Cell 网格热力图             │   │
│  │   实体: 512          │  │   20×20 网格                 │   │
│  │   半径: 50           │  │   🟢🟢🟡🟢🟢                │   │
│  │   事件/秒: 1200      │  │   🟢🟡🔴🟡🟢                │   │
│  │                     │  │   🟢🟢🟡🟢🟢                │   │
│  │   [实体散点图]       │  │   过载: Cell 42 (180/150)    │   │
│  └─────────────────────┘  └─────────────────────────────┘   │
│                                                              │
│  ┌──────────────────────────────────────────────────────┐   │
│  │  最近日志                                             │   │
│  │  [INFO] AOI: Entity 42 entered Cell 12               │   │
│  │  [WARN] CELL: Cell 42 overloaded (180 > 150)         │   │
│  │  [INFO] NET: New connection from 192.168.1.5:54321   │   │
│  └──────────────────────────────────────────────────────┘   │
│                                                              │
│  ┌──────────────────────────────────────────────────────┐   │
│  │  系统信息                                             │   │
│  │  后端: io_uring | eBPF: ✅ | ZCRX: ✅ | 编译: GCC 15 │   │
│  └──────────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────┘
```

### 4.2 技术实现

- 纯 HTML + 内嵌 CSS + 内嵌 JS（单文件，~15KB）
- Canvas 绘制 AOI 散点图和 Cell 热力图
- `setInterval` 每秒轮询 `/api/stats`
- 响应式数字动画（计数跳动效果）

---

## 5. 模块设计

### 5.1 文件结构

```
src_c/admin/                    # 新增 Web 管理后台模块
├── ce_http.h                   # HTTP 服务器接口
├── ce_http.c                   # HTTP 服务器实现（纯 C，零依赖）
├── ce_admin_api.h              # 管理 API 接口
├── ce_admin_api.c              # API 处理函数（读取引擎状态）
├── ce_admin_dashboard_html.h   # 内嵌 HTML 仪表盘（C 字符串常量）
└── CMakeLists.txt              # 构建配置
```

### 5.2 ce_http.h — 微型 HTTP 服务器

```c
/*
 * ChaosEngine 微型 HTTP 服务器
 * 纯 C99，零依赖，仅支持 GET 请求
 */

#ifndef CE_HTTP_H
#define CE_HTTP_H

#include "public_api/ce_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CeHttpServer CeHttpServer;

/** HTTP 请求 */
typedef struct CeHttpRequest {
    const char* method;    /* "GET" */
    const char* path;      /* "/api/stats" */
    const char* query;     /* "key=value" or NULL */
} CeHttpRequest;

/** HTTP 响应 */
typedef struct CeHttpResponse {
    int         status;    /* 200, 404, 500 */
    const char* content_type;
    const char* body;
    int         body_len;
} CeHttpResponse;

/** 路由处理函数 */
typedef CeHttpResponse (*CeHttpHandler)(const CeHttpRequest* req);

/** 启动 HTTP 服务器（后台线程） */
CeHttpServer* ce_http_start(int port);

/** 注册路由 */
void ce_http_route(CeHttpServer* srv, const char* path, CeHttpHandler handler);

/** 停止 HTTP 服务器 */
void ce_http_stop(CeHttpServer* srv);

#ifdef __cplusplus
}
#endif

#endif /* CE_HTTP_H */
```

### 5.3 ce_admin_api.h — 管理 API

```c
/*
 * ChaosEngine 管理 API
 * 读取引擎内部状态，序列化为 JSON
 */

#ifndef CE_ADMIN_API_H
#define CE_ADMIN_API_H

#include "network/ce_http.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 注册所有管理 API 路由到 HTTP 服务器 */
void ce_admin_register_routes(CeHttpServer* srv);

/** 引擎统计快照（JSON） */
char* ce_admin_stats_json(void);

/** AOI 状态（JSON） */
char* ce_admin_aoi_json(void);

/** Cell 状态（JSON） */
char* ce_admin_cell_json(void);

/** 网络统计（JSON） */
char* ce_admin_network_json(void);

/** 内存统计（JSON） */
char* ce_admin_memory_json(void);

/** 最近日志（JSON） */
char* ce_admin_log_json(int lines);

#ifdef __cplusplus
}
#endif

#endif /* CE_ADMIN_API_H */
```

### 5.4 HTTP 服务器实现要点

```
ce_http.c 实现：
1. 创建 TCP socket，绑定端口，listen
2. 使用 io_uring（如果可用）或 select 接受连接
3. 解析 HTTP 请求行（仅 GET，忽略 header）
4. 查路由表，调用 handler
5. 构造 HTTP 响应（状态行 + Content-Type + Content-Length + body）
6. 发送响应，关闭连接（HTTP/1.0 短连接）
7. 在独立线程中运行，不阻塞主循环
```

---

## 6. 验收标准

- [ ] `chaos_server` 启动后，浏览器访问 `http://localhost:9090` 显示仪表盘
- [ ] 仪表盘显示实时数据（实体数/连接数/FPS/内存）
- [ ] AOI 页面显示十字链表状态（实体数/事件统计）
- [ ] Cell 页面显示网格热力图（颜色编码负载）
- [ ] 网络页面显示连接数/流量/重传
- [ ] 日志页面显示最近 50 条日志
- [ ] 页面每秒自动刷新，无明显延迟
- [ ] 全部 6/6 单元测试仍通过
- [ ] HTTP 服务器不阻塞 TCP echo 服务

---

## 7. 实现路线图

```
Phase 1: HTTP 服务器 (~4h)
  ├─ 1.1 ce_http.h 接口定义
  ├─ 1.2 ce_http.c 实现（TCP socket + HTTP 解析 + 路由 + 响应）
  ├─ 1.3 单元测试：GET / 返回 200
  └─ 1.4 提交: [feat](admin): 微型 HTTP 服务器

Phase 2: 管理 API (~3h)
  ├─ 2.1 ce_admin_api.h 接口
  ├─ 2.2 ce_admin_api.c 实现（stats/aoi/cell/network/memory/log）
  ├─ 2.3 JSON 序列化（手动拼接，不依赖第三方库）
  └─ 2.4 提交: [feat](admin): 管理 API

Phase 3: 前端仪表盘 (~3h)
  ├─ 3.1 单文件 HTML + CSS + JS 仪表盘
  ├─ 3.2 Canvas 散点图 + Cell 热力图
  ├─ 3.3 自动轮询 + 数字动画
  └─ 3.4 提交: [feat](admin): Web 仪表盘

Phase 4: 集成 + 测试 (~2h)
  ├─ 4.1 集成到 chaos_server 主循环
  ├─ 4.2 端到端测试（浏览器访问验证）
  ├─ 4.3 全部单元测试通过
  └─ 4.4 提交: [feat](admin): 集成 Web 后台到 chaos_server
```

---

> **下一步：** 确认后按 Phase 1 开始实现 HTTP 服务器。
