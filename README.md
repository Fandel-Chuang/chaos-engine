<div align="center">

# ChaosEngine

### 面向 3D 多人游戏的、客户端/服务器同构的纯 C 内核游戏引擎

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C99](https://img.shields.io/badge/C-99-blue.svg)]()
[![C++17](https://img.shields.io/badge/C%2B%2B-17-orange.svg)]()
[![Lua](https://img.shields.io/badge/Lua-5.4-blue.svg)]()
[![Vulkan](https://img.shields.io/badge/Vulkan-RHI-red.svg)]()
[![Platform](https://img.shields.io/badge/platform-Win%20%7C%20Linux%20%7C%20macOS%20%7C%20iOS%20%7C%20Android-green.svg)]()

**io_uring 零拷贝 · eBPF/XDP 内核加速 · ECS + 动态 AOI · Vulkan 渲染 · Lua 服务化 · KCP/TCP/WebSocket 三协议**

</div>

---

## 目录

- [设计理念](#设计理念)
- [架构总览](#架构总览)
- [服务拓扑](#服务拓扑)
- [核心子系统](#核心子系统)
  - [ECS 架构](#1-ecs-架构)
  - [网络协议栈](#2-网络协议栈)
  - [高性能 IO](#3-高性能-io)
  - [网关服务](#4-网关服务)
  - [游戏服务器](#5-游戏服务器)
  - [数据库代理](#6-数据库代理)
  - [路由服务](#7-路由服务)
  - [管理后台](#8-管理后台)
  - [Vulkan 渲染](#9-vulkan-渲染)
  - [同步与复制](#10-同步与复制)
  - [Lua 服务化](#11-lua-服务化)
  - [eBPF/XDP 加速](#12-ebpfxdp-加速)
- [LoopEngine 闭环验证](#loopengine-闭环验证)
- [CI/CD](#cicd)
- [性能基准](#性能基准)
- [快速开始](#快速开始)
- [编译选项](#编译选项)
- [项目结构](#项目结构)
- [参考引擎](#参考引擎)
- [许可证](#许可证)

---

## 设计理念

| 原则 | 说明 |
|------|------|
| **极度简洁** | 内核代码量最小化，每个模块只做一件事 |
| **高效执行** | 纯 C99 内核，Cache-Friendly 数据布局，io_uring + eBPF 内核态加速 |
| **同构逻辑** | 同一份战斗代码，客户端帧同步 + 服务端状态同步 |
| **严格分层** | C 内核 → public_api → C++ 编辑器 → Lua 脚本，四层隔离 |
| **全平台** | Windows / Linux / macOS / iOS / Android |
| **脚本驱动** | Lua 服务化，热更新无需重启进程 |

---

## 架构总览

ChaosEngine 采用五层架构，从下到上依次为：平台层 → 引擎内核 → 公共 API → 编辑器层 → Lua 服务层。

<div align="center">
<table style="border-collapse:collapse;width:100%;max-width:920px;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;font-size:14px;">
<tr>
<td style="padding:0;">

<!-- ===== Layer 5: Lua 服务层 ===== -->
<div style="background:linear-gradient(135deg,#fef3c7,#fde68a);border:2px solid #f59e0b;border-radius:12px 12px 0 0;padding:18px 16px 14px;">
<div style="font-size:12px;font-weight:700;color:#92400e;letter-spacing:1px;margin-bottom:10px;">⬎ LAYER 5 · Lua 服务层</div>
<div style="display:flex;gap:8px;justify-content:center;flex-wrap:wrap;">
<div style="background:#fff;border:1.5px solid #f59e0b;border-radius:8px;padding:8px 14px;font-weight:600;color:#92400e;">Gateway</div>
<div style="background:#fff;border:1.5px solid #f59e0b;border-radius:8px;padding:8px 14px;font-weight:600;color:#92400e;">Router</div>
<div style="background:#fff;border:1.5px solid #f59e0b;border-radius:8px;padding:8px 14px;font-weight:600;color:#92400e;">DBProxy</div>
<div style="background:#fff;border:1.5px solid #f59e0b;border-radius:8px;padding:8px 14px;font-weight:600;color:#92400e;">Admin</div>
<div style="background:#fff;border:1.5px solid #f59e0b;border-radius:8px;padding:8px 14px;font-weight:600;color:#92400e;">Services</div>
</div>
</div>

<!-- ===== Layer 4: 编辑器层 ===== -->
<div style="background:linear-gradient(135deg,#e0e7ff,#c7d2fe);border:2px solid #6366f1;border-top:none;padding:18px 16px 14px;">
<div style="font-size:12px;font-weight:700;color:#3730a3;letter-spacing:1px;margin-bottom:10px;">⬎ LAYER 4 · 编辑器层 (C++17)</div>
<div style="display:flex;gap:8px;justify-content:center;flex-wrap:wrap;">
<div style="background:#fff;border:1.5px solid #6366f1;border-radius:8px;padding:8px 14px;font-weight:600;color:#3730a3;">Dear ImGui</div>
<div style="background:#fff;border:1.5px solid #6366f1;border-radius:8px;padding:8px 14px;font-weight:600;color:#3730a3;">资源导入</div>
<div style="background:#fff;border:1.5px solid #6366f1;border-radius:8px;padding:8px 14px;font-weight:600;color:#3730a3;">日志观测</div>
<div style="background:#fff;border:1.5px solid #6366f1;border-radius:8px;padding:8px 14px;font-weight:600;color:#3730a3;">UI 面板</div>
</div>
</div>

<!-- ===== Layer 3: public_api ===== -->
<div style="background:linear-gradient(135deg,#d1fae5,#a7f3d0);border:2px solid #10b981;border-top:none;padding:18px 16px 14px;">
<div style="font-size:12px;font-weight:700;color:#065f46;letter-spacing:1px;margin-bottom:10px;">⬎ LAYER 3 · public_api (纯 C 头文件)</div>
<div style="background:#fff;border:1.5px solid #10b981;border-radius:8px;padding:8px 20px;font-weight:600;color:#065f46;display:inline-block;">模块间通信接口 · ce_core.h / ce_ecs.h / ce_rhi.h / ce_network.h / ce_server.h / ce_gateway.h / ce_replication.h / ce_sync.h</div>
</div>

<!-- ===== Layer 2: 引擎内核 ===== -->
<div style="background:linear-gradient(135deg,#dbeafe,#bfdbfe);border:2px solid #3b82f6;border-top:none;padding:18px 16px 14px;">
<div style="font-size:12px;font-weight:700;color:#1e40af;letter-spacing:1px;margin-bottom:10px;">⬎ LAYER 2 · 引擎内核 (纯 C99)</div>
<div style="display:flex;gap:6px;justify-content:center;flex-wrap:wrap;">
<div style="background:#fff;border:1.5px solid #3b82f6;border-radius:8px;padding:7px 12px;font-weight:600;color:#1e40af;">Core</div>
<div style="background:#fff;border:1.5px solid #3b82f6;border-radius:8px;padding:7px 12px;font-weight:600;color:#1e40af;">ECS</div>
<div style="background:#fff;border:1.5px solid #3b82f6;border-radius:8px;padding:7px 12px;font-weight:600;color:#1e40af;">Render (RHI Vulkan)</div>
<div style="background:#fff;border:1.5px solid #3b82f6;border-radius:8px;padding:7px 12px;font-weight:600;color:#1e40af;">Network</div>
<div style="background:#fff;border:1.5px solid #3b82f6;border-radius:8px;padding:7px 12px;font-weight:600;color:#1e40af;">Server</div>
<div style="background:#fff;border:1.5px solid #3b82f6;border-radius:8px;padding:7px 12px;font-weight:600;color:#1e40af;">Gateway</div>
<div style="background:#fff;border:1.5px solid #3b82f6;border-radius:8px;padding:7px 12px;font-weight:600;color:#1e40af;">Sync</div>
<div style="background:#fff;border:1.5px solid #3b82f6;border-radius:8px;padding:7px 12px;font-weight:600;color:#1e40af;">Replication</div>
<div style="background:#fff;border:1.5px solid #3b82f6;border-radius:8px;padding:7px 12px;font-weight:600;color:#1e40af;">Save</div>
<div style="background:#fff;border:1.5px solid #3b82f6;border-radius:8px;padding:7px 12px;font-weight:600;color:#1e40af;">eBPF</div>
</div>
</div>

<!-- ===== Layer 1: 平台层 ===== -->
<div style="background:linear-gradient(135deg,#f3f4f6,#e5e7eb);border:2px solid #6b7280;border-top:none;border-radius:0 0 12px 12px;padding:18px 16px 14px;">
<div style="font-size:12px;font-weight:700;color:#374151;letter-spacing:1px;margin-bottom:10px;">⬎ LAYER 1 · 平台抽象层</div>
<div style="display:flex;gap:8px;justify-content:center;flex-wrap:wrap;">
<div style="background:#fff;border:1.5px solid #6b7280;border-radius:8px;padding:7px 14px;font-weight:600;color:#374151;">Windows</div>
<div style="background:#fff;border:1.5px solid #6b7280;border-radius:8px;padding:7px 14px;font-weight:600;color:#374151;">Linux</div>
<div style="background:#fff;border:1.5px solid #6b7280;border-radius:8px;padding:7px 14px;font-weight:600;color:#374151;">macOS</div>
<div style="background:#fff;border:1.5px solid #6b7280;border-radius:8px;padding:7px 14px;font-weight:600;color:#374151;">iOS</div>
<div style="background:#fff;border:1.5px solid #6b7280;border-radius:8px;padding:7px 14px;font-weight:600;color:#374151;">Android</div>
</div>
</div>

</td>
</tr>
</table>
</div>

---

## 服务拓扑

ChaosEngine 的集群由 6 个独立服务进程组成，每个服务通过 `public_api` 与内核交互，服务间通过二进制协议通信。

<div align="center">
<table style="border-collapse:collapse;width:100%;max-width:920px;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;font-size:14px;">
<tr><td style="padding:0;">

<!-- 客户端行 -->
<div style="background:linear-gradient(135deg,#ede9fe,#ddd6fe);border:2px solid #8b5cf6;border-radius:12px 12px 0 0;padding:18px 16px 14px;">
<div style="font-size:12px;font-weight:700;color:#5b21b6;letter-spacing:1px;margin-bottom:10px;">客户端层</div>
<div style="display:flex;gap:10px;justify-content:center;flex-wrap:wrap;">
<div style="background:#fff;border:2px solid #8b5cf6;border-radius:10px;padding:10px 16px;text-align:center;min-width:140px;">
<div style="font-weight:700;color:#5b21b6;">chaos_client</div>
<div style="font-size:11px;color:#7c3aed;margin-top:3px;">Vulkan 3D 渲染</div>
<div style="font-size:11px;color:#7c3aed;">网络同步</div>
</div>
<div style="background:#fff;border:2px solid #8b5cf6;border-radius:10px;padding:10px 16px;text-align:center;min-width:140px;">
<div style="font-weight:700;color:#5b21b6;">chaos_editor</div>
<div style="font-size:11px;color:#7c3aed;margin-top:3px;">Dear ImGui</div>
<div style="font-size:11px;color:#7c3aed;">C++ 编辑器</div>
</div>
</div>
</div>

<!-- 箭头 -->
<div style="text-align:center;padding:4px 0;background:#f9fafb;border-left:2px solid #6b7280;border-right:2px solid #6b7280;font-size:18px;color:#6b7280;">⬇ KCP / TCP / WebSocket ⬇</div>

<!-- 网关行 -->
<div style="background:linear-gradient(135deg,#fef3c7,#fde68a);border:2px solid #f59e0b;border-left:2px solid #6b7280;border-right:2px solid #6b7280;padding:18px 16px 14px;">
<div style="font-size:12px;font-weight:700;color:#92400e;letter-spacing:1px;margin-bottom:10px;">网关层</div>
<div style="display:flex;justify-content:center;">
<div style="background:#fff;border:2px solid #f59e0b;border-radius:10px;padding:10px 20px;text-align:center;min-width:200px;">
<div style="font-weight:700;color:#92400e;">chaos_gateway</div>
<div style="font-size:11px;color:#b45309;margin-top:3px;">协议路由 · 连接管理</div>
<div style="font-size:11px;color:#b45309;">TCP 9000 / KCP 9001 / WS 9002</div>
</div>
</div>
</div>

<!-- 箭头 -->
<div style="text-align:center;padding:4px 0;background:#f9fafb;border-left:2px solid #6b7280;border-right:2px solid #6b7280;font-size:18px;color:#6b7280;">⬇ 内部路由 ⬇</div>

<!-- 游戏服务 + 路由 行 -->
<div style="background:linear-gradient(135deg,#dbeafe,#bfdbfe);border:2px solid #3b82f6;border-left:2px solid #6b7280;border-right:2px solid #6b7280;padding:18px 16px 14px;">
<div style="font-size:12px;font-weight:700;color:#1e40af;letter-spacing:1px;margin-bottom:10px;">游戏服务层</div>
<div style="display:flex;gap:20px;justify-content:center;flex-wrap:wrap;">
<div style="background:#fff;border:2px solid #3b82f6;border-radius:10px;padding:10px 16px;text-align:center;min-width:180px;">
<div style="font-weight:700;color:#1e40af;">chaos_server</div>
<div style="font-size:11px;color:#2563eb;margin-top:3px;">游戏逻辑 · AOI · Cell</div>
<div style="font-size:11px;color:#2563eb;">TCP 7777</div>
</div>
<div style="background:#fff;border:2px solid #3b82f6;border-radius:10px;padding:10px 16px;text-align:center;min-width:180px;">
<div style="font-weight:700;color:#1e40af;">chaos_router</div>
<div style="font-size:11px;color:#2563eb;margin-top:3px;">跨区路由 · 一致性哈希</div>
<div style="font-size:11px;color:#2563eb;">TCP 9100 / 9101</div>
</div>
</div>
</div>

<!-- 箭头 -->
<div style="text-align:center;padding:4px 0;background:#f9fafb;border-left:2px solid #6b7280;border-right:2px solid #6b7280;font-size:18px;color:#6b7280;">⬇ 持久化 ⬇</div>

<!-- 数据层 + 管理行 -->
<div style="display:flex;">
<div style="flex:1;background:linear-gradient(135deg,#d1fae5,#a7f3d0);border:2px solid #10b981;border-left:2px solid #6b7280;border-bottom:2px solid #6b7280;border-radius:0 0 0 12px;padding:18px 16px 14px;">
<div style="font-size:12px;font-weight:700;color:#065f46;letter-spacing:1px;margin-bottom:10px;">数据层</div>
<div style="display:flex;justify-content:center;">
<div style="background:#fff;border:2px solid #10b981;border-radius:10px;padding:10px 16px;text-align:center;min-width:180px;">
<div style="font-weight:700;color:#065f46;">chaos_dbproxy</div>
<div style="font-size:11px;color:#059669;margin-top:3px;">状态镜像 · 持久化</div>
<div style="font-size:11px;color:#059669;">TCP 9003</div>
</div>
</div>
</div>
<div style="flex:1;background:linear-gradient(135deg,#fecaca,#fca5a5);border:2px solid #ef4444;border-right:2px solid #6b7280;border-bottom:2px solid #6b7280;border-radius:0 0 12px 0;padding:18px 16px 14px;">
<div style="font-size:12px;font-weight:700;color:#991b1b;letter-spacing:1px;margin-bottom:10px;">管理层</div>
<div style="display:flex;justify-content:center;">
<div style="background:#fff;border:2px solid #ef4444;border-radius:10px;padding:10px 16px;text-align:center;min-width:180px;">
<div style="font-weight:700;color:#991b1b;">chaos_admin</div>
<div style="font-size:11px;color:#dc2626;margin-top:3px;">Lapis Web · Dashboard</div>
<div style="font-size:11px;color:#dc2626;">HTTP 9090 / WebSocket</div>
</div>
</div>
</div>
</div>

</td></tr>
</table>
</div>

### 服务端口速查

| 服务 | 进程名 | 协议 | 端口 | 职责 |
|------|--------|------|------|------|
| 游戏服务器 | `chaos_server` | TCP | 7777 | 游戏逻辑 + AOI + Cell 空间划分 |
| 网关 | `chaos_gateway` | TCP / KCP / WebSocket | 9000 / 9001 / 9002 | 协议路由 + 连接管理 |
| 数据库代理 | `chaos_dbproxy` | TCP | 9003 | 状态镜像 + 持久化 |
| 路由服务 | `chaos_router` | TCP | 9100 / 9101 | 跨区路由 + 一致性哈希 |
| 管理后台 | `chaos_admin` | HTTP / WebSocket | 9090 | Lapis Web + 实时 Dashboard |

---

## 核心子系统

### 1. ECS 架构

ChaosEngine 采用 Archetype 存储的 ECS（Entity-Component-System）架构，所有游戏实体和逻辑均基于 ECS 构建。

**核心特性：**

- **Archetype 存储** — 相同组件组合的实体连续存储，Cache-Friendly 内存布局
- **组件即数据** — 纯数据结构，无函数指针，零虚函数开销
- **系统即逻辑** — 按组件查询批量处理实体，数据局部性最优
- **动态 AOI** — 兴趣区域（Area of Interest）半径 100，仅同步可见实体
- **Cell 空间划分** — 支持 2D / 3D，按区域分片管理实体

```c
#include "ce_ecs.h"

/* 创建世界 */
ce_ecs_world_t* world = ce_ecs_create();

/* 注册组件 */
ce_ecs_register(world, CE_COMP_TRANSFORM, sizeof(transform_t));
ce_ecs_register(world, CE_COMP_MOVEMENT, sizeof(movement_t));
ce_ecs_register(world, CE_COMP_RENDER,   sizeof(render_t));

/* 创建实体并挂载组件 */
ce_entity_t e = ce_ecs_spawn(world);
ce_ecs_set(world, e, CE_COMP_TRANSFORM, &(transform_t){.x=0, .y=0, .z=0});

/* 系统迭代：查询所有含 Transform + Movement 的实体 */
ce_ecs_iter_t it = ce_ecs_query(world, CE_COMP_TRANSFORM | CE_COMP_MOVEMENT);
while (ce_ecs_next(&it)) {
    transform_t* t = ce_ecs_get(&it, CE_COMP_TRANSFORM);
    movement_t*  m = ce_ecs_get(&it, CE_COMP_MOVEMENT);
    /* 更新位置 */
    t->x += m->vx * dt;
    t->z += m->vz * dt;
}
```

**单元测试覆盖：** `test_aoi` / `test_ecs` / `test_cell`

---

### 2. 网络协议栈

ChaosEngine 采用自定义二进制协议，支持 TCP / KCP / WebSocket 三种传输层，Gateway 统一收发。

**消息格式：**

```
┌────────────────────────────────────────┐
│  total_len (4B)  │  type (2B)  │ payload │
│     总长度        │   消息类型   │   负载   │
└────────────────────────────────────────┘
```

**消息类型：**

| 类型 ID | 消息 | 方向 | 说明 |
|---------|------|------|------|
| `0x0001` | PING | 客户端 → 服务端 | 心跳探测 |
| `0x0002` | PONG | 服务端 → 客户端 | 心跳响应 |
| `0x0010` | LOGIN | 客户端 → 服务端 | 登录请求 |
| `0x0011` | LOGIN_RESP | 服务端 → 客户端 | 登录响应 |
| `0x0020` | JOIN | 客户端 → 服务端 | 加入游戏世界 |
| `0x0021` | ENTITY_UPDATE | 服务端 → 客户端 | 实体状态同步 |

**三协议对比：**

| 协议 | 传输层 | 特点 | 适用场景 |
|------|--------|------|----------|
| TCP | SOCK_STREAM | 可靠有序，系统默认 | 内部服务通信 |
| KCP | UDP | 可靠 UDP，低延迟 | 实时游戏通信 |
| WebSocket | TCP | 浏览器兼容 | Web 客户端 / H5 游戏 |

---

### 3. 高性能 IO

ChaosEngine 在 Linux 平台上利用内核态技术实现极致网络性能，并提供 POSIX 回退方案。

| 技术 | 层级 | 说明 | 性能 |
|------|------|------|------|
| **io_uring** | Linux 5.1+ | 零拷贝接收（ZRX），异步提交队列 | P50 延迟 1µs，比 POSIX 快 100x |
| **eBPF/XDP** | Linux 内核 | 内核态包过滤与分发，绕过内核协议栈 | 纳秒级包处理 |
| **POSIX AIO** | 跨平台 | 异步 IO 回退方案，兼容 macOS / Windows | 通用兼容 |

```
数据包流向 (Linux 高性能模式):

  网卡 RX → XDP/eBPF (内核态过滤) → io_uring SQ → 用户态处理 → io_uring CQ
                                    ↑                          ↓
                              绕过协议栈                零拷贝接收 (ZRX)
```

**单元测试覆盖：** `test_network` / `test_async_io`

---

### 4. 网关服务

`chaos_gateway` 是集群的统一入口，负责协议路由、连接管理和心跳维护。Gateway 同时绑定 TCP / KCP / WebSocket 三种协议，自动识别连接类型并路由到对应处理逻辑。

**Gateway Lua 服务模块：**

| 模块 | 职责 |
|------|------|
| `connection` | 连接生命周期管理 |
| `heartbeat` | 心跳检测与超时清理 |
| `websocket` | WebSocket 协议处理 |
| `kcp_server` | KCP 协议处理 |
| `router` | 内部消息路由 |
| `game_connector` | 与 Game Server 的连接维护 |

**万并发优化成果：**

| 指标 | 优化前 | 优化后 | 提升 |
|------|--------|--------|------|
| QPS | 14,819 | 97,564 | **6.6x** |
| P50 延迟 | 621ms | 29ms | **21x** |
| P99 延迟 | 638ms | 63ms | **10x** |

---

### 5. 游戏服务器

`chaos_server` 承载核心游戏逻辑，集成 AOI 和 Cell 空间划分。

- **AOI（兴趣区域）** — 动态半径 100，仅同步玩家可见范围内的实体
- **Cell 空间划分** — 将游戏世界划分为网格 Cell，按 Cell 分片管理实体，支持 2D / 3D
- **游戏逻辑执行** — ECS 系统驱动，帧率独立的逻辑更新
- **实体管理** — 实体的创建、销毁、属性变更

---

### 6. 数据库代理

`chaos_dbproxy` 提供数据持久化层，通过状态镜像机制实现高效读写。

**DBProxy Lua 服务模块：**

| 模块 | 职责 |
|------|------|
| `handler` | 读写请求处理 |
| `heartbeat` | 心跳保活 |
| `protocol` | 数据序列化协议 |
| `state_mirror` | 内存状态镜像，减少磁盘 IO |

---

### 7. 路由服务

`chaos_router` 负责跨服路由和服务发现，采用一致性哈希算法。

**Router Lua 服务模块：**

| 模块 | 职责 |
|------|------|
| `cluster` | 集群节点管理 |
| `cross_region` | 跨区域消息路由 |
| `hash_ring` | 一致性哈希环 |
| `health` | 节点健康检查 |
| `registry` | 服务注册与发现 |

---

### 8. 管理后台

`chaos_admin` 基于 Lapis Web 框架，提供 HTTP 管理接口和 WebSocket 实时 Dashboard。

**Admin Lua 服务模块：**

| 模块 | 职责 |
|------|------|
| `app` | Lapis Web 应用入口 |
| `ws_handler` | WebSocket 连接处理 |
| `dashboard_html` | Dashboard 页面渲染 |

---

### 9. Vulkan 渲染

ChaosEngine 通过 RHI（Rendering Hardware Interface）抽象层封装图形 API，当前实现 Vulkan 后端。

| 组件 | 文件 | 说明 |
|------|------|------|
| RHI 抽象层 | `ce_rhi.h` | 跨 API 接口定义 |
| Vulkan 后端 | `ce_rhi_vulkan.c` | Vulkan 实现 |
| 渲染管线 | `ce_render.c` | 渲染流程编排 |

- **60 FPS** 稳定渲染
- 支持窗口模式和全屏模式
- X11 窗口系统集成（Linux）

---

### 10. 同步与复制

ChaosEngine 采用**客户端帧同步 + 服务端状态同步**的双重同步机制：

| 机制 | 运行端 | 说明 |
|------|--------|------|
| 帧同步 | 客户端 | 本地预测渲染，降低操作延迟感 |
| 状态同步 | 服务端 | 权威状态推送给所有客户端 |
| 属性复制 | 双端 | 实体属性自动复制 (Replication) |

**消息流：**

```
客户端 LOGIN → 服务端 LOGIN_RESP → 客户端 JOIN → 服务端 ENTITY_UPDATE (持续同步)
```

**单元测试覆盖：** `test_replication`

---

### 11. Lua 服务化

ChaosEngine 借鉴 Skynet 的 Actor 模型，将网络服务逻辑下沉到 Lua 层，实现热更新和灵活配置。每个服务进程加载对应的 Lua 服务脚本：

| 服务进程 | Lua 模块 | 职责 |
|----------|----------|------|
| `chaos_gateway` | gateway/ | 连接管理、心跳、协议处理、路由 |
| `chaos_router` | router/ | 集群管理、跨区路由、一致性哈希 |
| `chaos_dbproxy` | dbproxy/ | 数据读写、状态镜像、持久化 |
| `chaos_admin` | admin/ | Web 管理后台、WebSocket Dashboard |
| 游戏服务 | services/ | 好友系统等游戏逻辑服务 |

Lua 服务共享公共模块 `shared/`，避免重复代码。

---

### 12. eBPF/XDP 加速

ChaosEngine 在 Linux 内核态通过 eBPF/XDP 技术实现高性能包处理：

- **XDP 挂载点** — 网卡 RX 队列，数据包进入协议栈之前拦截
- **eBPF 程序** — 内核态执行，过滤和分发数据包
- **绕过协议栈** — 匹配的包直接投递到用户态，减少内核切换开销
- **POSIX 回退** — 非 Linux 平台自动回退到 POSIX 异步 IO

---

## LoopEngine 闭环验证

ChaosEngine 配套 [LoopEngine](loop-engine/) — Python 实现的四域闭环自动化验证引擎，确保每次提交的代码质量。

| 闭环域 | 验证内容 | 状态 |
|--------|----------|------|
| **dev** | 二进制编译验证 | ✅ |
| **test** | GitHub Actions CI 触发 + 轮询 + 结果解析 | ✅ |
| **verify** | 集群启动 + 冒烟测试 + Admin API 验证 + 实体同步检查 | ✅ |
| **client_ui** | Vulkan 客户端启动 + 窗口检测 + 结构化断言 + 视觉复核 | ✅ |

**特性：**

- 自动重试机制
- Wayland 兼容
- Markdown / JSON 格式报告
- **四域全绿总耗时：197 秒**

---

## CI/CD

GitHub Actions 流水线包含 5 个 Job：

| # | Job | 内容 |
|---|-----|------|
| 1 | 编译 & 测试 (Debug) | CMake 编译 + ctest 单元测试 |
| 2 | Release 编译 | Release 模式全量编译 |
| 3 | Lua 语法检查 | `luac` 语法验证所有 Lua 脚本 |
| 4 | Gateway 集成测试 | 网关服务集成测试 |
| 5 | 内存检查 | Valgrind 内存泄漏检测 |

---

## 性能基准

### Gateway 万并发优化

| 指标 | 优化前 | 优化后 | 提升倍数 |
|------|--------|--------|----------|
| QPS | 14,819 | 97,564 | 6.6x |
| P50 延迟 | 621 ms | 29 ms | 21x |
| P99 延迟 | 638 ms | 63 ms | 10x |

### IO 性能对比

| IO 方案 | P50 延迟 | 相对 POSIX |
|---------|----------|------------|
| POSIX 异步 IO | ~100 µs | 1x (基准) |
| io_uring (ZRX) | ~1 µs | 100x |

### 渲染性能

| 指标 | 数值 |
|------|------|
| 目标帧率 | 60 FPS |
| 渲染后端 | Vulkan |
| 窗口模式 | 窗口 / 全屏 |

### 项目规模

| 统计项 | 数值 |
|--------|------|
| C / C++ / Lua 源文件 | 374 |
| 单元测试 | 22 |
| 服务进程 | 6 |
| 网络协议 | 3 (TCP / KCP / WebSocket) |

---

## 快速开始

### 环境要求

- CMake 3.20+
- C99 兼容编译器 (GCC / Clang / MSVC)
- C++17 兼容编译器（编辑器）
- Lua 5.4
- Vulkan SDK（客户端渲染）
- Linux 5.1+（io_uring，可选）

### 编译

```bash
# 克隆仓库
git clone https://github.com/Fandel-Chuang/chaos-engine.git
cd chaos-engine

# 编译（含编辑器）
mkdir build && cd build
cmake .. -DCHAOS_BUILD_EDITOR=ON
cmake --build . -j$(nproc)

# 编译测试
cmake .. -DCHAOS_BUILD_EDITOR=ON -DCHAOS_BUILD_TESTS=ON
cmake --build . -j$(nproc)
ctest --output-on-failure
```

### 启动集群

```bash
# 一键启动全部服务
./scripts/start_cluster.sh --all
# 启动: game(7777) + gateway(9000/9001/9002) + dbproxy(9003) + router(9100) + admin(9090)

# 单独启动某个服务
./scripts/start_cluster.sh --gateway
./scripts/start_cluster.sh --server
./scripts/start_cluster.sh --dbproxy
./scripts/start_cluster.sh --router
./scripts/start_cluster.sh --admin
```

### 启动客户端

```bash
# Vulkan 客户端，连接本地网关
./scripts/start_client.sh --vulkan --connect 127.0.0.1:9000

# 验证客户端同步
./scripts/verify_client_sync.sh
```

### 运行编辑器

```bash
./bin/chaos_editor
```

---

## 编译选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `CHAOS_BUILD_EDITOR` | ON | 编译 C++ 编辑器 |
| `CHAOS_BUILD_TESTS` | OFF | 编译单元测试 |
| `CHAOS_BUILD_SAMPLES` | OFF | 编译示例程序 |
| `CHAOS_USE_DOUBLE_PROCESS` | OFF | 双进程 IPC 模式 |

---

## 项目结构

```
chaos-engine/
├── src_c/                  # 纯 C99 内核
│   ├── core/               #   核心基础设施
│   ├── ecs/                #   ECS 架构
│   ├── render/             #   渲染 (RHI Vulkan)
│   ├── network/            #   网络协议栈
│   ├── gateway/            #   网关服务
│   ├── server/             #   游戏服务器
│   ├── sync/               #   同步机制
│   ├── replication/        #   属性复制
│   ├── ebpf/               #   eBPF/XDP 加速
│   ├── dbproxy/            #   数据库代理
│   ├── script/             #   Lua 脚本绑定
│   ├── save/               #   存档系统
│   ├── log/                #   日志系统
│   └── plugin/             #   插件系统
├── src_cpp/                # C++17 编辑器
│   ├── editor_logic/       #   编辑器逻辑
│   ├── importer/           #   资源导入
│   ├── log_observer/       #   日志观测
│   └── ui/                 #   UI 面板
├── src_lua/                # Lua 服务脚本
│   ├── gateway/            #   网关服务 (6 模块)
│   ├── router/             #   路由服务 (5 模块)
│   ├── dbproxy/            #   数据库代理 (4 模块)
│   ├── admin/              #   管理后台 (3 模块)
│   ├── services/           #   游戏逻辑服务
│   └── shared/             #   公共模块
├── loop-engine/            # LoopEngine 闭环验证 (Python)
├── scripts/                # 运维脚本
│   ├── start_cluster.sh    #   集群启动
│   ├── start_client.sh     #   客户端启动
│   └── verify_client_sync.sh
├── tests/                  # 单元测试 + 基准测试
├── ci/                     # CI 辅助
├── docs/                   # 设计规格 + 基准报告
├── CMakeLists.txt          # CMake 构建配置
├── Dockerfile              # 容器化
└── LICENSE                 # MIT 许可证
```

---

## 参考引擎

ChaosEngine 在设计中借鉴了以下优秀开源引擎的理念：

| 引擎 | 借鉴内容 |
|------|----------|
| [Skynet](https://github.com/cloudwu/skynet) | Actor 并发模型、Lua 服务化 |
| [KBEngine](https://github.com/kbengine/kbengine) | Cell 空间划分、动态 AOI |
| [Ant Engine](https://github.com/eclipse-arch/ant) | 纯 ECS 架构、Archetype 存储 |
| Unreal Engine | Actor / Component 概念、属性复制 |

---

## 许可证

[MIT License](LICENSE) © ChaosEngine Contributors

---

<div align="center">

**ChaosEngine v0.1.0** · 纯 C 内核 · Lua 服务化 · Vulkan 渲染 · io_uring 加速

</div>
