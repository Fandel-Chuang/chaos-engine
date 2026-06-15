# ChaosEngine 设计规格说明书 v0.1

> **状态：** 草案 | **日期：** 2026-06-15 | **作者：** zhongfangdao

---

## 目录

1. [愿景与定位](#1-愿景与定位)
2. [核心架构](#2-核心架构)
3. [模块设计](#3-模块设计)
4. [客户端/服务器同构方案](#4-客户端服务器同构方案)
5. [ECS 架构设计](#5-ecs-架构设计)
6. [渲染抽象层 (RHI)](#6-渲染抽象层-rhi)
7. [网络层设计](#7-网络层设计)
8. [插件系统](#8-插件系统)
9. [脚本层 (Lua)](#9-脚本层-lua)
10. [编辑器架构](#10-编辑器架构)
11. [日志与观测系统（第八模式）](#11-日志与观测系统第八模式)
12. [Git 分支模型与 CI/CD](#12-git-分支模型与-cicd)
13. [MVP v0.1 范围](#13-mvp-v01-范围)
14. [开发路线图](#14-开发路线图)
15. [附录：参考引擎分析](#15-附录参考引擎分析)

---

## 1. 愿景与定位

### 1.1 一句话定义

**ChaosEngine** 是一个面向 3D 多人游戏的、客户端/服务器同构的、纯 C 内核 + Lua 脚本的轻量级开源游戏引擎。

### 1.2 核心设计原则

| 原则 | 说明 |
|------|------|
| **极度简洁** | 内核代码量最小化，每个模块只做一件事 |
| **高效执行** | 纯 C 内核，无虚函数开销，Cache-Friendly 数据布局 |
| **同构逻辑** | 同一份战斗/游戏逻辑代码，客户端跑帧同步，服务端跑状态同步 |
| **严格分层** | C 内核 → public_api → C++ 编辑器，三层隔离，单向依赖 |
| **全平台** | Windows / Linux / macOS / iOS / Android |
| **AI 友好** | 代码结构、命名、日志全部面向 AI 可解析设计 |

### 1.3 参考引擎与借鉴点

| 引擎 | 借鉴内容 |
|------|----------|
| **Skynet** | Actor 并发模型、Lua 服务化架构、轻量级消息传递 |
| **KBEngine** | Cell 空间划分、动态 AOI、多人同屏管理、Base/Cell 双层实体 |
| **Ant Engine** | 纯 ECS 架构、Archetype 存储、System 调度管线 |
| **Unreal Engine** | Gameplay 框架（Actor/Component 概念）、属性复制系统、网络优先级 |

### 1.4 不做什么（明确边界）

- ❌ 不做可视化编程（蓝图）
- ❌ 不做完整的物理引擎（集成 Bullet/PhysX）
- ❌ 不做资源商店/市场
- ❌ 不做 UE/Unity 级别的编辑器
- ❌ 首版不做 PBR 完整管线

---

## 2. 核心架构

### 2.1 分层架构图

```
┌─────────────────────────────────────────────────────────┐
│                    编辑器层 (C++17)                       │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌────────────┐  │
│  │   UI     │ │ Importer │ │  Editor  │ │ LogObserver │  │
│  │ DearImGui│ │ 资源导入  │ │  Logic   │ │  第八模式   │  │
│  └──────────┘ └──────────┘ └──────────┘ └────────────┘  │
├─────────────────────────────────────────────────────────┤
│                  public_api (纯 C 头文件)                 │
│         ↑ 编辑器唯一入口，extern "C" 调用 ↑               │
├─────────────────────────────────────────────────────────┤
│                    引擎内核 (纯 C99)                      │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌──────┐ ┌────────┐  │
│  │  Core  │ │  ECS   │ │Render  │ │Network│ │Plugin  │  │
│  │ 平台层  │ │ 实体系统│ │  RHI   │ │ 网络层 │ │插件系统│  │
│  └────────┘ └────────┘ └────────┘ └──────┘ └────────┘  │
│  ┌────────┐ ┌────────┐ ┌────────┐ ┌──────────────────┐  │
│  │  Math  │ │ Memory │ │  Log   │ │   Lua Runtime    │  │
│  │ 数学库 │ │ 内存池 │ │ 日志   │ │   脚本运行时      │  │
│  └────────┘ └────────┘ └────────┘ └──────────────────┘  │
├─────────────────────────────────────────────────────────┤
│                    平台抽象层 (Platform)                  │
│   Windows / Linux / macOS / iOS / Android               │
└─────────────────────────────────────────────────────────┘
```

### 2.2 编译模式

| 模式 | 包含模块 | 用途 |
|------|----------|------|
| **Client** | 内核 + 渲染 + 网络(客户端) + Lua | 玩家客户端 |
| **Server** | 内核 + 网络(服务端) + Lua（无渲染） | 专用服务器 |
| **Headless** | 内核 + Lua（无渲染/无网络） | 单元测试、CI |
| **Editor** | 内核 + 渲染 + 编辑器 UI | 开发工具 |

### 2.3 目录结构

```
chaos-engine/
├── src_c/                     # 【引擎内核】纯 C99，禁止 C++ 语法
│   ├── core/                  # 平台抽象、数学、内存、线程
│   │   ├── ce_platform.h/c    #   平台抽象层（窗口、输入、文件）
│   │   ├── ce_math.h/c        #   数学库（向量、矩阵、四元数）
│   │   ├── ce_memory.h/c      #   内存池、分配器
│   │   ├── ce_thread.h/c      #   线程池、原子操作
│   │   └── ce_time.h/c        #   高精度计时器
│   ├── ecs/                   # ECS 实体组件系统
│   │   ├── ce_entity.h/c      #   实体管理
│   │   ├── ce_component.h/c   #   组件注册与存储
│   │   ├── ce_system.h/c      #   系统调度管线
│   │   └── ce_query.h/c       #   实体查询/迭代器
│   ├── render/                # 渲染抽象层 (RHI)
│   │   ├── ce_rhi.h           #   RHI 接口定义
│   │   ├── ce_rhi_opengl.c    #   OpenGL 后端
│   │   ├── ce_rhi_vulkan.c    #   Vulkan 后端（后续）
│   │   ├── ce_rhi_metal.c     #   Metal 后端（后续）
│   │   ├── ce_mesh.h/c        #   网格数据
│   │   ├── ce_material.h/c    #   材质系统
│   │   └── ce_camera.h/c      #   摄像机
│   ├── network/               # 网络层
│   │   ├── ce_socket.h/c      #   Socket 封装（TCP/UDP/KCP）
│   │   ├── ce_replication.h/c #   属性复制系统
│   │   ├── ce_lockstep.h/c    #   帧同步（客户端）
│   │   └── ce_statesync.h/c   #   状态同步（服务端）
│   ├── plugin/                # 插件系统
│   │   ├── ce_plugin.h/c      #   插件状态机
│   │   └── ce_plugin_registry.h/c  # 插件注册表
│   ├── script/                # Lua 运行时绑定
│   │   ├── ce_lua.h/c         #   Lua VM 封装
│   │   └── ce_lua_bindings.c  #   C API → Lua 绑定
│   ├── log/                   # 日志系统
│   │   └── ce_log.h/c         #   第八模式日志
│   └── public_api/            # 【对外接口】纯 C 头文件
│       ├── chaos_engine.h     #   引擎主接口
│       ├── ce_types.h         #   基础类型
│       ├── ce_ecs.h           #   ECS 接口
│       ├── ce_plugin.h        #   插件接口
│       └── ce_log.h           #   日志接口
│
├── src_cpp/                   # 【编辑器】纯 C++17
│   ├── ui/                    # Dear ImGui 界面
│   ├── importer/              # 资源导入（FBX/GLTF/PNG）
│   ├── editor_logic/          # 编辑逻辑（Gizmo、撤销重做）
│   └── log_observer/          # 第八模式日志观测面板
│
├── src_lua/                   # Lua 脚本（游戏逻辑、服务端逻辑）
│   ├── server/                #   服务端脚本
│   │   ├── cell_mgr.lua       #   Cell 空间管理
│   │   ├── aoi.lua            #   AOI 动态调整
│   │   └── entity_base.lua    #   服务端实体基类
│   ├── client/                #   客户端脚本
│   │   ├── battle.lua         #   战斗逻辑（帧同步）
│   │   └── ui_binding.lua     #   UI 绑定
│   └── shared/                #   共享逻辑（同构核心）
│       ├── combat.lua         #   战斗计算（客户端+服务端同一份）
│       └── skill_system.lua   #   技能系统
│
├── plugins/                   # 插件目录
├── tests/                     # 测试
│   ├── unit/                  #   单元测试
│   └── integration/           #   集成测试
├── ci/                        # CI 配置
│   └── rules/                 #   静态检查规则
├── docs/                      # 文档
│   └── spec/                  #   设计规格
├── CMakeLists.txt             # 根构建文件
├── .gitignore
└── README.md
```

---

## 3. 模块设计

### 3.1 模块依赖关系（单向无环）

```
                    ┌─────────┐
                    │ Editor  │  (C++17, 仅依赖 public_api)
                    └────┬────┘
                         │ extern "C"
                    ┌────▼────┐
                    │public_api│  (纯 C 头文件)
                    └────┬────┘
                         │
    ┌────────────────────┼────────────────────┐
    │                    │                    │
┌───▼───┐  ┌────────┐  ┌▼──────┐  ┌────────▼──┐
│Script │  │Network │  │Render │  │  Plugin    │
│(Lua)  │  │        │  │(RHI)  │  │            │
└───┬───┘  └───┬────┘  └───┬───┘  └─────┬──────┘
    │          │            │             │
    └──────────┴────────────┴─────────────┘
                      │
               ┌──────▼──────┐
               │     ECS     │
               └──────┬──────┘
                      │
               ┌──────▼──────┐
               │    Core     │  (平台、数学、内存、日志)
               └─────────────┘
```

### 3.2 各模块职责

| 模块 | 职责 | 行数估算 |
|------|------|----------|
| **Core** | 平台抽象、数学库、内存池、线程、时间 | ~3000 |
| **ECS** | 实体管理、组件存储、系统调度、查询 | ~2500 |
| **Render** | RHI 抽象、OpenGL 后端、网格/材质/相机 | ~4000 |
| **Network** | Socket、KCP、复制系统、帧同步、状态同步 | ~3000 |
| **Plugin** | 插件状态机、注册表、生命周期 | ~1000 |
| **Script** | Lua VM 封装、C API 绑定 | ~1500 |
| **Log** | 第八模式日志、环形缓冲、回调 | ~800 |
| **Editor** | Dear ImGui 窗口、资源导入、编辑逻辑 | ~5000 |
| **public_api** | 对外头文件（仅声明） | ~500 |

---

## 4. 客户端/服务器同构方案

### 4.1 核心思想

> 同一份战斗逻辑 Lua 代码，编译一次，两处运行。

```
┌──────────────────────────────────────────────────┐
│              shared/combat.lua                    │
│         （战斗计算、技能判定、伤害公式）             │
│         客户端和服务器加载同一份文件                │
└────────────┬──────────────────┬──────────────────┘
             │                  │
      ┌──────▼──────┐    ┌──────▼──────┐
      │  客户端模式  │    │  服务端模式  │
      │  帧同步      │    │  状态同步    │
      │  表现层驱动  │    │  权威验证    │
      └─────────────┘    └─────────────┘
```

### 4.2 两种网络模式

| 特性 | 帧同步 (Lockstep) | 状态同步 (State Sync) |
|------|-------------------|----------------------|
| **适用场景** | MOBA、RTS、格斗（<10人） | MMO、开放世界（>100人） |
| **运行位置** | 客户端执行，服务器转发 | 服务器执行，客户端插值 |
| **确定性要求** | 极高（浮点/随机数一致） | 低（服务器权威） |
| **带宽** | 低（仅输入指令） | 中高（状态数据） |
| **延迟容忍** | 低（等最慢玩家） | 高（客户端预测+插值） |
| **反外挂** | 弱（客户端计算） | 强（服务器验证） |

### 4.3 同构实现方式

```c
// 引擎编译时通过宏切换模式
#ifdef CE_BUILD_SERVER
    #define CE_NET_MODE CE_NET_STATESYNC
    // 不链接渲染模块
#else
    #define CE_NET_MODE CE_NET_LOCKSTEP
    // 链接渲染模块
#endif
```

Lua 层通过全局变量判断运行环境：
```lua
-- shared/combat.lua
local function calculate_damage(attacker, defender, skill)
    -- 同一份计算逻辑
    local base_damage = skill.power * attacker.atk / defender.def
    
    if CHAOS_IS_SERVER then
        -- 服务端：直接应用伤害，广播结果
        defender.hp = defender.hp - base_damage
        broadcast_damage(attacker.id, defender.id, base_damage)
    else
        -- 客户端：播放表现，等待服务端确认
        play_hit_effect(defender.pos)
        -- 帧同步模式下本地直接计算
        if NET_MODE == "lockstep" then
            defender.hp = defender.hp - base_damage
        end
    end
    
    return base_damage
end
```

### 4.4 KBEngine 风格的空间管理（后续版本）

```
┌─────────────────────────────────────────────┐
│                  Game Space                  │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐     │
│  │ Cell A  │  │ Cell B  │  │ Cell C  │     │
│  │ (0,0)   │  │ (1,0)   │  │ (2,0)   │     │
│  │ 100人   │  │ 80人    │  │ 120人   │     │
│  └─────────┘  └─────────┘  └─────────┘     │
│  ┌─────────┐  ┌─────────┐  ┌─────────┐     │
│  │ Cell D  │  │ Cell E  │  │ Cell F  │     │
│  │ (0,1)   │  │ (1,1)   │  │ (2,1)   │     │
│  └─────────┘  └─────────┘  └─────────┘     │
│                                             │
│  动态 AOI：根据玩家密度自动调整 Cell 大小     │
│  跨 Cell 实体：通过边界传送/镜像实现          │
└─────────────────────────────────────────────┘
```

---

## 5. ECS 架构设计

### 5.1 存储模型：Archetype（原型）

借鉴 Ant Engine / Bevy 的 Archetype 模式：

```
Archetype = 一组特定的组件组合

例如：
  Archetype<Transform, MeshRenderer>        → 静态场景物体
  Archetype<Transform, RigidBody, Collider> → 物理物体
  Archetype<Transform, Health, AI>          → AI 敌人
```

- 相同 Archetype 的实体，组件数据连续存储在内存中
- 遍历时 Cache-Friendly，SIMD 友好
- 添加/移除组件时，实体在 Archetype 间迁移

### 5.2 核心 API

```c
// 实体
CeEntity e = ce_entity_create();

// 组件
Transform* t = ce_entity_add_component(e, COMPONENT_TRANSFORM);
t->x = 10.0f;

// 系统
CE_SYSTEM(move_system, float dt) {
    // 查询所有拥有 Transform + Velocity 的实体
    CE_QUERY_BEGIN(Transform, Velocity);
        transform->x += velocity->vx * dt;
        transform->y += velocity->vy * dt;
    CE_QUERY_END;
}

// 注册
ce_system_register("move", move_system, CE_PHASE_UPDATE, 0);
```

### 5.3 系统管线（Phase）

```
CE_PHASE_PRE_UPDATE    (输入采集)
    ↓
CE_PHASE_UPDATE        (逻辑更新)
    ↓
CE_PHASE_POST_UPDATE   (状态同步)
    ↓
CE_PHASE_RENDER        (渲染提交)
    ↓
CE_PHASE_POST_RENDER   (UI/调试)
```

### 5.4 内置组件（首版）

| 组件 | 字段 | 用途 |
|------|------|------|
| `Transform` | position, rotation, scale | 空间变换 |
| `Velocity` | vx, vy, vz | 速度 |
| `Health` | hp, max_hp | 生命值 |
| `MeshRenderer` | mesh_id, material_id | 网格渲染 |
| `Camera` | fov, near, far | 摄像机 |
| `Light` | type, color, intensity | 光源 |
| `NetworkId` | id, owner | 网络标识 |
| `Script` | lua_ref | Lua 脚本引用 |

---

## 6. 渲染抽象层 (RHI)

### 6.1 设计目标

- 最小化接口数量（首版 < 30 个函数）
- 先实现 OpenGL 3.3 后端（覆盖所有目标平台）
- 接口设计预留 Vulkan/Metal 扩展空间

### 6.2 核心接口

```c
// 设备
CeRhiDevice* rhi_create_device(const CeRhiConfig* config);
void         rhi_destroy_device(CeRhiDevice* device);

// 资源
CeRhiBuffer*   rhi_create_buffer(CeRhiDevice* device, const CeBufferDesc* desc);
CeRhiTexture*  rhi_create_texture(CeRhiDevice* device, const CeTextureDesc* desc);
CeRhiShader*   rhi_create_shader(CeRhiDevice* device, CeShaderStage stage, const char* source);
CeRhiPipeline* rhi_create_pipeline(CeRhiDevice* device, const CePipelineDesc* desc);

// 命令
void rhi_begin_frame(CeRhiDevice* device);
void rhi_bind_pipeline(CeRhiDevice* device, CeRhiPipeline* pipeline);
void rhi_bind_buffer(CeRhiDevice* device, CeRhiBuffer* buffer, uint32_t slot);
void rhi_draw(CeRhiDevice* device, uint32_t vertex_count, uint32_t instance_count);
void rhi_end_frame(CeRhiDevice* device);
void rhi_present(CeRhiDevice* device);
```

### 6.3 后端实现优先级

| 优先级 | 后端 | 覆盖平台 |
|--------|------|----------|
| P0 | OpenGL 3.3 | Windows / Linux / macOS |
| P0 | OpenGL ES 3.0 | iOS / Android |
| P1 | Vulkan 1.2 | Windows / Linux / Android |
| P2 | Metal 2 | macOS / iOS |

---

## 7. 网络层设计

### 7.1 传输层

```
┌─────────────────────────────────────┐
│          ce_socket (抽象层)          │
├─────────────────────────────────────┤
│  TCP  │  UDP  │  KCP  │  WebSocket  │
│ (可靠)│(低延迟)│(可靠UDP)│(Web)     │
└─────────────────────────────────────┘
```

- **TCP**：登录、聊天、物品交易等可靠性优先的场景
- **UDP + KCP**：战斗、移动等低延迟场景
- 首版实现 TCP + UDP，KCP 后续集成

### 7.2 属性复制系统（借鉴 UE Replication）

```c
// 标记需要复制的组件属性
typedef struct CeReplicatedProp {
    uint32_t offset;      // 属性在组件中的偏移
    uint32_t size;        // 属性大小
    CeReplicateCondition cond;  // 复制条件
} CeReplicatedProp;

// 注册复制组件
ce_replication_register_component(COMPONENT_HEALTH, health_props, 2);

// 每帧自动比较脏数据并复制
ce_replication_tick(delta_time);
```

### 7.3 帧同步协议

```
帧同步流程：
1. 服务器收集所有客户端输入 → 广播 Frame N 的输入包
2. 客户端收到后执行 Frame N 的逻辑
3. 客户端缓存 N 帧状态，用于回滚/追帧

确定性保证：
- 浮点数：统一使用 IEEE 754，禁用快速数学优化
- 随机数：使用确定性种子 PRNG
- 排序：实体遍历顺序固定（按 ID 排序）
```

---

## 8. 插件系统

### 8.1 状态机

```
                 ┌──────────┐
                 │ UNLOADED │
                 └────┬─────┘
                      │ load()
                 ┌────▼─────┐
                 │ LOADING  │
                 └────┬─────┘
                      │ 完成
                 ┌────▼─────┐
          ┌──────│  LOADED  │──────┐
          │      └────┬─────┘      │
          │ init()    │            │ unload()
          │      ┌────▼─────┐      │
          │      │ RUNNING  │      │
          │      └────┬─────┘      │
          │ pause()   │  error()   │
          │      ┌────▼─────┐ ┌────▼─────┐
          │      │ PAUSED   │ │  ERROR   │
          │      └──────────┘ └──────────┘
          │                            │
          └────────────────────────────┘
                   unload()
               ┌────▼─────┐
               │UNLOADING │
               └──────────┘
```

### 8.2 插件接口

```c
typedef struct CePluginDesc {
    const char*   name;
    const char*   version;
    CePluginInitFn   init;       // 初始化
    CePluginUpdateFn update;     // 帧更新
    CePluginShutdownFn shutdown; // 关闭
    CeBool is_core;              // 核心插件不可卸载
} CePluginDesc;
```

---

## 9. 脚本层 (Lua)

### 9.1 设计原则

- **双 VM 隔离**：运行时 Lua VM（游戏逻辑）和编辑器 Lua VM（扩展脚本）完全独立
- **同构加载**：shared/ 目录下的 Lua 文件，客户端和服务端加载同一份
- **最小绑定**：只暴露必要的 C API 给 Lua，避免过度绑定

### 9.2 C → Lua 绑定策略

```c
// 通过 ce_lua 模块统一注册
ce_lua_register_function(L, "entity_create",     lua_entity_create);
ce_lua_register_function(L, "entity_add_comp",   lua_entity_add_component);
ce_lua_register_function(L, "entity_get_comp",   lua_entity_get_component);
ce_lua_register_function(L, "log_info",          lua_log_info);
ce_lua_register_function(L, "network_send",      lua_network_send);
```

### 9.3 Skynet 风格的服务化（后续版本）

```lua
-- 每个 Cell 是一个独立的 Lua 服务（Actor）
local cell = skynet.newservice("cell")
skynet.call(cell, "lua", "load_space", space_id)
skynet.call(cell, "lua", "enter_entity", entity_id, x, y, z)
```

---

## 10. 编辑器架构

### 10.1 技术选型

- **GUI**：Dear ImGui（轻量、即时模式、C 友好）
- **窗口**：GLFW（跨平台窗口管理）
- **3D 视口**：引擎自身渲染到 FBO，编辑器作为纹理显示

### 10.2 编辑器功能（MVP）

```
┌─────────────────────────────────────────────┐
│  File  Edit  View  Tools  Help              │
├───────────┬─────────────────┬───────────────┤
│ Hierarchy │                 │  Inspector    │
│           │                 │               │
│  ▶ Scene  │   3D Viewport   │  Entity: 42   │
│   ├ Cube  │   (引擎渲染)     │  Position:    │
│   ├ Light │                 │  [0][0][0]    │
│   └ Cam   │                 │  Rotation:    │
│           │                 │  [0][0][0]    │
│           │                 │               │
├───────────┴─────────────────┴───────────────┤
│  Console (第八模式日志)                       │
│  [INFO] Plugin "render" loaded (2.3ms)      │
│  [INFO] Scene "demo" loaded (45 entities)   │
└─────────────────────────────────────────────┘
```

### 10.3 编辑器与内核的隔离规则

| 允许 | 禁止 |
|------|------|
| ✅ 调用 public_api 函数 | ❌ 直接 include 内核内部头文件 |
| ✅ 读取组件数据（只读） | ❌ 直接修改内核内存 |
| ✅ 通过 edit_component 回调修改 | ❌ 持有内核原生指针长期使用 |
| ✅ 注册日志回调 | ❌ 替换内核函数指针 |
| ✅ 下发编辑指令 | ❌ 注入 C++ 对象到内核 |

---

## 11. 日志与观测系统（第八模式）

### 11.1 日志分级

| 级别 | 用途 | 第八模式下 |
|------|------|-----------|
| TRACE | 函数调用追踪 | ✅ 全部输出 |
| DEBUG | 调试信息 | ✅ 全部输出 |
| INFO | 状态变更 | ✅ 全部输出 |
| WARN | 警告 | ✅ 全部输出 |
| ERROR | 错误 | ✅ 全部输出 |
| FATAL | 致命错误 | ✅ 全部输出 |

### 11.2 四大类日志

```json
// ① 插件状态日志
{"ts": "2026-06-15T15:20:01", "type": "plugin_state",
 "plugin": "render", "from": "LOADING", "to": "LOADED", "elapsed_us": 2300}

// ② 界面变更日志
{"ts": "2026-06-15T15:20:05", "type": "ui_event",
 "window": "MainUI", "action": "create", "size": [1080, 720]}

// ③ 运行行为日志
{"ts": "2026-06-15T15:20:10", "type": "logic_event",
 "plugin": "combat", "fn": "calculate_damage", "result": "ok"}

// ④ 异常日志
{"ts": "2026-06-15T15:21:02", "type": "error",
 "plugin": "render", "msg": "Texture not found: hero.png", "stack": "..."}
```

### 11.3 实现

- 环形缓冲区（默认 4096 条），避免日志刷爆内存
- 编辑器通过 `ce_log_add_callback` 注册消费者
- 第八模式开启时，日志详细度提升到 TRACE 级别
- 日志同时输出到文件（`logs/YYYY-MM-DD/`）和控制台

---

## 12. Git 分支模型与 CI/CD

### 12.1 分支模型（精简 GitFlow）

```
main        ★───────★────────────★  (稳定发布)
             \     /            /
develop  ─────★───★────★─────★───  (日常集成)
               \  /      \   /
feature/xxx ────★         \ /
plugin/yyy  ───────────────★      (独立开发)
```

### 12.2 提交规范

```
[类型](作用域): 简述

类型：feat / fix / refactor / docs / perf / plugin / test
作用域：core / ecs / render / network / plugin / editor / script

示例：
  [feat](ecs): 实现 Archetype 实体存储
  [plugin](render): 完成 OpenGL RHI 后端
  [fix](network): 修复 KCP 重传超时
```

### 12.3 CI 流水线

```
Git Push / MR
    │
    ▼
┌──────────────┐
│ 静态检查      │  clang-format, clang-tidy, 跨目录引用检测
└──────┬───────┘
       │ 通过
       ▼
┌──────────────┐
│ 编译          │  Client / Server / Headless 三种模式
└──────┬───────┘
       │ 通过
       ▼
┌──────────────┐
│ 测试          │  单元测试 + 集成测试
└──────┬───────┘
       │ 通过
       ▼
┌──────────────┐
│ 合并 + 归档   │  自动 MR 合并，打版本标签
└──────────────┘
```

---

## 13. MVP v0.1 范围

### 13.1 目标

> 一个可编译、可运行的最小骨架，验证架构可行性。

### 13.2 包含内容

| 模块 | MVP 内容 | 不包含 |
|------|----------|--------|
| **Core** | 平台层（窗口+输入）、数学库、内存池、日志 | 完整线程池 |
| **ECS** | 实体创建/销毁、组件注册、Archetype 存储、System 调度 | 完整查询优化 |
| **Render** | RHI 接口定义 + OpenGL 3.3 后端（画三角形） | Vulkan/Metal、PBR |
| **Network** | Socket 封装（TCP/UDP）、基础消息收发 | KCP、帧同步、复制系统 |
| **Plugin** | 状态机 + 注册表 | 动态加载(.so/.dll) |
| **Script** | Lua VM 嵌入、基础 C API 绑定 | 完整绑定 |
| **Editor** | Dear ImGui 窗口 + 实体列表面板 | 3D 视口、Gizmo |
| **Build** | CMake 构建、三种编译模式 | CI 脚本 |

### 13.3 验收标准

- [ ] `cmake --build .` 在 Linux 下成功编译（Client/Server/Headless 三种模式）
- [ ] Headless 模式下运行单元测试，ECS 基本操作通过
- [ ] Client 模式下打开窗口，显示 Dear ImGui 界面
- [ ] Client 模式下渲染一个三角形
- [ ] Server 模式下启动，监听端口，接受客户端连接
- [ ] Lua 脚本可以调用 C API 创建实体

---

## 14. 开发路线图

```
v0.1 (MVP) ───── 2-4 周
  ├─ 项目骨架 + CMake
  ├─ Core 模块
  ├─ ECS 模块
  ├─ Render RHI + OpenGL 三角形
  ├─ Network 基础 Socket
  ├─ Lua 嵌入
  └─ Editor 骨架

v0.2 ─────────── 4-6 周
  ├─ 完整 RHI（纹理、Shader、材质）
  ├─ 3D 网格加载（GLTF）
  ├─ 基础光照
  ├─ 帧同步协议
  └─ Editor 3D 视口

v0.3 ─────────── 6-8 周
  ├─ 属性复制系统
  ├─ KCP 集成
  ├─ Cell 空间管理
  ├─ AOI 动态调整
  └─ 插件动态加载

v0.5 ─────────── 8-12 周
  ├─ Vulkan 后端
  ├─ 移动端适配
  ├─ 物理集成（Bullet）
  └─ 完整编辑器
```

---

## 15. 附录：参考引擎分析

### 15.1 Skynet

| 借鉴 | 不借鉴 |
|------|--------|
| Actor 并发模型 | 纯 Lua 服务（我们用 C 内核） |
| 消息驱动的服务架构 | 单进程模型（我们需要多进程） |
| 轻量级设计哲学 | 缺乏 3D 渲染 |

### 15.2 KBEngine

| 借鉴 | 不借鉴 |
|------|--------|
| Cell 空间划分 | Python 脚本（我们用 Lua） |
| 动态 AOI 调整 | 重量级架构 |
| Base/Cell 双层实体 | 固定 MMO 定位 |
| 多人同屏优化 | 缺乏客户端渲染 |

### 15.3 Ant Engine

| 借鉴 | 不借鉴 |
|------|--------|
| 纯 ECS 架构 | Rust 实现（我们用 C） |
| Archetype 存储 | 缺乏网络层 |
| System 调度管线 | 缺乏编辑器 |

### 15.4 Unreal Engine

| 借鉴 | 不借鉴 |
|------|--------|
| Actor/Component 概念模型 | 重量级架构 |
| 属性复制系统 | 蓝图可视化编程 |
| 网络优先级/相关性 | C++ 模板重度使用 |
| Gameplay 框架设计 | 编辑器复杂度 |

---

> **下一步：** 请审阅本 Spec，确认后按模块顺序逐步实现 v0.1。
