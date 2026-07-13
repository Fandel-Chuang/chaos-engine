# ChaosEngine C 版 Gateway 架构规格说明书

> **状态：** 草案 | **日期：** 2026-07-13 | **作者：** 老陈
>
> **主题：** 用纯 C99 实现 Gateway 进程，替代原 Lua 方案，追求极致效率

---

## 目录

1. [设计目标与背景](#1-设计目标与背景)
2. [架构总览](#2-架构总览)
3. [核心数据结构](#3-核心数据结构)
4. [协议设计](#4-协议设计)
5. [文件规划](#5-文件规划)
6. [模块详细设计](#6-模块详细设计)
7. [与现有代码的复用关系](#7-与现有代码的复用关系)
8. [CMake 集成方案](#8-cmake-集成方案)
9. [客户端改造点](#9-客户端改造点)
10. [MVP Phase 1 范围](#10-mvp-phase-1-范围)
11. [后续 Phase 规划](#11-后续-phase-规划)
12. [验收标准](#12-验收标准)

---

## 1. 设计目标与背景

### 1.1 背景

原 Gateway 设计采用 Lua 驱动方案（`src_lua/gateway/init.lua` + `ce_gateway_main.c` 加载 Lua 脚本）。用户要求改为纯 C 实现，理由：

- **极致效率**：消除 Lua VM 开销，减少内存分配和 GC 停顿
- **零拷贝友好**：C 层可直接操作 socket 缓冲区指针，避免 Lua string 来回拷贝
- **统一技术栈**：与 `ce_net_base`、`ce_async_io` 等已有 C 库无缝衔接
- **部署简化**：无需打包 Lua 运行时

### 1.2 设计约束

| 约束 | 说明 |
|------|------|
| 语言标准 | 纯 C99（`-std=c99`），禁止 C++ 语法 |
| 命名规范 | `ce_` 前缀，`ce_gateway_` 模块前缀 |
| 线程模型 | 单线程 epoll 事件循环（io_uring 可选加速） |
| 内存管理 | 启动时预分配连接池，运行时零动态分配（理想目标） |
| 协议兼容 | 客户端协议与 `ce_client_network.h` 兼容 |
| 后端协议 | 复用 `ce_net_base` 的 `[4B total_len][2B msg_type][N payload]` 格式 |

### 1.3 原 Lua spec 要点（需改为 C 实现）

- 多协议接入：TCP(9000) + KCP(9001) + WebSocket(9002)
- 消息路由：`msg_type -> 后端地址` 映射表
- 连接管理：心跳(30s)、超时(90s)、连接数上限(10000)
- 后端连接池：到 Game/DBProxy/Admin 的 TCP 长连接 + 健康检查
- 优雅关闭：SIGTERM 停 accept，等现有连接处理完

---

## 2. 架构总览

### 2.1 进程模型

```
                        ChaosEngine Gateway 进程 (chaos_gateway)
  ┌──────────────────────────────────────────────────────────────────────┐
  │                                                                      │
  │  ┌─────────┐   ┌──────────────────────────────────────────────────┐ │
  │  │ 信号处理 │   │           单线程 epoll 事件循环                    │ │
  │  │ SIGTERM │   │                                                    │ │
  │  │ SIGINT  │   │  ┌──────────┐  ┌──────────┐  ┌───────────────┐  │ │
  │  └────┬────┘   │  │ TCP 监听  │  │ 后端连接  │  │ 定时器 (timerfd)│  │ │
  │       │        │  │ :9000    │  │ 管理器    │  │ 心跳/超时/健康 │  │ │
  │       v        │  └────┬─────┘  └────┬─────┘  └───────┬───────┘  │ │
  │  g_running=0   │       │             │                │           │ │
  │       │        │       v             v                v           │ │
  │       └────────│  ┌──────────────────────────────────────────┐    │ │
  │                │  │        事件分发器 (epoll_wait)             │    │ │
  │                │  └──────────┬───────────────────────────────┘    │ │
  │                │             │                                     │ │
  │                │     ┌───────┴────────┐                            │ │
  │                │     v                v                            │ │
  │                │  客户端 FD        后端 FD                         │ │
  │                │     │                │                            │ │
  │                │     v                v                            │ │
  │                │  ┌────────┐    ┌───────────┐                      │ │
  │                │  │连接表   │    │后端连接池  │                      │ │
  │                │  │fd->Conn│    │(Game/DB)  │                      │ │
  │                │  └────┬───┘    └─────┬─────┘                      │ │
  │                │       │              │                             │ │
  │                │       v              v                             │ │
  │                │  ┌────────────────────────┐                        │ │
  │                │  │    路由表 (msg_type)    │                        │ │
  │                │  │  二分查找 -> backend_id │                        │ │
  │                │  └────────────────────────┘                        │ │
  │                └──────────────────────────────────────────────────┘ │
  └──────────────────────────────────────────────────────────────────────┘
          │                                    │
          v                                    v
   ┌──────────────┐                    ┌───────────────┐
   │  客户端       │                    │  Game Server   │
   │  (chaos_     │                    │  :7777         │
   │   client)    │                    ├───────────────┤
   │              │                    │  DBProxy       │
   └──────────────┘                    │  :9003         │
                                       ├───────────────┤
                                       │  Admin         │
                                       └───────────────┘
```

### 2.2 线程模型：单线程 epoll + 非阻塞 IO

**选择理由：**

- Gateway 是 IO 密集型进程，CPU 计算量极小（路由 + 转发）
- 单线程消除锁竞争，连接表和路由表无需加锁
- Linux epoll 在 10K 连接级别性能优异（O(1) 事件通知）
- `ce_async_io.h` 已封装 epoll/io_uring 双后端，可无缝切换

**事件循环伪代码：**

```c
while (g_running) {
    int n = epoll_wait(epfd, events, MAX_EVENTS, timeout_ms);

    for (int i = 0; i < n; i++) {
        int fd = events[i].data.fd;
        uint32_t ev = events[i].events;

        if (fd == listen_fd) {
            gateway_handle_accept(gw);          /* 新客户端连接 */
        } else if (fd == timer_fd) {
            gateway_handle_timer(gw);           /* 心跳/超时/健康检查 */
        } else if (fd == backend_health_fd) {
            gateway_backend_health_check(gw);   /* 后端健康检查 */
        } else if (gateway_conn_table_lookup(fd)) {
            /* 客户端连接事件 */
            if (ev & EPOLLIN)  gateway_handle_client_read(gw, fd);
            if (ev & EPOLLOUT) gateway_handle_client_write(gw, fd);
            if (ev & (EPOLLHUP | EPOLLERR)) gateway_close_client(gw, fd);
        } else if (gateway_backend_lookup_fd(fd)) {
            /* 后端连接事件 */
            if (ev & EPOLLIN)  gateway_handle_backend_read(gw, fd);
            if (ev & EPOLLOUT) gateway_handle_backend_write(gw, fd);
            if (ev & (EPOLLHUP | EPOLLERR)) gateway_backend_reconnect(gw, fd);
        }
    }

    /* 处理待关闭连接 */
    gateway_process_close_queue(gw);
}
```

### 2.3 io_uring 可选加速

当编译时检测到 `liburing`（`CHAOS_HAS_IO_URING` 宏），事件循环可切换为 io_uring 后端：

- 使用 `IORING_ACCEPT_SQ POLL` 模式减少系统调用
- 利用 ` Registered Buffers` 实现零拷贝接收
- `ce_async_io.h` 已封装此能力，Gateway 可直接复用

MVP Phase 1 先用 epoll，io_uring 作为性能优化选项在 Phase 3 启用。

### 2.4 端口规划

| 端口 | 协议 | 方向 | Phase | 说明 |
|------|------|------|-------|------|
| 9000 | TCP | 客户端 → Gateway | Phase 1 | 主要接入端口 |
| 9001 | KCP (UDP) | 客户端 → Gateway | Phase 2 | 可靠 UDP，低延迟 |
| 9002 | WebSocket | 客户端 → Gateway | Phase 3 | Web 客户端接入 |
| - | TCP | Gateway → Game:7777 | Phase 1 | 后端长连接 |
| - | TCP | Gateway → DBProxy:9003 | Phase 1 | 后端长连接 |
| - | TCP | Gateway → Admin | Phase 2 | 管理 IPC |

---

## 3. 核心数据结构

### 3.1 Gateway 上下文

```c
/*
 * Gateway 全局上下文
 * 单例，整个进程生命周期内存在一个实例
 */
typedef struct CeGateway {
    /* ---- 事件循环 ---- */
    int                 epfd;               /* epoll 实例 fd */
    int                 listen_fd;          /* TCP 监听 fd (:9000) */
    int                 timer_fd;           /* timerfd，用于周期性任务 */

    /* ---- 配置 ---- */
    CeGatewayConfig     config;             /* 启动配置（见 3.2） */

    /* ---- 客户端连接表 ---- */
    CeGatewayConnTable* conn_table;         /* fd -> CeGatewayConn* 哈希表 */

    /* ---- 路由表 ---- */
    CeGatewayRouter*    router;             /* msg_type -> backend 路由表 */

    /* ---- 后端连接池 ---- */
    CeGatewayBackend*   backend;            /* 后端连接池管理器 */

    /* ---- 统计 ---- */
    CeGatewayStats      stats;              /* 全局统计（见 3.6） */

    /* ---- 运行状态 ---- */
    volatile int        running;            /* 主循环运行标志 */
    int                 shutting_down;      /* 优雅关闭标志 */
    int                 active_conns;       /* 当前活跃连接数 */
} CeGateway;
```

### 3.2 Gateway 配置

```c
/*
 * Gateway 启动配置
 * 通过命令行参数或配置文件填充
 */
typedef struct CeGatewayConfig {
    /* ---- 监听配置 ---- */
    const char* listen_host;                /* 监听地址，默认 "0.0.0.0" */
    int         listen_port;                /* TCP 监听端口，默认 9000 */
    int         kcp_port;                   /* KCP 端口，默认 9001 (Phase 2) */
    int         ws_port;                    /* WebSocket 端口，默认 9002 (Phase 3) */

    /* ---- 连接限制 ---- */
    int         max_connections;            /* 最大客户端连接数，默认 10000 */
    int         backlog;                    /* listen backlog，默认 512 */

    /* ---- 心跳/超时 ---- */
    int         heartbeat_interval_ms;      /* 心跳发送间隔，默认 30000 (30s) */
    int         conn_timeout_ms;            /* 连接空闲超时，默认 90000 (90s) */
    int         backend_health_interval_ms; /* 后端健康检查间隔，默认 5000 (5s) */

    /* ---- 缓冲区 ---- */
    int         recv_buf_size;              /* 单连接接收缓冲区，默认 64KB */
    int         send_buf_size;             /* 单连接发送缓冲区，默认 64KB */

    /* ---- 后端配置 ---- */
    CeGatewayBackendConfig backends[CE_GW_MAX_BACKENDS]; /* 后端列表 */
    int                    backend_count;               /* 后端数量 */
} CeGatewayConfig;

#define CE_GW_MAX_BACKENDS  16   /* 最多后端数 */

/* 后端配置 */
typedef struct CeGatewayBackendConfig {
    CeGatewayBackendType type;              /* 后端类型（GAME/DBPROXY/ADMIN） */
    const char*          host;              /* 后端主机 */
    int                  port;              /* 后端端口 */
    int                  pool_size;         /* 连接池大小，默认 4 */
} CeGatewayBackendConfig;

/* 后端类型枚举 */
typedef enum CeGatewayBackendType {
    CE_GW_BACKEND_GAME    = 0,              /* Game Server (127.0.0.1:7777) */
    CE_GW_BACKEND_DBPROXY = 1,              /* DBProxy (127.0.0.1:9003) */
    CE_GW_BACKEND_ADMIN   = 2,              /* Admin IPC */
} CeGatewayBackendType;
```

### 3.3 Connection 对象（客户端连接）

```c
/*
 * 客户端连接对象
 * 每个接入的客户端连接创建一个实例
 * 存储在 conn_table 哈希表中，以 fd 为 key
 */
typedef struct CeGatewayConn {
    /* ---- 网络层 ---- */
    int                 fd;                 /* socket 文件描述符 */
    struct sockaddr_in  addr;               /* 客户端地址 */
    char                addr_str[64];       /* "ip:port" 字符串，用于日志 */

    /* ---- 状态 ---- */
    CeGatewayConnState  state;              /* 连接状态（见下） */
    uint64_t            connect_time_us;    /* 连接建立时间（单调时钟） */
    uint64_t            last_active_us;     /* 最后活跃时间（收到任何数据） */
    uint64_t            last_heartbeat_us;  /* 最后发送心跳时间 */

    /* ---- 接收缓冲区（零拷贝思路） ---- */
    /*
     * 接收缓冲区设计：
     * - 使用环形缓冲区 (ring buffer)
     * - recv() 直接写入 buf 的可写区域
     * - 解析消息头时直接在 buf 上操作，不 memcpy payload
     * - 只有需要转发到后端时，才将 payload 指针传给后端 send
     */
    uint8_t*            recv_buf;           /* 接收缓冲区（堆分配） */
    int                 recv_buf_size;      /* 缓冲区总大小 */
    int                 recv_offset;        /* 已写入数据的尾部偏移 */
    int                 recv_consumed;      /* 已消费（解析）的数据偏移 */

    /* ---- 发送缓冲区 ---- */
    /*
     * 发送缓冲区设计：
     * - 当 socket 不可写时，数据暂存在 send_buf
     * - 注册 EPOLLOUT，socket 可写后冲刷
     * - send_buf 满时触发背压（丢弃或断开）
     */
    uint8_t*            send_buf;           /* 发送缓冲区（堆分配） */
    int                 send_buf_size;      /* 缓冲区总大小 */
    int                 send_offset;        /* 待发送数据长度 */
    int                 send_writing;       /* 是否正在异步写入中 */

    /* ---- 路由关联 ---- */
    /*
     * 当客户端消息被路由到某个后端时，记录关联关系
     * 后端响应返回时，通过 pending_list 找到源客户端
     */
    CeGatewayBackendType  routed_backend;   /* 当前消息路由到的后端 */
    uint64_t              request_id;       /* 请求 ID（用于匹配响应） */

    /* ---- 统计 ---- */
    uint64_t            bytes_recv;         /* 接收总字节数 */
    uint64_t            bytes_sent;         /* 发送总字节数 */
    uint32_t            msgs_recv;          /* 接收消息数 */
    uint32_t            msgs_sent;          /* 发送消息数 */

    /* ---- 哈希表链（开链法） ---- */
    struct CeGatewayConn* hash_next;        /* 哈希冲突链表下一个节点 */
} CeGatewayConn;

/* 连接状态 */
typedef enum CeGatewayConnState {
    CE_GW_CONN_NONE       = 0,              /* 未初始化 */
    CE_GW_CONN_CONNECTING = 1,              /* 连接中（accept 尚未完成） */
    CE_GW_CONN_ACTIVE     = 2,              /* 活跃，正常通信 */
    CE_GW_CONN_CLOSING    = 3,              /* 正在关闭（等待发送完成） */
    CE_GW_CONN_CLOSED     = 4,              /* 已关闭，待回收 */
} CeGatewayConnState;
```

### 3.4 连接表（哈希表：fd -> CeGatewayConn*）

```c
/*
 * 客户端连接哈希表
 * Key = fd (int), Value = CeGatewayConn*
 * 使用开链法处理冲突
 *
 * 哈希函数：fd % bucket_count
 * fd 在同一时刻全局唯一，无碰撞（除非 fd 复用）
 * 查找复杂度：O(1) 平均
 *
 * 桶数建议：max_connections 的 2 倍，取 2 的幂（便于取模优化为位与）
 */
typedef struct CeGatewayConnTable {
    CeGatewayConn**     buckets;            /* 桶数组 */
    int                 bucket_count;       /* 桶数（2 的幂） */
    int                 bucket_mask;         /* bucket_count - 1，位与掩码 */
    int                 count;              /* 当前连接数 */
    int                 max_count;          /* 最大连接数 */
} CeGatewayConnTable;

/*
 * 连接表操作函数（声明在 ce_gateway_conn.h 内部或 ce_gateway.h 中）
 */
CeGatewayConnTable* ce_gateway_conn_table_create(int max_connections);
void                ce_gateway_conn_table_destroy(CeGatewayConnTable* table);
CeGatewayConn*      ce_gateway_conn_table_lookup(CeGatewayConnTable* table, int fd);
CeResult            ce_gateway_conn_table_insert(CeGatewayConnTable* table, CeGatewayConn* conn);
CeResult            ce_gateway_conn_table_remove(CeGatewayConnTable* table, int fd);

/* 连接对象生命周期 */
CeGatewayConn* ce_gateway_conn_create(int fd, const struct sockaddr_in* addr,
                                       int recv_buf_size, int send_buf_size);
void            ce_gateway_conn_destroy(CeGatewayConn* conn);
void            ce_gateway_conn_reset_stats(CeGatewayConn* conn);
```

### 3.5 路由表（数组 + 二分查找）

```c
/*
 * 消息路由表
 * msg_type -> backend_id 映射
 *
 * 使用有序数组 + 二分查找：
 * - msg_type 范围 0x0000-0xFFFF，实际使用的类型有限（通常 < 100 种）
 * - 启动时加载配置，运行时只读，无需加锁
 * - 二分查找 O(log N)，N=路由规则数，通常 < 50，实测 < 6 次比较
 * - 比哈希表更 cache 友好（连续内存）
 */
typedef struct CeGatewayRouteRule {
    uint16_t            msg_type;           /* 客户端消息类型 */
    uint16_t            backend_id;         /* 目标后端 ID（CeGatewayBackendConfig 数组索引） */
    uint8_t             flags;              /* 路由标志（见下） */
    uint8_t             reserved[3];        /* 对齐填充 */
} CeGatewayRouteRule;

/* 路由标志 */
#define CE_GW_ROUTE_FLAG_NONE         0x00  /* 无特殊标志 */
#define CE_GW_ROUTE_FLAG_NEED_RESP    0x01  /* 需要后端响应，记录请求关联 */
#define CE_GW_ROUTE_FLAG_BROADCAST    0x02  /* 广播到所有同类后端 */
#define CE_GW_ROUTE_FLAG_DROP_ON_ERR  0x04  /* 后端不可用时丢弃消息（默认断开客户端） */

/*
 * 路由表结构
 */
typedef struct CeGatewayRouter {
    CeGatewayRouteRule* rules;              /* 有序规则数组（按 msg_type 升序） */
    int                 rule_count;         /* 规则数量 */
    int                 rule_capacity;      /* 数组容量 */
} CeGatewayRouter;

/*
 * 路由表操作函数
 */
CeGatewayRouter* ce_gateway_router_create(void);
void              ce_gateway_router_destroy(CeGatewayRouter* router);

/* 添加路由规则（会自动按 msg_type 排序） */
CeResult ce_gateway_router_add(CeGatewayRouter* router,
                                uint16_t msg_type, uint16_t backend_id,
                                uint8_t flags);

/* 二分查找路由 */
const CeGatewayRouteRule* ce_gateway_router_lookup(
    const CeGatewayRouter* router, uint16_t msg_type);

/* 批量加载路由配置 */
CeResult ce_gateway_router_load_config(CeGatewayRouter* router,
                                        const CeGatewayRouteRule* rules,
                                        int count);
```

### 3.6 后端连接池

```c
/*
 * 后端连接池管理器
 * 管理到 Game/DBProxy/Admin 的 TCP 长连接
 *
 * 设计要点：
 * - 每个后端类型维护 pool_size 条 TCP 长连接
 * - 轮询策略选择连接发送消息
 * - 连接断开时自动重连（指数退避）
 * - 健康检查通过定时发送 PING 实现
 */
typedef struct CeGatewayBackend {
    CeGatewayBackendPool*   pools;          /* 后端连接池数组 */
    int                     pool_count;     /* 池数量（= 后端数量） */
    int                     pool_capacity;  /* 数组容量 */

    /* 响应路由表：request_id -> 客户端 fd */
    /*
     * 当客户端消息转发到后端时，记录 request_id -> client_fd 映射
     * 后端响应返回时，通过 request_id 找到目标客户端
     * 使用简单数组 + 线性探测哈希表，避免动态分配
     */
    CeGatewayPendingReq*    pending;        /* 待响应请求表 */
    int                     pending_count;  /* 待响应请求数 */
    int                     pending_capacity;
} CeGatewayBackend;

/*
 * 单个后端的连接池
 */
typedef struct CeGatewayBackendPool {
    CeGatewayBackendType    type;           /* 后端类型 */
    char                    host[256];      /* 后端地址 */
    int                     port;           /* 后端端口 */

    CeGatewayBackendConn*   conns;          /* 连接数组 */
    int                     conn_count;     /* 连接数 */
    int                     pool_size;      /* 池大小 */
    int                     round_robin;    /* 轮询索引 */

    /* 健康检查 */
    uint64_t                last_health_check_us;  /* 最后健康检查时间 */
    int                     healthy;        /* 是否健康（0=不可用, 1=正常） */
} CeGatewayBackendPool;

/*
 * 后端单条连接
 */
typedef struct CeGatewayBackendConn {
    int                 fd;                 /* socket fd */
    CeGatewayConnState  state;              /* 连接状态 */

    /* 接收缓冲区（后端响应） */
    uint8_t*            recv_buf;
    int                 recv_buf_size;
    int                 recv_offset;
    int                 recv_consumed;

    /* 统计 */
    uint64_t            bytes_sent;
    uint64_t            bytes_recv;
    uint32_t            msgs_sent;
    uint32_t            msgs_recv;

    /* 重连 */
    uint64_t            last_reconnect_us;  /* 最后重连时间 */
    int                 reconnect_attempts; /* 重连次数 */
} CeGatewayBackendConn;

/*
 * 待响应请求（用于后端响应路由回客户端）
 */
typedef struct CeGatewayPendingReq {
    uint64_t            request_id;         /* 请求 ID */
    int                 client_fd;          /* 源客户端 fd */
    CeGatewayBackendType backend_type;     /* 目标后端类型 */
    uint64_t            timestamp_us;       /* 请求时间（用于超时） */
    int                 in_use;             /* 槽位是否被占用 */
} CeGatewayPendingReq;

/*
 * 全局统计
 */
typedef struct CeGatewayStats {
    uint64_t    total_connections;          /* 历史总连接数 */
    uint64_t    total_messages_routed;      /* 路由消息总数 */
    uint64_t    total_bytes_proxied;        /* 代理转发总字节数 */
    uint64_t    total_errors;              /* 错误总数 */
    uint64_t    total_heartbeats_sent;     /* 发送心跳总数 */
    uint64_t    total_timeouts;            /* 超时断开数 */
    uint64_t    total_backend_reconnects;  /* 后端重连次数 */
    int         current_connections;       /* 当前连接数 */
} CeGatewayStats;
```

---

## 4. 协议设计

### 4.1 协议帧格式统一

现有代码库中存在两种帧格式，需要统一：

| 来源 | 格式 | 头大小 | 说明 |
|------|------|--------|------|
| `ce_net_base.h` | `[4B total_len][2B msg_type][N payload]` | 6B | total_len = 6 + payload_len，大端序 |
| `ce_client_network.h` | `[2B msg_type][4B body_len][N body]` | 6B | body_len = payload 长度，大端序 |
| 原 Lua spec | `[2B msg_type][4B body_len][N body]` | 6B | 同 ce_client_network |

**决策：Gateway 内部统一使用 `ce_net_base` 格式 `[4B total_len][2B msg_type][N payload]`**

理由：
1. `ce_net_base` 已有完整的 pack/unpack/peek 函数，直接复用
2. `total_len` 包含头部，peek_len 可一步判断消息是否完整
3. 后端（Game Server）已使用此格式（见 `ce_server_main.c`）
4. 客户端 `ce_client_network.h` 的格式差异仅在头部字段顺序，可通过 Gateway 在接入层做一次重排

### 4.2 协议帧结构

```
偏移  长度  字段          说明
──────────────────────────────────────────────────────
0     4     total_len     消息总长度（含头部6字节），大端序
4     2     msg_type      消息类型，大端序
6     N     payload       消息载荷（N = total_len - 6）

约束:
  - total_len >= 6（最小消息：仅头部）
  - total_len <= CE_NET_BASE_MAX_MSG_SIZE (256KB)
  - payload 可以为空（total_len = 6）
```

### 4.3 消息类型分配

```
范围           用途                          Gateway 处理
──────────────────────────────────────────────────────────
0x0001         PING (心跳请求)               Gateway 回复 PONG
0x0002         PONG (心跳响应)               更新客户端活跃时间
0x0010         LOGIN                         路由到 Game Server
0x0011         LOGIN_RESP                    从 Game Server 返回客户端
0x0100         GAME_DATA                     路由到 Game Server
0x8001         MSG_JOIN_REQUEST              路由到 Game Server
0x8002         MSG_JOIN_RESPONSE             从 Game Server 返回客户端
0x8003         MSG_POSITION_UPDATE           路由到 Game Server
0x8004         MSG_ENTITY_STATE              从 Game Server 返回客户端
0xFFFF         DISCONNECT                    Gateway 优雅断开客户端
0x1000-0x1FFF  跨区消息                      路由到 Router (Phase 4)
0x8000+        用户自定义                    按路由表配置转发
```

### 4.4 零拷贝解析流程

```
客户端数据到达
     │
     v
┌──────────────────┐
│ recv() 直接写入   │  ← 不经过中间缓冲区
│ recv_buf 尾部     │
└────────┬─────────┘
         │
         v
┌──────────────────┐
│ peek_len(recv_buf│  ← 仅读 4 字节判断消息完整性
│ + consumed)      │     不拷贝 payload
└────────┬─────────┘
         │
         v
    消息完整?
     │      │
    否      是
     │      │
     v      v
  等待   ┌──────────────────────┐
  更多   │ peek_type() 读 msg_type│  ← 直接在 recv_buf 上读取
  数据   └────────┬─────────────┘
                │
                v
         ┌──────────────────┐
         │ router_lookup()  │  ← 二分查找路由表
         └────────┬─────────┘
                  │
                  v
         ┌──────────────────┐
         │ backend_send()   │  ← payload 指针直接传给后端 send
         │ payload = recv_  │     后端 send 用 writev() 零拷贝发送
         │   buf + 6        │     [后端帧头][原始payload]
         └──────────────────┘
                  │
                  v
         ┌──────────────────┐
         │ recv_consumed += │  ← 移动消费指针，不 memcpy
         │   total_len      │
         └──────────────────┘
```

**关键点：**
- `recv_buf` 中可能包含多个完整消息，循环解析直到数据不完整
- `payload` 指针指向 `recv_buf` 内部，生命周期在下次 `recv()` 之前有效
- 转发到后端时，后端帧头（6B）和 payload 在不同内存区域，使用 `writev()` 一次系统调用发送

### 4.5 writev 零拷贝转发

```c
/*
 * 将客户端消息转发到后端
 * 后端使用 ce_net_base 格式，与客户端帧格式一致
 * 因此可以直接转发原始字节，无需重新打包
 *
 * 但如果客户端帧格式与后端不同（ce_client_network 的 [2B type][4B len]），
 * 则需要在接入层做一次头部重排（6字节，在寄存器中完成，无堆分配）
 */
struct iovec iov[2];

/* 方式 A: 帧格式一致时（理想情况），直接转发 */
iov[0].iov_base = recv_buf + recv_consumed;   /* 完整帧（含头部） */
iov[0].iov_len  = total_len;
writev(backend_fd, iov, 1);

/* 方式 B: 帧格式不一致时，重排头部 */
uint8_t header[6];
/* 原始: [2B msg_type][4B body_len] */
/* 目标: [4B total_len][2B msg_type] */
uint16_t msg_type = read_u16(recv_buf + recv_consumed);
uint32_t body_len = read_u32(recv_buf + recv_consumed + 2);
uint32_t total_len = body_len + 6;
write_u32(header, total_len);
write_u16(header + 4, msg_type);

iov[0].iov_base = header;                     /* 重排后的头部（栈上） */
iov[0].iov_len  = 6;
iov[1].iov_base = recv_buf + recv_consumed + 6; /* 原 payload，零拷贝 */
iov[1].iov_len  = body_len;
writev(backend_fd, iov, 2);
```

---

## 5. 文件规划

### 5.1 新增文件

```
src_c/gateway/
├── ce_gateway.h              # 公开接口（Gateway 上下文、配置、生命周期 API）
├── ce_gateway.c              # 主逻辑（事件循环、信号处理、初始化）
├── ce_gateway_conn.h         # 连接管理内部接口（连接表、Connection 对象）
├── ce_gateway_conn.c         # 连接管理实现
├── ce_gateway_router.h       # 路由表内部接口
├── ce_gateway_router.c       # 路由表实现
├── ce_gateway_backend.h      # 后端连接池内部接口
├── ce_gateway_backend.c      # 后端连接池实现
├── ce_gateway_protocol.h     # 协议编解码内部接口
└── ce_gateway_protocol.c     # 协议编解码实现

src_c/runtime/
└── ce_gateway_main.c         # 进程入口（替换现有 Lua 版本）
```

### 5.2 文件职责

| 文件 | 职责 | 主要结构/函数 |
|------|------|---------------|
| `ce_gateway.h` | 对外公开 API：Gateway 创建/启动/停止 | `CeGateway`, `CeGatewayConfig`, `ce_gateway_create()`, `ce_gateway_run()`, `ce_gateway_stop()` |
| `ce_gateway.c` | 事件循环、信号处理、模块协调 | `ce_gateway_init()`, `ce_gateway_event_loop()`, `ce_gateway_handle_accept()`, `ce_gateway_handle_timer()` |
| `ce_gateway_conn.h/c` | 客户端连接管理：连接表（哈希表）、连接对象生命周期、recv/send 缓冲 | `CeGatewayConn`, `CeGatewayConnTable`, `ce_gateway_conn_table_*()`, `ce_gateway_conn_handle_read()`, `ce_gateway_conn_handle_write()` |
| `ce_gateway_router.h/c` | 消息路由：路由表（有序数组+二分查找）、路由规则加载 | `CeGatewayRouter`, `CeGatewayRouteRule`, `ce_gateway_router_lookup()` |
| `ce_gateway_backend.h/c` | 后端连接池：到 Game/DBProxy 的 TCP 长连接、健康检查、重连、响应路由 | `CeGatewayBackend`, `CeGatewayBackendPool`, `ce_gateway_backend_send()`, `ce_gateway_backend_handle_read()` |
| `ce_gateway_protocol.h/c` | 协议编解码：帧解析、头部重排、消息校验 | `ce_gateway_protocol_parse()`, `ce_gateway_protocol_remap_header()` |
| `ce_gateway_main.c` | `main()` 入口：参数解析、引擎初始化、Gateway 启动 | `main()` |

### 5.3 头文件包含关系

```
ce_gateway_main.c
  └── ce_gateway.h (公开接口)
        ├── ce_gateway_conn.h     (内部)
        │     └── ce_net_base.h   (复用：协议常量、消息类型)
        ├── ce_gateway_router.h   (内部)
        ├── ce_gateway_backend.h  (内部)
        │     └── ce_net_base.h   (复用：CeNetConnection, CeNetPool)
        └── ce_gateway_protocol.h (内部)
              └── ce_net_base.h   (复用：pack/unpack/peek 函数)
```

---

## 6. 模块详细设计

### 6.1 ce_gateway.h — 公开接口

```c
/*
 * ChaosEngine C 版 Gateway - 公开接口
 *
 * 纯 C99，ce_gateway_ 前缀
 * 单线程 epoll 事件循环
 */

#ifndef CE_GATEWAY_H
#define CE_GATEWAY_H

#include "public_api/ce_types.h"
#include "network/ce_net_base.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * 常量
 * ================================================================ */

#define CE_GW_DEFAULT_LISTEN_HOST      "0.0.0.0"
#define CE_GW_DEFAULT_TCP_PORT         9000
#define CE_GW_DEFAULT_KCP_PORT         9001
#define CE_GW_DEFAULT_WS_PORT          9002

#define CE_GW_DEFAULT_MAX_CONNECTIONS  10000
#define CE_GW_DEFAULT_BACKLOG          512

#define CE_GW_DEFAULT_HEARTBEAT_MS     30000   /* 30s */
#define CE_GW_DEFAULT_TIMEOUT_MS       90000   /* 90s */
#define CE_GW_DEFAULT_HEALTH_MS        5000    /* 5s */

#define CE_GW_DEFAULT_RECV_BUF_SIZE    (64 * 1024)   /* 64KB */
#define CE_GW_DEFAULT_SEND_BUF_SIZE    (64 * 1024)   /* 64KB */

#define CE_GW_MAX_BACKENDS             16

#define CE_GW_MAX_EVENTS               256     /* epoll_wait 单次最大事件数 */

/* ================================================================
 * 类型定义（前向声明）
 * ================================================================ */

typedef struct CeGateway             CeGateway;
typedef struct CeGatewayConfig       CeGatewayConfig;
typedef struct CeGatewayStats        CeGatewayStats;
typedef struct CeGatewayConn         CeGatewayConn;
typedef struct CeGatewayConnTable    CeGatewayConnTable;
typedef struct CeGatewayRouter       CeGatewayRouter;
typedef struct CeGatewayRouteRule    CeGatewayRouteRule;
typedef struct CeGatewayBackend      CeGatewayBackend;
typedef struct CeGatewayBackendPool  CeGatewayBackendPool;
typedef struct CeGatewayBackendConn  CeGatewayBackendConn;
typedef struct CeGatewayBackendConfig CeGatewayBackendConfig;

typedef enum CeGatewayBackendType {
    CE_GW_BACKEND_GAME    = 0,
    CE_GW_BACKEND_DBPROXY = 1,
    CE_GW_BACKEND_ADMIN   = 2,
} CeGatewayBackendType;

typedef enum CeGatewayConnState {
    CE_GW_CONN_NONE       = 0,
    CE_GW_CONN_CONNECTING = 1,
    CE_GW_CONN_ACTIVE     = 2,
    CE_GW_CONN_CLOSING    = 3,
    CE_GW_CONN_CLOSED     = 4,
} CeGatewayConnState;

/* ================================================================
 * 配置结构
 * ================================================================ */

struct CeGatewayBackendConfig {
    CeGatewayBackendType type;
    const char*          host;
    int                  port;
    int                  pool_size;         /* 0 = 使用默认值 4 */
};

struct CeGatewayConfig {
    /* 监听 */
    const char* listen_host;
    int         listen_port;
    int         kcp_port;
    int         ws_port;

    /* 连接限制 */
    int         max_connections;
    int         backlog;

    /* 心跳/超时 */
    int         heartbeat_interval_ms;
    int         conn_timeout_ms;
    int         backend_health_interval_ms;

    /* 缓冲区 */
    int         recv_buf_size;
    int         send_buf_size;

    /* 后端 */
    CeGatewayBackendConfig backends[CE_GW_MAX_BACKENDS];
    int                    backend_count;
};

struct CeGatewayStats {
    uint64_t    total_connections;
    uint64_t    total_messages_routed;
    uint64_t    total_bytes_proxied;
    uint64_t    total_errors;
    uint64_t    total_heartbeats_sent;
    uint64_t    total_timeouts;
    uint64_t    total_backend_reconnects;
    int         current_connections;
};

/* ================================================================
 * 生命周期 API
 * ================================================================ */

/**
 * 创建 Gateway 实例
 *
 * @param config  配置（NULL 使用默认值）
 * @return        Gateway 句柄，失败返回 NULL
 */
CeGateway* ce_gateway_create(const CeGatewayConfig* config);

/**
 * 初始化 Gateway（创建 epoll、绑定监听端口、连接后端）
 *
 * @param gw  Gateway 句柄
 * @return    CE_OK 成功，CE_ERR 失败
 */
CeResult ce_gateway_init(CeGateway* gw);

/**
 * 运行 Gateway 主事件循环（阻塞直到 ce_gateway_stop 被调用）
 *
 * @param gw  Gateway 句柄
 * @return    CE_OK 正常退出，CE_ERR 异常
 */
CeResult ce_gateway_run(CeGateway* gw);

/**
 * 停止 Gateway（设置运行标志为 0，事件循环将在下次迭代退出）
 *
 * @param gw  Gateway 句柄
 */
void ce_gateway_stop(CeGateway* gw);

/**
 * 优雅关闭 Gateway
 *
 * 停止 accept 新连接，等待现有连接处理完成或超时
 *
 * @param gw           Gateway 句柄
 * @param timeout_ms   优雅关闭超时（毫秒）
 */
void ce_gateway_shutdown(CeGateway* gw, int timeout_ms);

/**
 * 销毁 Gateway（释放所有资源）
 *
 * @param gw  Gateway 句柄
 */
void ce_gateway_destroy(CeGateway* gw);

/**
 * 获取 Gateway 统计信息
 */
void ce_gateway_get_stats(const CeGateway* gw, CeGatewayStats* stats);

/**
 * 获取默认配置（用于未指定参数时填充默认值）
 */
void ce_gateway_config_defaults(CeGatewayConfig* config);

#ifdef __cplusplus
}
#endif

#endif /* CE_GATEWAY_H */
```

### 6.2 ce_gateway.c — 主逻辑

**核心函数：**

```c
/* ---- 内部初始化 ---- */

/* 创建 epoll 实例和监听 socket */
static CeResult gw_init_epoll(CeGateway* gw);

/* 绑定 TCP 监听端口 */
static CeResult gw_init_listen(CeGateway* gw);

/* 创建 timerfd 用于周期性任务（心跳、超时、健康检查） */
static CeResult gw_init_timer(CeGateway* gw);

/* 初始化后端连接池（连接到 Game/DBProxy） */
static CeResult gw_init_backends(CeGateway* gw);

/* 加载默认路由表 */
static CeResult gw_init_router(CeGateway* gw);

/* ---- 事件处理 ---- */

/* 处理新客户端连接 (EPOLLIN on listen_fd) */
static void gw_handle_accept(CeGateway* gw);

/* 处理定时器事件 (心跳发送、超时检查、后端健康检查) */
static void gw_handle_timer(CeGateway* gw);

/* 处理客户端可读事件 */
static void gw_handle_client_read(CeGateway* gw, int fd);

/* 处理客户端可写事件（冲刷发送缓冲区） */
static void gw_handle_client_write(CeGateway* gw, int fd);

/* 处理后端可读事件（后端响应） */
static void gw_handle_backend_read(CeGateway* gw, int fd);

/* 关闭客户端连接 */
static void gw_close_client(CeGateway* gw, int fd);

/* ---- 事件循环 ---- */

CeResult ce_gateway_run(CeGateway* gw) {
    struct epoll_event events[CE_GW_MAX_EVENTS];

    while (gw->running) {
        int timeout = gw_calc_next_timeout(gw);  /* 根据最近定时任务计算超时 */
        int n = epoll_wait(gw->epfd, events, CE_GW_MAX_EVENTS, timeout);

        if (n < 0) {
            if (errno == EINTR) continue;        /* 被信号中断 */
            CE_LOG_ERROR("GW", "epoll_wait: %s", strerror(errno));
            break;
        }

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            uint32_t ev = events[i].events;

            if (fd == gw->listen_fd) {
                gw_handle_accept(gw);
            } else if (fd == gw->timer_fd) {
                gw_handle_timer(gw);
            } else if (ce_gateway_conn_table_lookup(gw->conn_table, fd)) {
                /* 客户端连接 */
                if (ev & EPOLLIN)  gw_handle_client_read(gw, fd);
                if (ev & EPOLLOUT) gw_handle_client_write(gw, fd);
                if (ev & (EPOLLHUP | EPOLLERR)) gw_close_client(gw, fd);
            } else {
                /* 后端连接 */
                CeGatewayBackendConn* bc = ce_gateway_backend_find_conn(gw->backend, fd);
                if (bc) {
                    if (ev & EPOLLIN)  gw_handle_backend_read(gw, fd);
                    if (ev & EPOLLOUT) ce_gateway_backend_flush(gw->backend, fd);
                    if (ev & (EPOLLHUP | EPOLLERR)) ce_gateway_backend_reconnect(gw->backend, fd);
                }
            }
        }

        /* 优雅关闭检查 */
        if (gw->shutting_down && gw->active_conns == 0) {
            break;
        }
    }

    return CE_OK;
}
```

### 6.3 ce_gateway_conn.c — 连接管理

**连接表哈希函数：**

```c
/* fd 直接作为哈希 key，取模运算优化为位与（bucket_count 为 2 的幂） */
static inline int conn_table_hash(int fd, int mask) {
    return fd & mask;
}
```

**客户端连接读取流程：**

```c
/*
 * 处理客户端可读事件
 *
 * 流程：
 *   1. recv() 写入 recv_buf 尾部
 *   2. 循环解析完整消息（零拷贝：直接在 recv_buf 上 peek）
 *   3. 路由查找 -> 后端转发
 *   4. 更新活跃时间
 *   5. 检查超时
 */
static void gw_handle_client_read(CeGateway* gw, int fd) {
    CeGatewayConn* conn = ce_gateway_conn_table_lookup(gw->conn_table, fd);
    if (!conn) return;

    /* 写入 recv_buf 尾部 */
    int writable = conn->recv_buf_size - conn->recv_offset;
    if (writable <= 0) {
        /* 缓冲区满，可能客户端发送速度过快或未完整解析 */
        /* 先压缩缓冲区：将未消费数据移到头部 */
        ce_gateway_conn_compact_recv(conn);
        writable = conn->recv_buf_size - conn->recv_offset;
        if (writable <= 0) {
            /* 缓冲区仍然满，断开连接 */
            gw_close_client(gw, fd);
            return;
        }
    }

    ssize_t n = recv(fd, conn->recv_buf + conn->recv_offset, writable, 0);
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            gw_close_client(gw, fd);
        }
        return;
    }

    conn->recv_offset += n;
    conn->bytes_recv += n;
    conn->last_active_us = ce_net_base_now_us();

    /* 循环解析消息 */
    while (conn->recv_consumed + CE_NET_BASE_HEADER_SIZE <= conn->recv_offset) {
        uint32_t total_len = ce_net_base_peek_len(
            conn->recv_buf + conn->recv_consumed,
            conn->recv_offset - conn->recv_consumed);

        if (total_len == 0) break;        /* 头部不完整 */

        if (total_len > CE_NET_BASE_MAX_MSG_SIZE) {
            CE_LOG_WARN("GW", "Message too large from fd=%d: %u bytes", fd, total_len);
            gw_close_client(gw, fd);
            return;
        }

        if (conn->recv_consumed + total_len > conn->recv_offset) {
            break;                        /* 消息体不完整，等待更多数据 */
        }

        /* 消息完整，解析 */
        uint16_t msg_type = ce_net_base_peek_type(
            conn->recv_buf + conn->recv_consumed,
            total_len);

        /* 处理心跳 */
        if (msg_type == CE_NET_MSG_PING) {
            ce_net_conn_send_pong_via_fd(fd);   /* 直接回复 PONG */
            conn->recv_consumed += total_len;
            continue;
        }
        if (msg_type == CE_NET_MSG_PONG) {
            conn->last_active_us = ce_net_base_now_us();
            conn->recv_consumed += total_len;
            continue;
        }
        if (msg_type == CE_NET_MSG_DISCONNECT) {
            gw_close_client(gw, fd);
            return;
        }

        /* 路由查找 */
        const CeGatewayRouteRule* rule =
            ce_gateway_router_lookup(gw->router, msg_type);

        if (!rule) {
            CE_LOG_WARN("GW", "No route for msg_type=0x%04X from fd=%d",
                        msg_type, fd);
            conn->recv_consumed += total_len;
            gw->stats.total_errors++;
            continue;
        }

        /* 转发到后端（零拷贝：payload 指针指向 recv_buf） */
        CeResult r = ce_gateway_backend_forward(
            gw->backend, rule->backend_id, rule->flags,
            fd,  /* client_fd，用于响应路由 */
            conn->recv_buf + conn->recv_consumed,  /* 完整帧 */
            total_len);

        if (r != CE_OK) {
            CE_LOG_WARN("GW", "Backend forward failed for fd=%d msg=0x%04X",
                        fd, msg_type);
            if (rule->flags & CE_GW_ROUTE_FLAG_DROP_ON_ERR) {
                conn->recv_consumed += total_len;
            } else {
                gw_close_client(gw, fd);
                return;
            }
        }

        conn->msgs_recv++;
        conn->recv_consumed += total_len;
        gw->stats.total_messages_routed++;
        gw->stats.total_bytes_proxied += total_len;
    }

    /* 压缩缓冲区 */
    ce_gateway_conn_compact_recv(conn);
}
```

### 6.4 ce_gateway_router.c — 消息路由

```c
/*
 * 二分查找路由规则
 *
 * rules 数组按 msg_type 升序排列
 * O(log N) 查找，N = rule_count（通常 < 50）
 */
const CeGatewayRouteRule* ce_gateway_router_lookup(
    const CeGatewayRouter* router, uint16_t msg_type)
{
    int lo = 0, hi = router->rule_count - 1;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint16_t mid_type = router->rules[mid].msg_type;

        if (mid_type == msg_type) {
            return &router->rules[mid];
        } else if (mid_type < msg_type) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    return NULL;  /* 未找到路由 */
}

/*
 * 添加路由规则（插入后保持有序）
 * 若 msg_type 已存在则更新
 */
CeResult ce_gateway_router_add(CeGatewayRouter* router,
                                uint16_t msg_type, uint16_t backend_id,
                                uint8_t flags)
{
    /* 二分查找插入位置 */
    int lo = 0, hi = router->rule_count - 1;
    int pos = router->rule_count;

    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        uint16_t mid_type = router->rules[mid].msg_type;

        if (mid_type == msg_type) {
            /* 已存在，更新 */
            router->rules[mid].backend_id = backend_id;
            router->rules[mid].flags = flags;
            return CE_OK;
        } else if (mid_type < msg_type) {
            lo = mid + 1;
        } else {
            pos = mid;
            hi = mid - 1;
        }
    }

    /* 检查容量 */
    if (router->rule_count >= router->rule_capacity) {
        int new_cap = router->rule_capacity * 2;
        CeGatewayRouteRule* new_rules = realloc(router->rules,
            sizeof(CeGatewayRouteRule) * new_cap);
        if (!new_rules) return CE_ERR;
        router->rules = new_rules;
        router->rule_capacity = new_cap;
    }

    /* 插入到 pos 位置（memmove 保持有序） */
    if (pos < router->rule_count) {
        memmove(&router->rules[pos + 1], &router->rules[pos],
                sizeof(CeGatewayRouteRule) * (router->rule_count - pos));
    }

    router->rules[pos].msg_type   = msg_type;
    router->rules[pos].backend_id = backend_id;
    router->rules[pos].flags      = flags;
    router->rule_count++;

    return CE_OK;
}
```

**默认路由表配置：**

```c
/*
 * 默认路由规则
 * 在 gw_init_router() 中加载
 */
static const CeGatewayRouteRule default_routes[] = {
    /* msg_type,                     backend_id,                  flags */
    { CE_NET_MSG_LOGIN,              CE_GW_BACKEND_GAME,          CE_GW_ROUTE_FLAG_NEED_RESP },
    { CE_NET_MSG_LOGIN_RESP,         CE_GW_BACKEND_GAME,          CE_GW_ROUTE_FLAG_NONE },
    { CE_NET_MSG_GAME_DATA,          CE_GW_BACKEND_GAME,          CE_GW_ROUTE_FLAG_NONE },
    { MSG_JOIN_REQUEST,              CE_GW_BACKEND_GAME,          CE_GW_ROUTE_FLAG_NEED_RESP },
    { MSG_JOIN_RESPONSE,             CE_GW_BACKEND_GAME,          CE_GW_ROUTE_FLAG_NONE },
    { MSG_POSITION_UPDATE,           CE_GW_BACKEND_GAME,          CE_GW_ROUTE_FLAG_NONE },
    { MSG_ENTITY_STATE,              CE_GW_BACKEND_GAME,          CE_GW_ROUTE_FLAG_NONE },
    /* 可扩展：DBProxy 路由 */
    /* { 0x0200, CE_GW_BACKEND_DBPROXY, CE_GW_ROUTE_FLAG_NEED_RESP }, */
};
```

### 6.5 ce_gateway_backend.c — 后端连接池

```c
/*
 * 初始化后端连接池
 * 连接到所有配置的后端
 */
CeResult ce_gateway_backend_init(CeGatewayBackend* backend,
                                  const CeGatewayBackendConfig* configs,
                                  int count, int epfd)
{
    backend->pools = calloc(count, sizeof(CeGatewayBackendPool));
    backend->pool_count = count;
    backend->pool_capacity = count;

    for (int i = 0; i < count; i++) {
        CeGatewayBackendPool* pool = &backend->pools[i];
        pool->type = configs[i].type;
        strncpy(pool->host, configs[i].host, sizeof(pool->host) - 1);
        pool->port = configs[i].port;
        pool->pool_size = configs[i].pool_size > 0 ? configs[i].pool_size : 4;
        pool->round_robin = 0;
        pool->healthy = 0;

        pool->conns = calloc(pool->pool_size, sizeof(CeGatewayBackendConn));
        pool->conn_count = pool->pool_size;

        /* 创建到后端的连接 */
        for (int j = 0; j < pool->pool_size; j++) {
            CeGatewayBackendConn* bc = &pool->conns[j];
            bc->recv_buf_size = CE_GW_DEFAULT_RECV_BUF_SIZE;
            bc->recv_buf = malloc(bc->recv_buf_size);
            bc->state = CE_GW_CONN_NONE;

            if (gw_backend_connect_one(pool, bc) == CE_OK) {
                /* 注册到 epoll */
                struct epoll_event ev = {0};
                ev.events = EPOLLIN;
                ev.data.fd = bc->fd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, bc->fd, &ev);
                pool->healthy = 1;
            }
        }
    }

    return CE_OK;
}

/*
 * 转发消息到后端（零拷贝 writev）
 *
 * @param backend_id   目标后端 ID
 * @param flags        路由标志
 * @param client_fd    源客户端 fd（用于响应路由）
 * @param data         完整消息帧（含头部）
 * @param len          消息总长度
 */
CeResult ce_gateway_backend_forward(CeGatewayBackend* backend,
                                     uint16_t backend_id, uint8_t flags,
                                     int client_fd,
                                     const uint8_t* data, uint32_t len)
{
    if (backend_id >= (uint16_t)backend->pool_count) {
        return CE_ERR;
    }

    CeGatewayBackendPool* pool = &backend->pools[backend_id];
    if (!pool->healthy) {
        CE_LOG_WARN("GW", "Backend %d unhealthy, cannot forward", backend_id);
        return CE_ERR;
    }

    /* 轮询选择一条连接 */
    CeGatewayBackendConn* bc = &pool->conns[pool->round_robin % pool->conn_count];
    pool->round_robin++;

    if (bc->state != CE_GW_CONN_ACTIVE) {
        /* 尝试下一条 */
        for (int i = 0; i < pool->conn_count; i++) {
            bc = &pool->conns[(pool->round_robin + i) % pool->conn_count];
            if (bc->state == CE_GW_CONN_ACTIVE) break;
            bc = NULL;
        }
        if (!bc) return CE_ERR;
    }

    /* 如果需要响应，记录请求映射 */
    if (flags & CE_GW_ROUTE_FLAG_NEED_RESP) {
        ce_gateway_backend_add_pending(backend, client_fd, backend_id);
    }

    /* 零拷贝发送：直接 write 原始帧（帧格式与后端一致） */
    ssize_t n = send(bc->fd, data, len, MSG_NOSIGNAL);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* TODO: 暂存到后端发送缓冲区，注册 EPOLLOUT */
            CE_LOG_WARN("GW", "Backend fd=%d send buffer full", bc->fd);
            return CE_ERR;
        }
        CE_LOG_ERROR("GW", "Backend send failed fd=%d: %s", bc->fd, strerror(errno));
        bc->state = CE_GW_CONN_CLOSING;
        return CE_ERR;
    }

    bc->bytes_sent += n;
    bc->msgs_sent++;
    return CE_OK;
}

/*
 * 处理后端响应（从后端收到的消息转发回客户端）
 */
static void gw_handle_backend_read(CeGateway* gw, int fd) {
    CeGatewayBackendConn* bc = ce_gateway_backend_find_conn(gw->backend, fd);
    if (!bc) return;

    ssize_t n = recv(fd, bc->recv_buf + bc->recv_offset,
                     bc->recv_buf_size - bc->recv_offset, 0);
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            CE_LOG_WARN("GW", "Backend fd=%d disconnected", fd);
            ce_gateway_backend_reconnect(gw->backend, fd);
        }
        return;
    }

    bc->recv_offset += n;
    bc->bytes_recv += n;

    /* 循环解析完整消息 */
    while (bc->recv_consumed + CE_NET_BASE_HEADER_SIZE <= bc->recv_offset) {
        uint32_t total_len = ce_net_base_peek_len(
            bc->recv_buf + bc->recv_consumed,
            bc->recv_offset - bc->recv_consumed);

        if (total_len == 0 || total_len > CE_NET_BASE_MAX_MSG_SIZE) break;
        if (bc->recv_consumed + total_len > bc->recv_offset) break;

        /* 消息完整，查找目标客户端 */
        int client_fd = ce_gateway_backend_find_pending(gw->backend, fd);
        if (client_fd >= 0) {
            /* 转发到客户端 */
            CeGatewayConn* conn = ce_gateway_conn_table_lookup(gw->conn_table, client_fd);
            if (conn && conn->state == CE_GW_CONN_ACTIVE) {
                ssize_t sent = send(client_fd,
                    bc->recv_buf + bc->recv_consumed, total_len, MSG_NOSIGNAL);
                if (sent > 0) {
                    conn->bytes_sent += sent;
                    conn->msgs_sent++;
                }
            }
            ce_gateway_backend_remove_pending(gw->backend, client_fd);
        }

        bc->recv_consumed += total_len;
        bc->msgs_recv++;
    }

    /* 压缩缓冲区 */
    if (bc->recv_consumed > 0) {
        int remaining = bc->recv_offset - bc->recv_consumed;
        if (remaining > 0) {
            memmove(bc->recv_buf, bc->recv_buf + bc->recv_consumed, remaining);
        }
        bc->recv_offset = remaining;
        bc->recv_consumed = 0;
    }
}

/*
 * 后端重连（指数退避）
 */
void ce_gateway_backend_reconnect(CeGatewayBackend* backend, int fd) {
    CeGatewayBackendPool* pool = NULL;
    CeGatewayBackendConn* bc = NULL;

    /* 找到对应的后端池和连接 */
    for (int i = 0; i < backend->pool_count; i++) {
        pool = &backend->pools[i];
        for (int j = 0; j < pool->conn_count; j++) {
            if (pool->conns[j].fd == fd) {
                bc = &pool->conns[j];
                break;
            }
        }
        if (bc) break;
    }

    if (!bc || !pool) return;

    /* 指数退避检查 */
    uint64_t now = ce_net_base_now_us();
    uint32_t backoff_ms = CE_NET_BASE_RECONNECT_MIN_MS
        << (bc->reconnect_attempts < 5 ? bc->reconnect_attempts : 5);
    if (backoff_ms > CE_NET_BASE_RECONNECT_MAX_MS) {
        backoff_ms = CE_NET_BASE_RECONNECT_MAX_MS;
    }

    if (now - bc->last_reconnect_us < (uint64_t)backoff_ms * 1000) {
        return;  /* 退避时间内，不重试 */
    }

    bc->last_reconnect_us = now;
    bc->reconnect_attempts++;

    /* 关闭旧 fd */
    if (bc->fd > 0) {
        close(bc->fd);
        bc->fd = -1;
    }

    /* 重新连接 */
    if (gw_backend_connect_one(pool, bc) == CE_OK) {
        CE_LOG_INFO("GW", "Backend %s:%d reconnected (attempt %d)",
                    pool->host, pool->port, bc->reconnect_attempts);
        bc->reconnect_attempts = 0;
        pool->healthy = 1;
    } else {
        CE_LOG_WARN("GW", "Backend %s:%d reconnect failed (attempt %d)",
                    pool->host, pool->port, bc->reconnect_attempts);
        /* 检查池中是否所有连接都断了 */
        int any_alive = 0;
        for (int j = 0; j < pool->conn_count; j++) {
            if (pool->conns[j].state == CE_GW_CONN_ACTIVE) {
                any_alive = 1;
                break;
            }
        }
        if (!any_alive) pool->healthy = 0;
    }
}
```

### 6.6 ce_gateway_protocol.c — 协议编解码

```c
/*
 * 协议编解码模块
 *
 * 主要职责：
 *   1. 消息完整性校验（复用 ce_net_base_peek_len / peek_type）
 *   2. 帧格式转换（客户端格式 <-> 后端格式，如果需要）
 *   3. 心跳消息处理
 *
 * 大部分功能直接复用 ce_net_base.h 的函数，此模块仅做 Gateway 层面的封装
 */

/*
 * 检查缓冲区中是否有完整消息
 *
 * @param buf        缓冲区
 * @param buf_len    缓冲区有效数据长度
 * @param out_total  输出：完整消息的总长度（如果有）
 * @return           CE_OK 有完整消息, CE_ERR 数据不完整或错误
 */
CeResult ce_gateway_protocol_peek(const uint8_t* buf, uint32_t buf_len,
                                   uint32_t* out_total)
{
    if (buf_len < CE_NET_BASE_HEADER_SIZE) {
        return CE_ERR;  /* 头部不完整 */
    }

    uint32_t total = ce_net_base_peek_len(buf, buf_len);
    if (total == 0) {
        return CE_ERR;  /* 头部不完整 */
    }

    if (total < CE_NET_BASE_HEADER_SIZE) {
        return CE_ERR;  /* 长度字段非法 */
    }

    if (total > CE_NET_BASE_MAX_MSG_SIZE) {
        return CE_ERR;  /* 消息过大 */
    }

    if (buf_len < total) {
        return CE_ERR;  /* 消息体不完整 */
    }

    *out_total = total;
    return CE_OK;
}

/*
 * 帧格式重排（如果客户端使用 ce_client_network 的格式）
 *
 * 原始格式: [2B msg_type][4B body_len][N body]    (ce_client_network.h)
 * 目标格式: [4B total_len][2B msg_type][N payload] (ce_net_base.h)
 *
 * 如果客户端已经使用 ce_net_base 格式，此函数为 no-op
 *
 * @param src         原始帧数据
 * @param src_len     原始帧长度
 * @param dst         输出缓冲区（至少 src_len 字节）
 * @param dst_len     输出缓冲区大小
 * @return            重排后的帧长度，0 表示缓冲区不足
 */
uint32_t ce_gateway_protocol_remap_header(
    const uint8_t* src, uint32_t src_len,
    uint8_t* dst, uint32_t dst_len)
{
    if (src_len < 6 || dst_len < src_len) return 0;

    /* 读取原始头部 */
    uint16_t msg_type = ((uint16_t)src[0] << 8) | src[1];
    uint32_t body_len = ((uint32_t)src[2] << 24) | ((uint32_t)src[3] << 16)
                      | ((uint32_t)src[4] << 8) | src[5];

    uint32_t total_len = body_len + 6;

    /* 写入新头部 */
    dst[0] = (uint8_t)(total_len >> 24);
    dst[1] = (uint8_t)(total_len >> 16);
    dst[2] = (uint8_t)(total_len >> 8);
    dst[3] = (uint8_t)(total_len);
    dst[4] = (uint8_t)(msg_type >> 8);
    dst[5] = (uint8_t)(msg_type);

    /* payload 直接拷贝（如果格式不同，需要这次拷贝） */
    if (src_len > 6) {
        memcpy(dst + 6, src + 6, src_len - 6);
    }

    return src_len;
}
```

### 6.7 ce_gateway_main.c — 进程入口

```c
/*
 * ChaosEngine C 版 Gateway 进程入口
 *
 * 替代原 Lua 版本（ce_gateway_main.c 加载 src_lua/gateway/init.lua）
 *
 * 用法:
 *   ./chaos_gateway [--port PORT] [--backend HOST:PORT] [--max-conn N]
 *
 * 默认配置:
 *   监听: 0.0.0.0:9000
 *   后端: Game Server 127.0.0.1:7777
 *   后端: DBProxy 127.0.0.1:9003
 *   最大连接: 10000
 */

#define _POSIX_C_SOURCE 200112L
#define _GNU_SOURCE                          /* timerfd_create */

#include "public_api/chaos_engine.h"
#include "gateway/ce_gateway.h"
#include "network/ce_net_base.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static volatile int g_running = 1;
static CeGateway* g_gateway = NULL;

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
    if (g_gateway) {
        ce_gateway_stop(g_gateway);
    }
}

static void usage(const char* prog) {
    fprintf(stderr,
        "Usage: %s [options]\n"
        "Options:\n"
        "  --port PORT          TCP listen port (default: 9000)\n"
        "  --host HOST          Listen address (default: 0.0.0.0)\n"
        "  --game HOST:PORT     Game Server backend (default: 127.0.0.1:7777)\n"
        "  --dbproxy HOST:PORT  DBProxy backend (default: 127.0.0.1:9003)\n"
        "  --max-conn N         Max client connections (default: 10000)\n"
        "  --heartbeat MS       Heartbeat interval in ms (default: 30000)\n"
        "  --timeout MS         Connection timeout in ms (default: 90000)\n"
        "  --help               Show this help\n",
        prog);
}

int main(int argc, char** argv) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    /* 忽略 SIGPIPE（send 到已关闭的 socket） */
    signal(SIGPIPE, SIG_IGN);

    /* 默认配置 */
    CeGatewayConfig config;
    ce_gateway_config_defaults(&config);

    /* 解析命令行参数 */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            config.listen_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            config.listen_host = argv[++i];
        } else if (strcmp(argv[i], "--game") == 0 && i + 1 < argc) {
            /* 解析 HOST:PORT */
            char* colon = strchr(argv[i + 1], ':');
            if (colon) {
                *colon = '\0';
                config.backends[0].host = argv[i + 1];
                config.backends[0].port = atoi(colon + 1);
            }
            i++;
        } else if (strcmp(argv[i], "--dbproxy") == 0 && i + 1 < argc) {
            char* colon = strchr(argv[i + 1], ':');
            if (colon) {
                *colon = '\0';
                config.backends[1].host = argv[i + 1];
                config.backends[1].port = atoi(colon + 1);
            }
            i++;
        } else if (strcmp(argv[i], "--max-conn") == 0 && i + 1 < argc) {
            config.max_connections = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--heartbeat") == 0 && i + 1 < argc) {
            config.heartbeat_interval_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            config.conn_timeout_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
    }

    /* 初始化引擎 */
    CeEngineConfig engine_cfg = {
        .app_name      = "ChaosEngine-Gateway",
        .window_width  = 0,
        .window_height = 0,
        .fullscreen    = CE_FALSE,
        .vsync         = CE_FALSE,
        .log_level     = CE_LOG_INFO,
        .log_file_path = "logs/chaos_gateway.log"
    };
    if (ce_init(&engine_cfg) != CE_OK) {
        fprintf(stderr, "[Gateway] Engine init failed\n");
        return 1;
    }

    if (ce_net_init() != CE_OK) {
        fprintf(stderr, "[Gateway] Network init failed\n");
        ce_shutdown();
        return 1;
    }

    /* 创建并初始化 Gateway */
    g_gateway = ce_gateway_create(&config);
    if (!g_gateway) {
        fprintf(stderr, "[Gateway] Create failed\n");
        ce_net_shutdown();
        ce_shutdown();
        return 1;
    }

    if (ce_gateway_init(g_gateway) != CE_OK) {
        fprintf(stderr, "[Gateway] Init failed\n");
        ce_gateway_destroy(g_gateway);
        ce_net_shutdown();
        ce_shutdown();
        return 1;
    }

    printf("========================================\n");
    printf("  ChaosEngine Gateway (C) v0.1.0\n");
    printf("  Listen: %s:%d\n", config.listen_host, config.listen_port);
    printf("  Max connections: %d\n", config.max_connections);
    printf("  Heartbeat: %dms, Timeout: %dms\n",
           config.heartbeat_interval_ms, config.conn_timeout_ms);
    for (int i = 0; i < config.backend_count; i++) {
        printf("  Backend[%d]: %s:%d (type=%d)\n",
               i, config.backends[i].host, config.backends[i].port,
               config.backends[i].type);
    }
    printf("  Press Ctrl+C to exit.\n");
    printf("========================================\n\n");

    /* 运行事件循环 */
    ce_gateway_run(g_gateway);

    /* 优雅关闭 */
    printf("[Gateway] Shutting down...\n");
    ce_gateway_shutdown(g_gateway, 5000);  /* 5s 优雅关闭超时 */
    ce_gateway_destroy(g_gateway);

    ce_net_shutdown();
    ce_shutdown();

    printf("[Gateway] Shutdown complete.\n");
    return 0;
}

/* 默认配置填充 */
void ce_gateway_config_defaults(CeGatewayConfig* config) {
    memset(config, 0, sizeof(*config));

    config->listen_host    = CE_GW_DEFAULT_LISTEN_HOST;
    config->listen_port    = CE_GW_DEFAULT_TCP_PORT;
    config->kcp_port       = CE_GW_DEFAULT_KCP_PORT;
    config->ws_port        = CE_GW_DEFAULT_WS_PORT;
    config->max_connections = CE_GW_DEFAULT_MAX_CONNECTIONS;
    config->backlog        = CE_GW_DEFAULT_BACKLOG;
    config->heartbeat_interval_ms = CE_GW_DEFAULT_HEARTBEAT_MS;
    config->conn_timeout_ms       = CE_GW_DEFAULT_TIMEOUT_MS;
    config->backend_health_interval_ms = CE_GW_DEFAULT_HEALTH_MS;
    config->recv_buf_size  = CE_GW_DEFAULT_RECV_BUF_SIZE;
    config->send_buf_size  = CE_GW_DEFAULT_SEND_BUF_SIZE;

    /* 默认后端 */
    config->backends[0].type      = CE_GW_BACKEND_GAME;
    config->backends[0].host      = "127.0.0.1";
    config->backends[0].port      = 7777;
    config->backends[0].pool_size = 4;

    config->backends[1].type      = CE_GW_BACKEND_DBPROXY;
    config->backends[1].host      = "127.0.0.1";
    config->backends[1].port      = 9003;
    config->backends[1].pool_size = 2;

    config->backend_count = 2;
}
```

---

## 7. 与现有代码的复用关系

### 7.1 复用 ce_net_base 的部分

| ce_net_base 功能 | Gateway 使用方式 | 复用程度 |
|------------------|-----------------|----------|
| `ce_net_base_pack()` | 后端发送时打包消息帧 | ✅ 直接复用 |
| `ce_net_base_unpack()` | 解析后端响应消息 | ✅ 直接复用 |
| `ce_net_base_peek_len()` | 客户端/后端消息完整性检查 | ✅ 直接复用 |
| `ce_net_base_peek_type()` | 客户端/后端消息类型读取 | ✅ 直接复用 |
| `CeNetMsgType` 枚举 | PING/PONG/DISCONNECT/LOGIN 等消息类型 | ✅ 直接复用 |
| `CE_NET_BASE_HEADER_SIZE` | 协议头大小常量 | ✅ 直接复用 |
| `CE_NET_BASE_MAX_MSG_SIZE` | 消息大小上限 | ✅ 直接复用 |
| `ce_net_base_now_us()` | 获取单调时间（心跳/超时判断） | ✅ 直接复用 |
| `ce_net_base_is_heartbeat()` | 判断心跳消息 | ✅ 直接复用 |
| `CeNetConnConfig` / `CeNetConnection` | 后端连接管理 | ⚠️ 部分参考（Gateway 自行管理 epoll 注册，不直接用 CeNetConnection 的阻塞 API） |
| `CeNetPool` | 后端连接池 | ❌ 不直接复用（Gateway 需要非阻塞 + epoll 集成，CeNetPool 是阻塞设计） |
| `CE_NET_BASE_RECONNECT_MIN/MAX_MS` | 后端重连退避参数 | ✅ 复用常量值 |

### 7.2 复用 ce_async_io 的部分

| ce_async_io 功能 | Gateway 使用方式 | 复用程度 |
|-----------------|-----------------|----------|
| `CeAsyncContext` | 事件循环 | ⚠️ 可选复用（Phase 1 直接用 epoll，Phase 3 可切换 io_uring） |
| `ce_async_accept/recv/send` | 异步操作提交 | ⚠️ 可选（直接用 epoll + 非阻塞 recv/send 更简单） |

**决策：MVP Phase 1 直接使用 epoll 系统调用，不经过 ce_async_io 抽象层。**

理由：
- Gateway 需要精细控制 epoll 事件注册（EPOLLIN/EPOLLOUT 切换）
- ce_async_io 的 accept/recv 模型适合 server 的 "提交-等待-回调" 模式
- Gateway 的 "epoll_wait -> 查连接表 -> 读 -> 路由 -> 写后端" 流程更直接
- Phase 3 可选择切换到 io_uring，届时再适配

### 7.3 新写的部分

| 模块 | 原因 |
|------|------|
| `CeGatewayConnTable`（哈希表） | ce_net_base 无 fd->Conn 哈希表，需新写 |
| `CeGatewayRouter`（路由表） | 全新功能，原 Lua 版在 Lua 层实现 |
| `CeGatewayBackend`（后端连接池） | 需要非阻塞 + epoll 集成 + 响应路由，CeNetPool 不满足 |
| `ce_gateway_protocol_remap_header()` | 帧格式转换，ce_net_base 无此功能 |
| Gateway 事件循环 | 全新，原 Lua 版由 LuaSocket 协程驱动 |

### 7.4 复用关系图

```
                    ┌─────────────────────────────────┐
                    │       ce_gateway_main.c          │
                    │       (进程入口)                  │
                    └────────────┬────────────────────┘
                                 │
                    ┌────────────v────────────────────┐
                    │        ce_gateway.h/c            │
                    │    (事件循环 + 模块协调)          │
                    └──┬──────┬──────┬──────┬─────────┘
                       │      │      │      │
              ┌────────┘      │      │      └──────────┐
              v               v      v                  v
     ┌────────────────┐ ┌──────────┐ ┌──────────┐ ┌──────────────┐
     │ce_gateway_conn │ │router    │ │backend   │ │protocol      │
     │(连接表+对象)    │ │(路由表)   │ │(后端池)   │ │(帧解析)       │
     └───────┬────────┘ └────┬─────┘ └────┬─────┘ └──────┬───────┘
             │               │            │              │
             │               │            │              v
             │               │            │      ┌───────────────┐
             │               │            │      │ ce_net_base.h │ ← 复用
             │               │            │      │ peek/pack/    │
             │               │            │      │ unpack/常量    │
             │               │            │      └───────────────┘
             │               │            │
     ┌───────v───────┐       │            │
     │ epoll (Linux)  │←──────┴────────────┘
     │ 系统调用        │     Gateway 直接使用 epoll，
     │ (不经过         │     不经过 ce_async_io 抽象
     │  ce_async_io)  │
     └───────────────┘
```

---

## 8. CMake 集成方案

### 8.1 新增 gateway 子目录 CMakeLists.txt

在 `src_c/gateway/CMakeLists.txt` 中：

```cmake
# ============================================================
# ChaosEngine Gateway 模块 - 纯 C99
# 产出静态库 engine_gateway，供 chaos_gateway 可执行文件链接
# ============================================================

add_library(engine_gateway STATIC
    ce_gateway.c
    ce_gateway_conn.c
    ce_gateway_router.c
    ce_gateway_backend.c
    ce_gateway_protocol.c
)

target_include_directories(engine_gateway
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src_c/public_api
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src_c
        ${CMAKE_SOURCE_DIR}/src_c/network
        ${CMAKE_SOURCE_DIR}/src_c/gateway
)

target_link_libraries(engine_gateway PRIVATE engine_core)

target_compile_options(engine_gateway PRIVATE
    $<$<C_COMPILER_ID:GNU>:-Wall -Wextra -std=c99 -Wpedantic>
    $<$<C_COMPILER_ID:Clang>:-Wall -Wextra -std=c99 -Wpedantic>
)

set_target_properties(engine_gateway PROPERTIES LINKER_LANGUAGE C)
```

### 8.2 修改 src_c/CMakeLists.txt

**1. 添加 gateway 子目录（在 admin_ipc 之前）：**

```cmake
# ============================================================
# Gateway 模块 (纯 C 实现)
# ============================================================
add_subdirectory(gateway)
```

**2. 替换 chaos_gateway 可执行文件定义：**

将现有的 Lua 版本：

```cmake
# ============================================================
# Gateway 进程可执行文件 (Lua 驱动)        ← 删除或注释
# ============================================================
add_executable(chaos_gateway
    runtime/ce_gateway_main.c
)
target_include_directories(chaos_gateway PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LUA_HEADERS_DIR}
)
target_link_libraries(chaos_gateway PRIVATE engine_core)
set_target_properties(chaos_gateway PROPERTIES LINKER_LANGUAGE C)
```

替换为 C 版本：

```cmake
# ============================================================
# Gateway 进程可执行文件 (纯 C 实现)
# ============================================================
add_executable(chaos_gateway
    runtime/ce_gateway_main.c
)
target_include_directories(chaos_gateway PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(chaos_gateway PRIVATE engine_core engine_gateway)
set_target_properties(chaos_gateway PROPERTIES LINKER_LANGUAGE C)

# 传递 io_uring 宏给 chaos_gateway
if(CHAOS_USE_IO_URING AND LIBURING_FOUND)
    target_compile_definitions(chaos_gateway PRIVATE CHAOS_HAS_IO_URING)
    target_include_directories(chaos_gateway PRIVATE ${LIBURING_INCLUDE_DIRS})
    target_link_libraries(chaos_gateway PRIVATE ${LIBURING_LIBRARIES})
endif()
```

### 8.3 构建验证

```bash
cd /home/zhongfangdao/chaos-engine
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make chaos_gateway -j$(nproc)

# 验证产物
./chaos_gateway --help
./chaos_gateway --port 9000 --game 127.0.0.1:7777
```

---

## 9. 客户端改造点

### 9.1 ce_client_main.c 端口修改

当前 `ce_client_main.c` 第 30 行：

```c
static int         g_gateway_port = 7777;
```

**改为：**

```c
static int         g_gateway_port = 9000;
```

这样客户端默认连接 Gateway 的 9000 端口，而非直连 Game Server 的 7777 端口。

### 9.2 ce_client_network.h 端口一致性

`ce_client_network.h` 已定义 `CE_CLIENT_DEFAULT_PORT 9000`，与 Gateway 监听端口一致，无需修改。

### 9.3 协议格式对齐

`ce_client_network.h` 使用的帧格式为 `[2B msg_type][4B body_len][N body]`，与 Gateway 内部的 `ce_net_base` 格式 `[4B total_len][2B msg_type][N payload]` 不同。

**两种方案（择一）：**

**方案 A（推荐）：统一客户端为 ce_net_base 格式**

修改 `ce_client_network.c` 中的打包/解包逻辑，改用 `ce_net_base_pack()` / `ce_net_base_unpack()`。这样客户端和 Gateway 使用相同帧格式，Gateway 无需做头部重排，真正零拷贝。

改动范围：
- `ce_client_network.c` 中的 `ce_client_net_send_join()`、`ce_client_net_send_position()`、`ce_client_net_poll()` 的打包/解包逻辑
- 消息类型改为 `CeNetMsgType` 枚举值

**方案 B（过渡）：Gateway 做头部重排**

Gateway 在 `ce_gateway_protocol.c` 中调用 `ce_gateway_protocol_remap_header()` 转换帧格式。客户端无需改动，但 Gateway 多一次 6 字节的头部重排（payload 仍零拷贝）。

**决策：MVP Phase 1 采用方案 B（快速验证），Phase 2 迁移到方案 A（统一协议）。**

### 9.4 改动文件清单

| 文件 | 改动内容 | Phase |
|------|---------|-------|
| `src_c/runtime/ce_client_main.c` | `g_gateway_port` 从 7777 改为 9000 | Phase 1 |
| `src_c/runtime/ce_client_network.c` | （方案 A）改用 ce_net_base 格式打包/解包 | Phase 2 |
| `src_c/runtime/ce_client_network.h` | （方案 A）消息类型改用 CeNetMsgType | Phase 2 |

---

## 10. MVP Phase 1 范围

### 10.1 Phase 1 目标

**最小可用 Gateway：TCP 接入 + 消息路由 + 后端转发**

客户端通过 Gateway 连接 Game Server，消息双向透传。

### 10.2 Phase 1 功能清单

| 功能 | 状态 | 说明 |
|------|------|------|
| TCP 监听 :9000 | ✅ 必须 | 单线程 epoll accept |
| 客户端连接管理 | ✅ 必须 | fd->Conn 哈希表，O(1) 查找 |
| 消息帧解析 | ✅ 必须 | 复用 ce_net_base_peek_len/peek_type |
| 消息路由 | ✅ 必须 | 二分查找路由表 |
| 后端 TCP 连接池 | ✅ 必须 | 到 Game Server 7777 的长连接 |
| 后端转发 | ✅ 必须 | writev 零拷贝转发 |
| 后端响应回传 | ✅ 必须 | 后端响应转发回客户端 |
| 心跳 PING/PONG | ✅ 必须 | 客户端心跳检测 |
| 连接超时断开 | ✅ 必须 | 90s 无活动断开 |
| 优雅关闭 | ✅ 必须 | SIGTERM 停 accept，等连接处理完 |
| KCP 接入 | ❌ 后续 | Phase 2 |
| WebSocket 接入 | ❌ 后续 | Phase 3 |
| io_uring 加速 | ❌ 后续 | Phase 3 |
| 协议统一（方案 A） | ❌ 后续 | Phase 2 |
| 后端健康检查 | ⚠️ 简化 | Phase 1 仅检查连接状态，不发送 PING |
| DBProxy 后端 | ⚠️ 可选 | Phase 1 仅连接 Game Server 即可验证 |

### 10.3 Phase 1 数据流

```
客户端                  Gateway                    Game Server
  │                        │                           │
  │── TCP connect :9000 ──>│                           │
  │<── TCP established ────│                           │
  │                        │── TCP connect :7777 ─────>│
  │                        │<── TCP established ───────│
  │                        │                           │
  │── LOGIN (0x0010) ─────>│                           │
  │                        │── 路由查找: 0x0010 -> GAME │
  │                        │── forward to Game ────────>│
  │                        │                           │
  │                        │<── LOGIN_RESP (0x0011) ───│
  │                        │── 查 pending: resp -> client_fd
  │<── LOGIN_RESP ─────────│                           │
  │                        │                           │
  │── PING (0x0001) ──────>│                           │
  │<── PONG (0x0002) ──────│   (Gateway 直接回复)       │
  │                        │                           │
  │── POSITION (0x8003) ──>│── forward to Game ────────>│
  │                        │<── ENTITY_STATE (0x8004) ─│
  │<── ENTITY_STATE ───────│                           │
  │                        │                           │
  │── DISCONNECT (0xFFFF) >│── 关闭连接                 │
  │                        │                           │
```

### 10.4 Phase 1 验收测试

```bash
# 1. 启动 Game Server
./chaos_server &

# 2. 启动 Gateway
./chaos_gateway --port 9000 --game 127.0.0.1:7777 &

# 3. 启动客户端（连接 Gateway 而非直连 Game Server）
./chaos_client --connect 127.0.0.1:9000

# 4. 验证：
#    - 客户端能加入游戏（JOIN_REQUEST -> JOIN_RESPONSE 透传成功）
#    - 客户端位置更新能到达 Game Server
#    - Game Server 的 ENTITY_STATE 广播能通过 Gateway 到达客户端
#    - 心跳正常（30s 间隔，无超时断开）
#    - Ctrl+C Gateway 后，客户端收到断开通知
```

---

## 11. 后续 Phase 规划

### Phase 2：KCP 接入 + 协议统一

| 功能 | 说明 |
|------|------|
| KCP 监听 :9001 | 复用 `ce_kcp.h`，UDP socket + KCP 协议栈 |
| 协议统一（方案 A） | 客户端改用 ce_net_base 格式，消除头部重排 |
| DBProxy 后端 | 连接 DBProxy :9003，路由 DB 相关消息 |
| 后端健康检查 | 定时发送 PING 到后端，检测可用性 |
| 连接数限制 | max_connections 上限强制执行 |

### Phase 3：WebSocket + io_uring

| 功能 | 说明 |
|------|------|
| WebSocket 监听 :9002 | HTTP 升级握手 + WebSocket 帧封装 |
| io_uring 后端 | 切换 epoll -> io_uring，Registered Buffers 零拷贝 |
| 背压机制 | 后端不可写时，客户端 send_buf 满时丢弃策略 |
| 连接限流 | 令牌桶限流，防止 SYN flood |

### Phase 4：跨区路由 + Admin

| 功能 | 说明 |
|------|------|
| Admin IPC | 管理 API：查看连接列表、路由表、统计 |
| 跨区消息路由 | 复用 `CeNetRouterMesh`，0x1000-0x1FFF 消息转发到 Router |
| 配置热加载 | 运行时更新路由表（通过 Admin IPC） |
| TLS 支持 | TCP + TLS 加密（可选） |

---

## 12. 验收标准

### 12.1 代码质量

- [ ] 纯 C99 编译通过（`-std=c99 -Wall -Wextra -Wpedantic` 无警告）
- [ ] 所有公开函数有 Doxygen 注释
- [ ] 所有结构体字段有注释说明
- [ ] 无内存泄漏（Valgrind 验证）
- [ ] 无未初始化变量（Valgrind 验证）

### 12.2 功能验证

- [ ] Gateway 启动后监听 :9000
- [ ] 客户端通过 Gateway 连接 Game Server 成功
- [ ] JOIN_REQUEST/RESPONSE 双向透传
- [ ] POSITION_UPDATE / ENTITY_STATE 双向透传
- [ ] PING/PONG 心跳正常
- [ ] 90s 无活动连接自动断开
- [ ] SIGTERM 优雅关闭（停 accept，等连接处理完）
- [ ] 后端断开后自动重连

### 12.3 性能指标

- [ ] 单连接消息转发延迟 < 0.1ms（本机）
- [ ] 1000 并发连接稳定运行 10 分钟无崩溃
- [ ] 10000 连接 epoll_wait 响应时间 < 1ms
- [ ] 内存占用：每连接 < 128KB（含收发缓冲区）
- [ ] 路由表查找 < 6 次比较（50 条规则二分查找）

### 12.4 构建集成

- [ ] `src_c/gateway/CMakeLists.txt` 正确生成 `engine_gateway` 静态库
- [ ] `src_c/CMakeLists.txt` 中 `chaos_gateway` 链接 `engine_gateway`
- [ ] `make chaos_gateway` 编译成功
- [ ] 不影响其他可执行文件的构建（chaos_server, chaos_client 等）

---

## 附录 A：关键常量速查

```c
/* 端口 */
#define CE_GW_DEFAULT_TCP_PORT         9000
#define CE_GW_DEFAULT_KCP_PORT         9001
#define CE_GW_DEFAULT_WS_PORT          9002

/* 连接 */
#define CE_GW_DEFAULT_MAX_CONNECTIONS  10000
#define CE_GW_DEFAULT_BACKLOG          512

/* 心跳/超时 */
#define CE_GW_DEFAULT_HEARTBEAT_MS     30000   /* 30s */
#define CE_GW_DEFAULT_TIMEOUT_MS       90000   /* 90s */
#define CE_GW_DEFAULT_HEALTH_MS        5000    /* 5s */

/* 缓冲区 */
#define CE_GW_DEFAULT_RECV_BUF_SIZE    (64 * 1024)
#define CE_GW_DEFAULT_SEND_BUF_SIZE    (64 * 1024)

/* 复用 ce_net_base 常量 */
#define CE_NET_BASE_HEADER_SIZE         6
#define CE_NET_BASE_MAX_MSG_SIZE        (256 * 1024)
#define CE_NET_BASE_RECONNECT_MIN_MS    1000
#define CE_NET_BASE_RECONNECT_MAX_MS    30000
```

## 附录 B：消息类型路由表（默认）

| msg_type | 名称 | 方向 | 路由后端 | 标志 |
|----------|------|------|---------|------|
| 0x0001 | PING | C→G | Gateway 本地处理 | - |
| 0x0002 | PONG | C→G | Gateway 本地处理 | - |
| 0x0010 | LOGIN | C→G | GAME | NEED_RESP |
| 0x0011 | LOGIN_RESP | G→C | GAME→C | - |
| 0x0100 | GAME_DATA | C→G | GAME | NONE |
| 0x8001 | JOIN_REQUEST | C→G | GAME | NEED_RESP |
| 0x8002 | JOIN_RESPONSE | G→C | GAME→C | - |
| 0x8003 | POSITION_UPDATE | C→G | GAME | NONE |
| 0x8004 | ENTITY_STATE | G→C | GAME→C | - |
| 0xFFFF | DISCONNECT | C→G | Gateway 本地处理 | - |

## 附录 C：与原 Lua Gateway 的对应关系

| 原 Lua 模块 | C 替代模块 | 说明 |
|-------------|-----------|------|
| `src_lua/gateway/init.lua` | `ce_gateway_main.c` + `ce_gateway.c` | 进程入口 + 事件循环 |
| `src_lua/gateway/protocol.lua` | `ce_gateway_protocol.c` | 帧编解码 |
| `src_lua/gateway/router.lua` | `ce_gateway_router.c` | 消息路由 |
| `src_lua/gateway/server.lua` | `ce_gateway_conn.c` | TCP 接入 + 连接管理 |
| `src_lua/gateway/backend.lua` | `ce_gateway_backend.c` | 后端连接池 |
| LuaSocket 协程 | epoll 事件循环 | I/O 驱动模型 |
| Lua table 路由表 | C 数组 + 二分查找 | 路由查找 |
| Lua GC | 无 GC（C 手动管理） | 内存管理 |
