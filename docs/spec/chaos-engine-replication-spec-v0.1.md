# ChaosEngine 属性复制 & RPC 系统 — 设计规格说明书 v0.1

> **状态**: 待确认  
> **日期**: 2026-06-15  
> **依赖**: ECS (ce_ecs), AOI (ce_aoi), Save (ce_save), DBProxy (ce_dbproxy), Network (ce_network)  
> **关联 spec**: chaos-engine-spec-v0.1.md §7 (网络同步)

---

## 目录

1. [愿景与定位](#1-愿景与定位)
2. [核心概念](#2-核心概念)
3. [架构总览](#3-架构总览)
4. [属性 Flag 系统](#4-属性-flag-系统)
5. [脏标管线](#5-脏标管线)
6. [RPC 系统](#6-rpc-系统)
7. [Mailbox 路由](#7-mailbox-路由)
8. [FlatBuffers 协议](#8-flatbuffers-协议)
9. [发送时序](#9-发送时序)
10. [AOI 集成](#10-aoi-集成)
11. [DBProxy 集成](#11-dbproxy-集成)
12. [Gateway 集成](#12-gateway-集成)
13. [模块设计](#13-模块设计)
14. [API 定义](#14-api-定义)
15. [MVP 范围](#15-mvp-范围)
16. [路线图](#16-路线图)
17. [参考分析](#17-参考分析)

---

## 1. 愿景与定位

### 1.1 目标

为 ChaosEngine 提供统一的客户端/服务器属性复制和 RPC 通信系统，实现：

- **一套脏标管线**同时驱动 Game→Client 实时同步和 Game→DBProxy 持久化存档
- **字段级同步策略**：AOI 广播、属主独占、服务器专属、持久化标记
- **Actor 模型路由**：基于 Mailbox 的 Server↔Server RPC
- **FlatBuffers 序列化**：零拷贝、多语言支持
- **双通道客户端**：KCP（不可靠属性同步）+ TCP（可靠 RPC）

### 1.2 设计原则

| 原则 | 说明 |
|------|------|
| **统一管线** | Game→Client 和 Game→DBProxy 共用同一套脏标+序列化 |
| **协议解耦** | RPC 与具体传输协议解耦，现有 `ce_rpc_call` 保持不变 |
| **字段级粒度** | 同一组件内不同字段可有不同同步策略 |
| **帧末批量** | 属性变化默认帧末批量发送，RPC 可选合并立即发送 |
| **Actor 寻址** | Server→Server 通过 Router 的 entity→mailbox 映射寻址 |

---

## 2. 核心概念

### 2.1 四个 Flag

```
CE_FLAG_AOI_BROADCAST  (0x01)  属性变化推送给 AOI 范围内所有客户端
CE_FLAG_OWNER_ONLY     (0x02)  属性变化只推送给拥有该实体的客户端
CE_FLAG_SERVER_ONLY    (0x04)  属性永不推送客户端，仅服务器内存
CE_FLAG_PERSIST        (0x08)  属性变化标记脏时同步到 DBProxy 存档
```

### 2.2 三条 RPC 通道

```
Client ──RPC──► Server    请求: 使用技能、拾取物品、移动
Server ──RPC──► Client    通知: 播放特效、弹出提示、UI更新
Server ──RPC──► Server    跨服: 玩家跨Cell迁移、全服广播、公会操作
```

### 2.3 传输通道

```
Client ◄══KCP══► Gateway    不可靠: 属性同步 (默认)
Client ◄══TCP══► Gateway    可靠:   RPC、登录、关键通知
Server ◄══TCP══► Server     可靠:   跨服 RPC
Gateway ◄══TCP══► Game      可靠:   全量连接 (同机全连)
Game    ◄══TCP══► DBProxy   可靠:   存档 (PERSIST only)
```

---

## 3. 架构总览

```
                              ┌──────────────┐
         Client ◄══KCP══════►│              │◄══KCP══════► Client
         Client ◄══TCP══════►│   Gateway    │◄══TCP══════► Client
                              │              │
                              └──┬───┬───┬───┘
                                 │   │   │
                            TCP  │   │   │ TCP (同机全连)
                                 │   │   │
              ┌──────────────────┘   │   └──────────────────┐
              │                      │                      │
    ┌─────────▼──────┐    ┌─────────▼──────┐    ┌─────────▼──────┐
    │    Game A      │    │    Router      │    │    Game B      │
    │                │    │                │    │                │
    │ ┌────────────┐ │    │ 服务注册表      │    │ ┌────────────┐ │
    │ │    ECS     │ │    │ mailbox映射     │    │ │    ECS     │ │
    │ │    AOI     │ │    │ entity→Game    │    │ │    AOI     │ │
    │ │    Save    │ │    └────────────────┘    │ │    Save    │ │
    │ └─────┬──────┘ │                          │ └─────┬──────┘ │
    │       │        │                          │       │        │
    │ ┌─────▼──────┐ │    ┌──────────┐          │ ┌─────▼──────┐ │
    │ │  ReplMgr   │◄┼────┤ DBProxy  │──► MongoDB│ │  ReplMgr   │ │
    │ │  Mailbox   │ │    └──────────┘          │ │  Mailbox   │ │
    │ └────────────┘ │                          │ └────────────┘ │
    └────────────────┘                          └────────────────┘
              │                                          │
              └──────────── TCP ─────────────────────────┘
                        (跨服 RPC: Router 查 mailbox)
```

### 3.1 数据流

```
每帧 Server Loop:

  1. System 修改组件 → ECS setter 自动标记脏
  2. 调用 RPC (合并) → [RPC + 当前脏属性] FlatBuffer → 立即发送
  3. 调用 RPC (不合并) → [仅 RPC] FlatBuffer → 立即发送
  4. tick 结束 → ReplMgr.flush()
     ├─ 收集本帧所有脏字段
     ├─ 按 flag 分流:
     │   ├─ AOI_BROADCAST → 查 AOI 范围内客户端 → Gateway 转发
     │   ├─ OWNER_ONLY    → 查 entity 属主 → Gateway 转发
     │   └─ PERSIST       → DBProxy 存档
     └─ 清除脏标
```

---

## 4. 属性 Flag 系统

### 4.1 Flag 定义

```c
// ce_replication.h

typedef enum CeReplFlag {
    CE_FLAG_AOI_BROADCAST  = 0x01,   // 同步给 AOI 范围内所有客户端
    CE_FLAG_OWNER_ONLY     = 0x02,   // 只同步给属主客户端
    CE_FLAG_SERVER_ONLY    = 0x04,   // 服务器专属，不同步客户端
    CE_FLAG_PERSIST        = 0x08,   // 标记脏时同步到 DBProxy 存档
} CeReplFlag;
```

### 4.2 字段描述符

```c
typedef enum CeReplFieldType {
    CE_REPL_TYPE_I8,
    CE_REPL_TYPE_I16,
    CE_REPL_TYPE_I32,
    CE_REPL_TYPE_I64,
    CE_REPL_TYPE_U8,
    CE_REPL_TYPE_U16,
    CE_REPL_TYPE_U32,
    CE_REPL_TYPE_U64,
    CE_REPL_TYPE_F32,
    CE_REPL_TYPE_F64,
    CE_REPL_TYPE_BOOL,
    CE_REPL_TYPE_STRING,
    CE_REPL_TYPE_VEC2,      // {float x, y}
    CE_REPL_TYPE_VEC3,      // {float x, y, z}
    CE_REPL_TYPE_VEC4,      // {float x, y, z, w}
    CE_REPL_TYPE_QUAT,      // {float x, y, z, w}
    CE_REPL_TYPE_BLOB,      // 变长二进制
} CeReplFieldType;

typedef struct CeReplConstraint {
    bool     has_min;         // 是否有最小值约束
    bool     has_max;         // 是否有最大值约束
    int64_t  min_value;       // 最小值 (类型擦除，按 field->type 解释)
    int64_t  max_value;       // 最大值
} CeReplConstraint;

typedef struct CeReplField {
    const char*        name;           // 字段名 (与 FlatBuffer schema 一致)
    CeReplFieldType    type;           // 字段类型
    uint32_t           flags;          // 位掩码: CE_FLAG_*
    uint32_t           offset;         // 字段在组件结构体中的偏移 (offsetof)
    uint32_t           size;           // 字段大小 (sizeof)
    CeReplConstraint   constraint;     // 值域约束 (可选，{0} 表示无约束)
} CeReplField;
```

### 4.3 组件注册示例

```c
// 注册 Player 组件的可复制字段
ce_replication_register_component(COMP_PLAYER, (CeReplField[]){
    {
        .name       = "position",
        .type       = CE_REPL_TYPE_VEC3,
        .flags      = CE_FLAG_AOI_BROADCAST | CE_FLAG_PERSIST,
        .offset     = offsetof(PlayerComponent, position),
        .size       = sizeof(Vec3),
        .constraint = {0},  // 无约束
    },
    {
        .name       = "rotation",
        .type       = CE_REPL_TYPE_QUAT,
        .flags      = CE_FLAG_AOI_BROADCAST,
        .offset     = offsetof(PlayerComponent, rotation),
        .size       = sizeof(Quat),
        .constraint = {0},
    },
    {
        .name       = "hp",
        .type       = CE_REPL_TYPE_I32,
        .flags      = CE_FLAG_AOI_BROADCAST | CE_FLAG_PERSIST,
        .offset     = offsetof(PlayerComponent, hp),
        .size       = sizeof(int32_t),
        .constraint = { .has_min = true, .min_value = 0,
                        .has_max = true, .max_value = 999999 },
    },
    {
        .name       = "max_hp",
        .type       = CE_REPL_TYPE_I32,
        .flags      = CE_FLAG_AOI_BROADCAST | CE_FLAG_PERSIST,
        .offset     = offsetof(PlayerComponent, max_hp),
        .size       = sizeof(int32_t),
        .constraint = { .has_min = true, .min_value = 1,
                        .has_max = true, .max_value = 999999 },
    },
    {
        .name       = "mp",
        .type       = CE_REPL_TYPE_I32,
        .flags      = CE_FLAG_OWNER_ONLY | CE_FLAG_PERSIST,
        .offset     = offsetof(PlayerComponent, mp),
        .size       = sizeof(int32_t),
        .constraint = { .has_min = true, .min_value = 0,
                        .has_max = true, .max_value = 999999 },
    },
    {
        .name       = "inventory",
        .type       = CE_REPL_TYPE_BLOB,
        .flags      = CE_FLAG_OWNER_ONLY | CE_FLAG_PERSIST,
        .offset     = offsetof(PlayerComponent, inventory),
        .size       = sizeof(InventoryData),
        .constraint = {0},
    },
    {
        .name       = "skill_cooldowns",
        .type       = CE_REPL_TYPE_BLOB,
        .flags      = CE_FLAG_OWNER_ONLY,       // 不存盘
        .offset     = offsetof(PlayerComponent, skill_cooldowns),
        .size       = sizeof(SkillCooldownData),
        .constraint = {0},
    },
    {
        .name       = "ai_state",
        .type       = CE_REPL_TYPE_I32,
        .flags      = CE_FLAG_SERVER_ONLY,       // 纯服务器
        .offset     = offsetof(PlayerComponent, ai_state),
        .size       = sizeof(int32_t),
        .constraint = {0},
    },
    {0}  // 终止标记
});
```

### 4.4 Flag 组合规则

| 组合 | 含义 | 示例 |
|------|------|------|
| `AOI \| PERSIST` | 周围玩家可见 + 存档 | position, hp |
| `AOI` | 周围玩家可见，不存档 | rotation (实时变化) |
| `OWNER \| PERSIST` | 仅自己可见 + 存档 | inventory, mp |
| `OWNER` | 仅自己可见，不存档 | skill_cooldowns |
| `SERVER` | 服务器内部 | ai_state, login_token |
| `SERVER \| PERSIST` | 服务器内部 + 存档 | quest_progress (仅服务器校验) |

### 4.5 编译期校验 & 启动失败

**原则**: 数值越界是 bug，不是运行时行为。不 clamp，直接拒绝启动。

#### 字段约束声明

```c
typedef struct CeReplConstraint {
    bool     has_min;         // 是否有最小值约束
    bool     has_max;         // 是否有最大值约束
    int64_t  min_value;       // 最小值 (类型擦除，按 field->type 解释)
    int64_t  max_value;       // 最大值
} CeReplConstraint;

typedef struct CeReplField {
    const char*        name;
    CeReplFieldType    type;
    uint32_t           flags;
    uint32_t           offset;
    uint32_t           size;
    CeReplConstraint   constraint;    // 值域约束
} CeReplField;
```

#### 注册示例

```c
ce_replication_register_component(COMP_PLAYER, (CeReplField[]){
    {
        .name       = "hp",
        .type       = CE_REPL_TYPE_I32,
        .flags      = CE_FLAG_AOI_BROADCAST | CE_FLAG_PERSIST,
        .offset     = offsetof(PlayerComponent, hp),
        .size       = sizeof(int32_t),
        .constraint = { .has_min = true, .min_value = 0,
                        .has_max = true, .max_value = 999999 },
    },
    {
        .name       = "level",
        .type       = CE_REPL_TYPE_U8,
        .flags      = CE_FLAG_AOI_BROADCAST | CE_FLAG_PERSIST,
        .offset     = offsetof(PlayerComponent, level),
        .size       = sizeof(uint8_t),
        .constraint = { .has_min = true, .min_value = 1,
                        .has_max = true, .max_value = 100 },
    },
    {0}
});
```

#### 编译期检查 (常量初始化)

对于编译期已知的常量初始化值，使用 `static_assert`：

```c
// ce_replication.h

// 编译期断言宏：字段初始值必须在约束范围内
#define CE_REPL_CHECK_INIT(field_type, field_name, init_value, constraint) \
    _Static_assert(                                                        \
        (!(constraint).has_min || (init_value) >= (constraint).min_value)  \
        &&                                                                \
        (!(constraint).has_max || (init_value) <= (constraint).max_value), \
        "ERROR: " #field_name " initial value out of range"                \
    )

// 使用示例
#define PLAYER_INITIAL_HP 100
CE_REPL_CHECK_INIT(int32_t, hp, PLAYER_INITIAL_HP,
    ((CeReplConstraint){.has_min=true, .min_value=0, .has_max=true, .max_value=999999}));
```

#### 启动时校验 (数据驱动初始化)

对于从配置/数据库加载的初始值，在 `ce_repl_init` 阶段逐字段校验：

```c
// ce_replication.c

static CeResult ce_repl_validate_initial_values(CeReplContext* ctx) {
    for (uint32_t comp_id = 0; comp_id < ctx->component_count; comp_id++) {
        const CeReplComponent* comp = &ctx->components[comp_id];
        for (uint32_t fi = 0; fi < comp->field_count; fi++) {
            const CeReplField* field = &comp->fields[fi];
            const CeReplConstraint* c = &field->constraint;

            if (!c->has_min && !c->has_max) continue;

            // 从默认组件模板读取初始值
            int64_t value = ce_repl_read_field_value(field->type,
                (uint8_t*)comp->default_template + field->offset);

            if (c->has_min && value < c->min_value) {
                CE_LOG_ERROR("Replication: field '%s.%s' initial value %ld < min %ld. "
                             "Server refusing to start.",
                             comp->name, field->name, value, c->min_value);
                return CE_ERR_VALIDATION;
            }
            if (c->has_max && value > c->max_value) {
                CE_LOG_ERROR("Replication: field '%s.%s' initial value %ld > max %ld. "
                             "Server refusing to start.",
                             comp->name, field->name, value, c->max_value);
                return CE_ERR_VALIDATION;
            }
        }
    }
    return CE_OK;
}
```

#### 运行时行为

```c
// ECS setter — 不 clamp，直接写入
// 如果调用方传入越界值，这是调用方的 bug
CeResult ce_ecs_set_component(CeEntity entity, uint32_t component_id, void* data) {
    // 直接写入，不检查范围
    memcpy(dst, data, component_size);

    // 标记脏
    ce_repl_mark_dirty(entity, component_id);

    return CE_OK;
}
```

#### 校验层级总结

```
编译期 (static_assert)
  ├─ 常量初始值越界 → 编译失败
  │
启动时 (ce_repl_validate_initial_values)
  ├─ 配置/数据库加载的初始值越界 → 起服失败，打印错误日志
  │
运行时
  └─ 不检查，不 clamp。越界是调用方 bug，由上层逻辑保证正确性
```

---

## 5. 脏标管线

### 5.1 脏标生命周期

```
ECS setter 写入组件字段
    │
    ▼
ce_repl_mark_dirty(entity_id, field_index)
    │
    ▼
ReplMgr 内部 dirty_bitset[entity_id] 置位
    │
    ├── RPC 合并发送时 → 收集当前脏字段 → 打包 → 发送 → 清除已发送字段的脏标
    │
    └── 帧末 flush → 收集剩余脏字段 → 打包 → 发送 → 清除所有脏标
```

### 5.2 脏标数据结构

```c
// 每个 entity 的脏标位图
typedef struct CeReplDirtyMask {
    uint64_t    bits[4];    // 最多 256 个字段 (4 × 64)
} CeReplDirtyMask;

// 全局脏标表
typedef struct CeReplDirtyTable {
    uint32_t        capacity;       // 容量
    uint32_t        count;          // 当前脏实体数
    uint64_t*       entity_ids;     // 脏实体 ID 列表
    CeReplDirtyMask* masks;         // 每个实体的脏字段位图
    uint32_t*       hash_buckets;   // entity_id → index 哈希表
} CeReplDirtyTable;
```

### 5.3 自动脏标

ECS 组件 setter 自动调用脏标：

```c
// 在 ce_ecs.c 的 set_component 中
CeResult ce_ecs_set_component(CeEntity entity, uint32_t component_id, void* data) {
    // ... 写入组件数据 ...
    
    // 自动标记脏
    ce_repl_mark_dirty(entity, component_id);
    
    return CE_OK;
}
```

### 5.4 手动脏标

对于批量操作或外部修改，提供手动接口：

```c
// 标记整个组件的所有字段为脏
void ce_repl_mark_component_dirty(CeEntity entity, uint32_t component_id);

// 标记单个字段为脏
void ce_repl_mark_field_dirty(CeEntity entity, uint32_t component_id, uint32_t field_index);
```

---

## 6. RPC 系统

### 6.1 RPC 定义

```c
// RPC 可靠性
typedef enum CeRpcReliability {
    CE_RPC_UNRELIABLE = 0,   // 默认: KCP (客户端) / UDP (可丢)
    CE_RPC_RELIABLE   = 1,   // TCP 保证送达
} CeRpcReliability;

// RPC 发送标志
typedef enum CeRpcSendFlag {
    CE_RPC_SEND_NONE        = 0x00,   // 默认: 不合并，RPC 立即发送
    CE_RPC_SEND_MERGE_ATTRS = 0x01,   // 合并当前脏属性到一个消息包
} CeRpcSendFlag;

// RPC 目标
typedef enum CeRpcTarget {
    CE_RPC_TARGET_CLIENT = 0,   // 发给属主客户端
    CE_RPC_TARGET_SERVER = 1,   // 发给目标 entity 所在服务器
    CE_RPC_TARGET_AOI    = 2,   // 广播给 AOI 内所有客户端
} CeRpcTarget;
```

### 6.2 RPC 调用接口

```c
/**
 * 发送 RPC
 *
 * @param entity_id     目标实体 (用于寻址)
 * @param target        目标类型
 * @param reliability   可靠性
 * @param send_flags    发送标志 (合并属性等)
 * @param method        RPC 方法名 (如 "cast_skill")
 * @param params        FlatBuffer 序列化的参数
 * @param params_len    参数长度
 * @return              CE_OK 成功
 */
CeResult ce_rpc_send(
    uint64_t        entity_id,
    CeRpcTarget     target,
    CeRpcReliability reliability,
    CeRpcSendFlag   send_flags,
    const char*     method,
    const uint8_t*  params,
    uint32_t        params_len
);

/**
 * 注册 RPC 方法处理器
 *
 * @param method    方法名
 * @param handler   处理函数
 * @param user_data 用户数据
 */
CeResult ce_rpc_register_handler(
    const char*     method,
    CeRpcHandler    handler,
    void*           user_data
);

/**
 * RPC 处理函数签名
 *
 * @param entity_id  来源实体
 * @param params     FlatBuffer 参数
 * @param params_len 参数长度
 * @param user_data  注册时的用户数据
 */
typedef void (*CeRpcHandler)(
    uint64_t        entity_id,
    const uint8_t*  params,
    uint32_t        params_len,
    void*           user_data
);
```

### 6.3 使用示例

```c
// Client→Server: 使用技能 (不可靠，不合并)
SkillCastParams params = { .skill_id = 3, .target_id = 456 };
ce_rpc_send(player_entity, CE_RPC_TARGET_SERVER, CE_RPC_UNRELIABLE,
            CE_RPC_SEND_NONE, "cast_skill", &params, sizeof(params));

// Server→Client: 播放特效 + 合并属性变化 (可靠)
ce_rpc_send(player_entity, CE_RPC_TARGET_CLIENT, CE_RPC_RELIABLE,
            CE_RPC_SEND_MERGE_ATTRS, "play_effect", &effect, sizeof(effect));
// → 一个 FlatBuffer 包: [header][rpc: play_effect][attrs: {hp:80, mp:45}]

// Server→Server: 跨服公会操作 (可靠，TCP)
ce_rpc_send(target_player, CE_RPC_TARGET_SERVER, CE_RPC_RELIABLE,
            CE_RPC_SEND_NONE, "guild_invite", &invite, sizeof(invite));
// → Router 查 mailbox: target_player → Game B → TCP 转发
```

---

## 7. Mailbox 路由

### 7.1 概念

Mailbox 是 Actor 模型的核心寻址机制。每个玩家实体上线时在 Router 注册 mailbox，Router 维护 `entity_id → mailbox → server_id` 映射。

### 7.2 生命周期

```
玩家登录:
  Client → Gateway → Game A
    │
    ▼
  Game A 创建 Player Entity (entity_id = 123)
    │
    ▼
  Game A → Router: REGISTER_MAILBOX {entity_id: 123, server_id: "game_a"}
    │
    ▼
  Router 记录: 123 → mailbox_123 → game_a

玩家下线:
  Game A → Router: UNREGISTER_MAILBOX {entity_id: 123}
    │
    ▼
  Router 删除映射
```

### 7.3 Router 数据结构

```c
// Router 内部
typedef struct CeMailboxEntry {
    uint64_t    entity_id;
    uint32_t    server_id;      // 目标 Game Server 的内部 ID
    uint64_t    created_at;     // 创建时间戳
} CeMailboxEntry;

typedef struct CeMailboxTable {
    CeMailboxEntry* entries;
    uint32_t        count;
    uint32_t        capacity;
    // entity_id → index 哈希表，O(1) 查找
} CeMailboxTable;
```

### 7.4 寻址流程

```
Game B 调用 ce_rpc_send(entity_123, CE_RPC_TARGET_SERVER, ...)
    │
    ▼
Game B → Router: RPC_FORWARD {target_entity: 123, method: "guild_invite", ...}
    │
    ▼
Router 查 mailbox: 123 → game_a
    │
    ▼
Router → Game A: RPC_FORWARD {source_server: "game_b", method: "guild_invite", ...}
    │
    ▼
Game A 收到 RPC，调用注册的 handler
```

---

## 8. FlatBuffers 协议

### 8.1 Schema 目录

```
schemas/
├── chaos_replication.fbs    # 属性同步消息
├── chaos_rpc.fbs            # RPC 消息
├── chaos_common.fbs         # 公共类型 (Vec3, Quat, EntityId...)
└── CMakeLists.txt           # flatc 编译规则
```

### 8.2 公共类型

```fbs
// schemas/chaos_common.fbs

namespace ChaosEngine;

struct Vec2 { x: float; y: float; }
struct Vec3 { x: float; y: float; z: float; }
struct Vec4 { x: float; y: float; z: float; w: float; }
struct Quat { x: float; y: float; z: float; w: float; }

table EntityId {
    id: uint64;
}
```

### 8.3 属性同步消息

```fbs
// schemas/chaos_replication.fbs

include "chaos_common.fbs";

namespace ChaosEngine;

// 单个字段的 delta 值
union FieldValue {
    Int8Value, Int16Value, Int32Value, Int64Value,
    Uint8Value, Uint16Value, Uint32Value, Uint64Value,
    FloatValue, DoubleValue, BoolValue,
    StringValue, Vec2Value, Vec3Value, Vec4Value, QuatValue,
    BlobValue
}

table Int8Value    { v: int8; }
table Int16Value   { v: int16; }
table Int32Value   { v: int32; }
table Int64Value   { v: int64; }
table Uint8Value   { v: uint8; }
table Uint16Value  { v: uint16; }
table Uint32Value  { v: uint32; }
table Uint64Value  { v: uint64; }
table FloatValue   { v: float; }
table DoubleValue  { v: double; }
table BoolValue    { v: bool; }
table StringValue  { v: string; }
table Vec2Value    { v: Vec2; }
table Vec3Value    { v: Vec3; }
table Vec4Value    { v: Vec4; }
table QuatValue    { v: Quat; }
table BlobValue    { v: [ubyte]; }

// 单个字段变化
table FieldDelta {
    component_id: uint16;       // 组件类型 ID
    field_index:  uint16;       // 字段在组件中的索引
    value:        FieldValue;   // 字段值
}

// 单个实体的属性变化
table EntityDelta {
    entity_id: uint64;          // 实体 ID
    fields:    [FieldDelta];    // 变化的字段列表
}

// 属性同步消息 (帧末批量)
table ReplicationMessage {
    frame_id:   uint64;         // 帧序号
    timestamp:  uint64;         // 服务器时间戳 (微秒)
    entities:   [EntityDelta];  // 所有脏实体的 delta
}

// AOI 进入全量同步
table EntityFullSync {
    entity_id: uint64;
    fields:    [FieldDelta];    // 所有非 SERVER_ONLY 字段
}

// AOI 离开通知
table EntityLeave {
    entity_id: uint64;
}
```

### 8.4 RPC 消息

```fbs
// schemas/chaos_rpc.fbs

include "chaos_common.fbs";
include "chaos_replication.fbs";

namespace ChaosEngine;

// RPC 调用
table RpcCall {
    method:      string;        // 方法名
    params:      [ubyte];       // 参数 (方法自定义 FlatBuffer)
    reliability: uint8;         // 0=unreliable, 1=reliable
    call_id:     uint32;        // 调用 ID (用于可靠 RPC 的 ack)
}

// RPC 响应
table RpcResponse {
    call_id:     uint32;
    result:      [ubyte];       // 返回值
    error_code:  int32;         // 0=成功
}

// 合并消息: RPC + 属性同步
table MergedMessage {
    rpc:     RpcCall;                   // RPC 调用 (可选)
    repl:    ReplicationMessage;        // 属性同步 (可选)
}

// 顶层消息包装
union MessagePayload {
    RpcCall, RpcResponse,
    ReplicationMessage,
    EntityFullSync, EntityLeave,
    MergedMessage
}

table Message {
    msg_type:  uint8;           // 消息类型
    payload:   MessagePayload;
}

// 消息类型枚举
enum MessageType : uint8 {
    RPC_CALL              = 0,
    RPC_RESPONSE          = 1,
    REPLICATION           = 2,
    ENTITY_FULL_SYNC      = 3,
    ENTITY_LEAVE          = 4,
    MERGED                = 5,
}
```

### 8.5 编译

```cmake
# schemas/CMakeLists.txt
find_program(FLATC_EXECUTABLE flatc REQUIRED)

set(FBS_FILES
    chaos_common.fbs
    chaos_replication.fbs
    chaos_rpc.fbs
)

flatbuffers_generate_headers(
    TARGET chaos_fbs
    SCHEMAS ${FBS_FILES}
    FLAGS --cpp --gen-object-api
    OUTPUT_DIR "${CMAKE_BINARY_DIR}/generated"
)
```

---

## 9. 发送时序

### 9.1 帧内时序

```
Server Loop 一帧:

  t=0  tick 开始
  │
  ├─ System A: 修改 entity.hp (80→70)
  │   └─ ce_ecs_set_component() → 自动标记脏
  │
  ├─ System B: ce_rpc_send(entity, MERGE_ATTRS, "cast_skill", ...)
  │   └─ 收集当前脏字段 {hp:70}
  │   └─ 打包: [RPC: cast_skill][ATTRS: {hp:70}]
  │   └─ 立即发送到 Gateway
  │   └─ 清除 hp 脏标
  │
  ├─ System C: 修改 entity.mp (100→45)
  │   └─ ce_ecs_set_component() → 自动标记脏
  │
  ├─ System D: ce_rpc_send(entity, NONE, "play_sound", ...)
  │   └─ 打包: [RPC: play_sound]  (不合并属性)
  │   └─ 立即发送到 Gateway
  │   └─ 不清理脏标
  │
  t=N  tick 结束
  │
  └─ ReplMgr.flush()
      └─ 收集剩余脏字段 {mp:45}
      └─ 按 flag 分流:
          ├─ mp 是 OWNER_ONLY → 发给 entity 属主客户端
          └─ 打包: [ATTRS: {mp:45}]
      └─ 清除所有脏标
```

### 9.2 保序规则

| 场景 | 保序？ | 说明 |
|------|--------|------|
| RPC 合并发送 | ✅ | RPC + 属性在同一消息包内 |
| RPC 不合并 + 属性 | ❌ | RPC 先发，属性帧末发，不保证相对顺序 |
| 多个 RPC (不合并) | ❌ | 各自独立发送，不保序 |
| 多个 RPC (合并) | ✅ | 每个合并包内 RPC+属性保序，但包之间不保序 |

---

## 10. AOI 集成

### 10.1 进入 AOI

```
Entity B 进入 Entity A 的 AOI 范围:
    │
    ▼
AOI 触发 CE_AOI_EVENT_ENTER
    │
    ▼
ReplMgr 收到事件:
    ├─ 收集 Entity B 的所有非 SERVER_ONLY 字段
    ├─ 打包为 EntityFullSync
    └─ 发送给 Entity A 的客户端
```

### 10.2 离开 AOI

```
Entity B 离开 Entity A 的 AOI 范围:
    │
    ▼
AOI 触发 CE_AOI_EVENT_LEAVE
    │
    ▼
ReplMgr 收到事件:
    └─ 发送 EntityLeave{entity_id: B} 给 Entity A 的客户端
```

### 10.3 AOI 范围内属性广播

```
帧末 flush:
    │
    ▼
ReplMgr 遍历脏实体:
    │
    ├─ entity.hp 变化 (flag: AOI_BROADCAST)
    │   ├─ 查 AOI: entity 周围有 Client1, Client2, Client3
    │   └─ 发送 EntityDelta{hp: 70} 给 Client1, Client2, Client3
    │
    └─ entity.mp 变化 (flag: OWNER_ONLY)
        └─ 发送 EntityDelta{mp: 45} 仅给 entity 属主 Client1
```

---

## 11. DBProxy 集成

### 11.1 过滤规则

Game→DBProxy 只同步标记了 `CE_FLAG_PERSIST` 的字段：

```
帧末 flush:
    │
    ▼
ReplMgr 收集脏字段:
    │
    ├─ entity.hp  (AOI | PERSIST) → 包含在 DBProxy 同步中
    ├─ entity.mp  (OWNER | PERSIST) → 包含在 DBProxy 同步中
    ├─ entity.rotation (AOI) → 跳过 (无 PERSIST)
    └─ entity.skill_cd (OWNER) → 跳过 (无 PERSIST)
    │
    ▼
打包 PERSIST 字段 → ce_sync_send_frame() → DBProxy → MongoDB
```

### 11.2 与现有 ce_sync 的关系

`ce_sync` 模块保持不变，ReplMgr 作为上层调用者：

```c
// 帧末 flush 中
void ce_repl_flush_to_dbproxy(CeReplContext* ctx) {
    CeSyncFrame frame = {0};
    
    // 遍历脏实体，只收集 PERSIST 字段
    for (每个脏实体) {
        for (每个脏字段) {
            if (字段有 CE_FLAG_PERSIST) {
                // 添加到 frame
            }
        }
    }
    
    // 通过现有 ce_sync 发送
    ce_sync_send_frame(ctx->sync_ctx, &frame);
}
```

---

## 12. Gateway 集成

### 12.1 连接模型

Gateway 与同机所有 Game Server 建立 TCP 全连接：

```
机器 1:
  Gateway ──TCP──► Game A (:9100)
          ──TCP──► Game B (:9101)
          ──TCP──► Game C (:9102)

机器 2:
  Gateway ──TCP──► Game D (:9100)
          ──TCP──► Game E (:9101)
```

### 12.2 配置

```lua
-- gateway/config.lua
local config = {
    game_servers = {
        { host = "127.0.0.1", port = 9100 },
        { host = "127.0.0.1", port = 9101 },
        { host = "127.0.0.1", port = 9102 },
    },
    -- TODO: 后续改为向 Router 查询本机 Game 列表
}
```

### 12.3 Gateway 职责

```
Gateway 收到 Game 发来的消息:
    │
    ├─ EntityDelta → 转发给目标客户端 (KCP 或 TCP)
    ├─ EntityFullSync → 转发给目标客户端
    ├─ EntityLeave → 转发给目标客户端
    └─ RpcCall (Server→Client) → 转发给目标客户端

Gateway 收到 Client 发来的消息:
    │
    ├─ RpcCall (Client→Server) → 转发给 entity 所在 Game
    └─ 属性同步请求 → 转发给 entity 所在 Game
```

---

## 13. 模块设计

### 13.1 新增文件

```
src_c/replication/
├── ce_replication.h          # 公共 API
├── ce_replication.c          # 核心实现 (脏标管线、flush、分流)
├── ce_replication_internal.h # 内部数据结构
├── ce_mailbox.h              # Mailbox 管理
├── ce_mailbox.c              # Mailbox 注册/注销/查询
├── ce_rpc_channel.h          # RPC 通道 (与协议解耦)
└── ce_rpc_channel.c          # RPC 收发、合并逻辑

schemas/
├── chaos_common.fbs          # 公共类型
├── chaos_replication.fbs     # 属性同步消息
├── chaos_rpc.fbs             # RPC 消息
└── CMakeLists.txt            # flatc 编译

tests/unit/
├── test_replication.c        # 脏标管线测试
├── test_mailbox.c            # Mailbox 路由测试
└── test_rpc_channel.c        # RPC 通道测试
```

### 13.2 修改文件

| 文件 | 修改内容 |
|------|---------|
| `src_c/ecs/ce_ecs.c` | `ce_ecs_set_component` 中调用 `ce_repl_mark_dirty` |
| `src_c/server/ce_aoi.c` | AOI 事件回调中通知 ReplMgr |
| `src_c/runtime/ce_server_main.c` | 初始化 ReplMgr + Mailbox |
| `src_c/runtime/ce_gateway_main.c` | 连接同机 Game，转发消息 |
| `CMakeLists.txt` | 添加 `engine_replication` 库 + `schemas/` 子目录 |
| `src_lua/gateway/config.lua` | 添加 `game_servers` 配置 |

### 13.3 模块依赖

```
engine_replication
    ├── engine_core      (ce_types, ce_memory, ce_time)
    ├── engine_ecs       (ce_ecs_set_component 中调用脏标)
    ├── engine_server    (AOI 事件回调)
    ├── engine_sync      (DBProxy 存档)
    └── FlatBuffers      (消息序列化)
```

---

## 14. API 定义

### 14.1 Replication Manager

```c
// ce_replication.h

/** 初始化复制管理器
 *
 * 初始化完成后自动调用 ce_repl_validate_initial_values() 校验所有已注册字段。
 * 校验失败返回 NULL，服务器应拒绝启动。
 */
CeReplContext* ce_repl_init(CeReplConfig* config);
void ce_repl_shutdown(CeReplContext* ctx);

/** 注册组件的可复制字段
 *
 * @param default_template  组件的默认值模板 (用于启动时校验初始值)
 */
CeResult ce_repl_register_component(
    CeReplContext*  ctx,
    uint32_t        component_id,
    const char*     component_name,
    const CeReplField* fields,
    const void*     default_template
);

/** 校验所有已注册字段的初始值是否在约束范围内
 *
 * 在 ce_repl_init 中自动调用。也可手动调用用于热更新场景。
 * 返回 CE_OK 表示全部通过，CE_ERR_VALIDATION 表示有字段越界。
 */
CeResult ce_repl_validate_initial_values(CeReplContext* ctx);

/** 标记字段脏 (ECS setter 自动调用) */
void ce_repl_mark_dirty(CeReplContext* ctx, uint64_t entity_id, uint32_t component_id);
void ce_repl_mark_field_dirty(CeReplContext* ctx, uint64_t entity_id, uint32_t component_id, uint32_t field_index);

/** 帧末 flush: 收集脏字段 → 按 flag 分流 → 发送 */
void ce_repl_flush(CeReplContext* ctx);

/** 每帧 tick (处理 AOI 事件、超时等) */
void ce_repl_tick(CeReplContext* ctx, float dt);

/** 设置 Gateway 连接 (用于发送到客户端) */
void ce_repl_set_gateway(CeReplContext* ctx, CeGatewayConn* gateway);

/** 设置 DBProxy 连接 (用于存档) */
void ce_repl_set_dbproxy(CeReplContext* ctx, CeSyncContext* sync);
```

### 14.2 RPC Channel

```c
// ce_rpc_channel.h

/** 发送 RPC */
CeResult ce_rpc_send(
    CeReplContext*  ctx,
    uint64_t        entity_id,
    CeRpcTarget     target,
    CeRpcReliability reliability,
    CeRpcSendFlag   send_flags,
    const char*     method,
    const uint8_t*  params,
    uint32_t        params_len
);

/** 注册 RPC 处理器 */
CeResult ce_rpc_register_handler(
    CeReplContext*  ctx,
    const char*     method,
    CeRpcHandler    handler,
    void*           user_data
);

/** 处理收到的 RPC (由网络层调用) */
void ce_rpc_dispatch(
    CeReplContext*  ctx,
    uint64_t        source_entity,
    const uint8_t*  data,
    uint32_t        data_len
);
```

### 14.3 Mailbox

```c
// ce_mailbox.h

/** 注册 mailbox (玩家上线) */
CeResult ce_mailbox_register(CeReplContext* ctx, uint64_t entity_id);

/** 注销 mailbox (玩家下线) */
void ce_mailbox_unregister(CeReplContext* ctx, uint64_t entity_id);

/** 查询 entity 所在服务器 (本地缓存) */
CeBool ce_mailbox_lookup(CeReplContext* ctx, uint64_t entity_id, uint32_t* server_id);

/** 向 Router 同步 mailbox 注册 */
CeResult ce_mailbox_sync_to_router(CeReplContext* ctx, uint64_t entity_id, CeBool is_register);
```

---

## 15. MVP 范围

### 15.1 Phase 1: 核心脏标管线 (MVP)

- [ ] `ce_replication.h/c` — 字段注册、脏标、flush
- [ ] ECS 集成 — `ce_ecs_set_component` 自动标记脏
- [ ] FlatBuffers schema 定义 + 编译
- [ ] 帧末 flush: 收集脏字段 → FlatBuffer 序列化
- [ ] 单元测试: 脏标标记/清除、字段注册、序列化往返

### 15.2 Phase 2: RPC 通道

- [ ] `ce_rpc_channel.h/c` — RPC 发送/接收/分发
- [ ] 合并发送 (MERGE_ATTRS)
- [ ] RPC handler 注册
- [ ] 单元测试: RPC 收发、合并、handler 分发

### 15.3 Phase 3: Mailbox + 路由

- [ ] `ce_mailbox.h/c` — 注册/注销/查询
- [ ] Router 集成 — mailbox 表维护
- [ ] Server→Server RPC 寻址
- [ ] 单元测试: mailbox 注册/查询、跨服 RPC 路由

### 15.4 Phase 4: Gateway + AOI 集成

- [ ] Gateway 同机 Game 全连接
- [ ] AOI 进入/离开全量同步
- [ ] AOI 范围内属性广播
- [ ] 客户端双通道 (KCP + TCP)
- [ ] 集成测试: 多客户端 AOI 同步

### 15.5 Phase 5: DBProxy 集成

- [ ] PERSIST 字段过滤
- [ ] 帧末 flush 到 DBProxy
- [ ] 集成测试: 属性变化 → DBProxy 存档

---

## 16. 路线图

| 版本 | 内容 | 预计 |
|------|------|------|
| v0.1 | Phase 1: 核心脏标管线 | MVP |
| v0.2 | Phase 2: RPC 通道 | |
| v0.3 | Phase 3: Mailbox + 路由 | |
| v0.4 | Phase 4: Gateway + AOI 集成 | |
| v0.5 | Phase 5: DBProxy 集成 | |
| v1.0 | 全功能 + 性能优化 + 压测 | |

### 16.1 TODO 清单

- [ ] Gateway 发现 Game 从配置改为向 Router 查询本机 IP 下的 Game 列表
- [ ] 可靠 RPC 的 ack/重传机制
- [ ] 客户端预测 + 服务器和解 (Client-side prediction + reconciliation)
- [ ] Delta 压缩 (只发变化字段 vs 全字段)
- [ ] 带宽限流 (每客户端每帧最大字节数)
- [ ] 优先级队列 (关键属性优先发送)

---

## 17. 参考分析

### 17.1 Unreal Engine 属性复制

- **借鉴**: 字段级 `Replicated` / `RepNotify` 标记
- **差异**: UE 的 `Condition` (COND_OwnerOnly, COND_SkipOwner) 对应我们的 `CE_FLAG_OWNER_ONLY`
- **差异**: UE 没有内置 PERSIST flag，存档由 GameMode/GameState 自行处理

### 17.2 KBEngine

- **借鉴**: Entity 属性同步 + AOI 广播
- **借鉴**: Mailbox 概念 (BASEAPP ↔ CELLAPP 通信)
- **差异**: KBEngine 的 Mailbox 是 Lua 层概念，我们下沉到 C 层

### 17.3 Skynet

- **借鉴**: Actor 模型 + 服务间消息传递
- **借鉴**: 服务地址 = handle，类似我们的 entity_id → mailbox 映射
- **差异**: Skynet 无 ECS，消息传递是通用机制

### 17.4 FlatBuffers vs Protobuf

| 特性 | FlatBuffers | Protobuf |
|------|-------------|----------|
| 零拷贝 | ✅ 直接访问，无需解析 | ❌ 需要 parse |
| 多语言 | ✅ C/C++/Lua/C#/... | ✅ |
| 性能 | 极快 (无分配) | 快 (需分配) |
| Schema 演进 | ✅ forwards/backwards 兼容 | ✅ |
| 消息大小 | 略大 (有 offset 表) | 更紧凑 |
| **选择理由** | 游戏实时同步要求零拷贝低延迟 | |

---

> **下一步**: 等待确认后进入 Phase 1 实现。
