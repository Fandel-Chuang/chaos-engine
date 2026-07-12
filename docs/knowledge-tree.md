# ChaosEngine 全量文件知识树

> 生成时间: 2026-07-04 | 仓库: gitee.com/zhong-fangdao/chaos-engine | 版本: v0.1 (MVP) | MIT

---

## 1. 仓库总览

3D 多人游戏引擎，客户端/服务端同构（客户端帧同步 + 服务端状态同步）。

**参考引擎:** Skynet (Actor/Lua), KBEngine (Cell/AOI), Ant Engine (ECS/Archetype), Unreal (Actor/Component, replication)

**技术栈:** 纯 C (C99) 内核 + C++17 (Dear ImGui) 编辑器 + Lua 5.4 脚本(延后) + FlatBuffers 序列化 + Vulkan RHI + io_uring/eBPF

---

## 2. 三层架构

```
编辑器层 (src_cpp/, C++17)      ← Dear ImGui UI + 资源导入 + 场景编辑
        ↓
public_api (src_c/public_api/)  ← 纯 C 头文件, 编辑器与内核唯一接口
        ↓
内核层 (src_c/, C99)            ← 16 个子模块
        ↓
平台抽象 (Win/Linux/macOS/iOS/Android)
```

**CI 隔离规则（强制）:**
- `src_c/` 禁止出现 C++ 关键字 (`check_cpp_in_c.py`)
- `src_c/` 不可 `#include` `src_cpp/` 头文件
- `src_cpp/` 只能经 `public_api/` 引用内核 (`check_cross_ref.py`)

---

## 3. 根目录结构

| 目录/文件 | 说明 |
|---|---|
| `src_c/` | C99 内核, 16 个子模块 |
| `src_cpp/` | C++17 编辑器 |
| `src_lua/` | Lua 脚本层 (空, 延后至 v0.3) |
| `tests/` | 单元测试 + 基准测试 |
| `bench_results/` | 基准结果 (4 个 JSON) |
| `schemas/` | FlatBuffers schema (4 个 .fbs) |
| `scripts/` | 24 个自动化脚本 |
| `ci/` | 构建脚本 + 隔离检查规则 |
| `docs/` | 设计文档与规格说明 (6 个 .md) |
| `openspec/` | OpenSpec 规范体系 (51 文件) |
| `third_party/` | 第三方依赖 (Lua/Vulkan/X11) |
| `plugins/` | 运行时插件目录 (空) |
| `llm/` | LLM 辅助脚本 |
| `CMakeLists.txt` | 根 CMake 配置 |
| `.gitee-ci.yml` | CI 配置 (GitHub Actions 语法, 4 job) |

---

## 4. 内核模块 (src_c/, 16 个)

| 模块 | 职责 |
|---|---|
| **core** | 内存分配 (ce_memory), 数学库 (ce_math: Vec2/3/4, Mat4, Quat) |
| **ecs** | 实体组件系统, Archetype 模式 |
| **render** | Vulkan RHI 渲染层 (ce_rhi, ce_rhi_vulkan) |
| **network** | 网络层, io_uring 事件驱动 + POSIX 非阻塞轮询 fallback |
| **server** | 游戏服务器: AOI (ce_aoi), Cell (ce_cell), 实体管理 |
| **sync** | 状态同步 |
| **replication** | 属性复制 (FlatBuffers) |
| **runtime** | 运行时: 客户端网络 (ce_client_network) |
| **dbproxy** | 数据库代理 (主备模式) |
| **save** | 存档系统 |
| **plugin** | 插件系统 (状态机 + 注册表 + 依赖管理) |
| **script** | Lua 脚本绑定 (延后至 v0.3) |
| **admin_ipc** | 管理后台 IPC 通信 |
| **ebpf** | eBPF 可观测性 |
| **log** | 日志系统 (6 级: TRACE→FATAL) |
| **public_api** | 公共接口头文件 (5 个) |

### public_api 头文件 (5 个)

| 头文件 | 内容 |
|---|---|
| `ce_types.h` | CeResult, CeLogLevel, CeEngineState, 数学类型, 引擎配置, 资源类型, 内存分配器虚表 |
| `ce_ecs.h` | ECS 公共接口 |
| `ce_log.h` | 日志系统公共接口 |
| `ce_plugin.h` | 插件公共接口: CePluginInfo, CePluginDesc, 7 个管理函数 |
| `chaos_engine.h` | 引擎总入口 |

---

## 5. 编辑器 (src_cpp/)

`editor_main.cpp` (入口) + 4 个子目录: `editor_logic/`, `importer/`, `log_observer/`, `ui/`

---

## 6. 构建系统

**CMake 3.20+**, C11 / C++17

| 选项 | 默认 | 说明 |
|---|---|---|
| `CHAOS_BUILD_EDITOR` | ON | 构建编辑器 |
| `CHAOS_BUILD_TESTS` | OFF | 构建测试 |
| `CHAOS_BUILD_SAMPLES` | OFF | 构建示例 |
| `CHAOS_USE_DOUBLE_PROCESS` | OFF | 双进程模式 |

**输出:** `./bin/chaos_editor`, `./bin/chaos_headless`

**CI (`.gitee-ci.yml`, GitHub Actions 语法):** 触发于 PR/push 到 master/develop

| Job | 说明 |
|---|---|
| build-and-test | Debug 构建 + ctest + 冒烟测试 + 客户端同步验证 |
| release-build | Release 编译检查 |
| lua-lint | Lua 语法检查 |
| memcheck | Valgrind (--leak-check=full) 检查 7 个测试二进制 |

**CI 隔离检查:**

| 脚本 | 规则 |
|---|---|
| `ci/rules/check_cpp_in_c.py` | 扫描 src_c/ 检测 C++ 关键字 (class, namespace, template...), 跳过 extern "C" |
| `ci/rules/check_cross_ref.py` | src_c 不可引用 src_cpp; src_cpp 只能经 public_api 引用 src_c |

---

## 7. 设计约定

### 7.1 命名约定（从代码归纳，spec 无显式章节）

| 层级 | 规则 | 示例 |
|---|---|---|
| C 函数 | `ce_<模块>_<动词>` | `ce_aoi_enter`, `ce_cell_split` |
| C 类型 | `Ce<PascalCase>` | `CeResult`, `CeServerEntityId`, `CeVec3` |
| C 宏/常量 | `CE_<大写>` | `CE_OK`, `CE_AOI_LEAVE`, `CE_LOG_TRACE` |
| 文件 | `ce_<模块>.h/.c` | `ce_types.h`, `ce_aoi.c` |

### 7.2 错误处理（定义于 ce_types.h）

```c
typedef int32_t CeResult;
#define CE_OK               0
#define CE_ERR             -1
#define CE_ERR_VALIDATION  -2   /* 值域校验失败 */
```

全项目仅 3 个错误码，无专用 error.h，各模块注释统一 `@return CE_OK 成功, CE_ERR 失败`

### 7.3 日志级别与引擎状态

日志: TRACE(0) → DEBUG(1) → INFO(2) → WARN(3) → ERROR(4) → FATAL(5)

引擎状态: UNINITIALIZED → INITIALIZING → RUNNING ⇄ PAUSED → SHUTTING_DOWN, 异常时 ERROR

### 7.4 缺失项

无 `.clang-format`、无 `CONTRIBUTING.md`、无独立编码风格文档

---

## 8. 插件系统

| 文件 | 说明 |
|---|---|
| `src_c/public_api/ce_plugin.h` | 公共 API: CePluginInfo, CePluginDesc, 7 个管理函数 |
| `src_c/plugin/ce_plugin.c` | 实现 |
| `src_c/plugin/ce_plugin_internal.h` | 内部头文件 |
| `openspec/specs/plugin/spec.md` | 规范 (4 项能力: 状态机/描述符/注册表/依赖管理) |
| `plugins/` | 运行时目录 (空) |

**spec vs 实现差异:** 公共头缺少 `unregister()`, `pause()`, `resume()`, `init()`, 依赖声明字段


---

## 9. OpenSpec 规范体系

### 基线规范 (7 个, 均已实现)
core, ecs, editor, network, plugin, render, script

### 变更提案 (6 个)

| 提案 | 状态 | 说明 |
|---|---|---|
| add-game-cluster-save | 规划中, tasks 全完成 | Game 实时备份 + DBProxy 主备 + 存档 |
| add-gateway | 规划中, tasks 未开始 | TCP/KCP/WebSocket 多协议接入层 |
| add-router-cluster | 规划中, 部分完成 | 服务发现 + 消息路由 + 微服务 |
| admin-web-backend | 已实现 | Web 管理后台 Lapis HTTP/WebSocket + IPC |
| io-uring-ebpf | 部分实现 | io_uring 异步 I/O + eBPF 可观测性 |
| render-vulkan-complete | 草案, tasks 全完成 | Vulkan RHI 完整化 + Server AOI/Cell |

### 网络端口

| 端口 | 用途 |
|---|---|
| 7777 | 客户端直连 |
| 9000 | Gateway TCP |
| 9001 | KCP / DBProxy 同步 |
| 9002 | WebSocket |
| 9003 | DBProxy 备份/数据库 |

---

## 10. 第三方依赖 (third_party/)

| 目录 | 内容 |
|---|---|
| `lua/` | Lua 5.4 头文件 + liblua5.4.so (→ 系统) |
| `vulkan/` | Vulkan SDK 头文件 (vulkan_core.h 1.26MB) + vk_video 编解码 + libvulkan.so.1 |
| `x11/` | X11 运行时 (~38 头文件) + libX11.so |
| `x11_dev/` | libx11-dev 解包 (libX11.a 2.24MB) |
| `xproto_dev/` | x11proto-dev 解包 (~70 proto 头 + 31 .pc) |

无 README/CMakeLists.txt，集成在根 CMakeLists.txt 中

---

## 11. FlatBuffers Schema (schemas/)

| 文件 | 说明 |
|---|---|
| `chaos_common.fbs` | namespace ChaosEngine; struct Vec2/Vec3/Vec4 |
| `chaos_replication.fbs` | union FieldValue (Int8-64, Uint8-64, Float, Double, Bool, String, Vec2/3/4, Quat, Blob) |
| `chaos_rpc.fbs` | table RpcCall { method, params, reliability, call_id }, table RpcResponse |
| `CMakeLists.txt` | flatc 编译 .fbs → C 头文件 |

---

## 12. 自动化脚本 (scripts/, 24 个)

| 类别 | 脚本 |
|---|---|
| **构建测试** | `build_and_test.sh`, `valgrind.supp` |
| **集群生命周期** | `start_cluster.sh` (--game/--gateway/--router/--dbproxy/--admin/--all), `stop_cluster.sh`, `status.sh` (--json/--watch), `start_with_admin.sh` |
| **组件启动** | `start_client.sh` (--tcp/--ws/--headless/--stress), `start_dbproxy.sh` (primary/backup), `start_gateway.sh`, `install-admin-deps.sh`, `verify-admin-deps.sh` |
| **测试** | `test_client.sh`, `test_dbproxy.sh` (9003), `test_gateway_stress.sh`, `test_gateway_tcp.sh` (9000), `test_gateway_ws.sh` (9002), `test_save.sh`, `test_sync.sh`, `joint_client_test.sh` (→7777), `verify_client_sync.sh` |
| **基准** | `bench_async_io.py` (P50/P90/P99, QPS), `bench_async_io.sh` (uring vs posix) |
| **压测** | `stress_client.py` (TCP Echo 7777), `stress_client_v2.py` (大包/小包/断连重连) |

---

## 13. 测试 (tests/)

`CMakeLists.txt` (14535B) + `test_client_network.c` + 子目录: `unit/`, `bench/`, `lua/`

Valgrind 检查: test_math, test_memory, test_ecs, test_aoi, test_cell, test_network, test_net_base

---

## 14. 基准结果 (bench_results/)

4 个 JSON: `posix_100`, `posix_1000`, `uring_100`, `uring_1000`

---

## 15. 文档 (docs/spec/, 6 个)

| 文件 | 大小 |
|---|---|
| `chaos-engine-spec-v0.1.md` | 31,704B |
| `chaos-engine-spec-v0.2.md` | 31,070B |
| `chaos-engine-admin-web-spec-v0.1.md` | 14,600B |
| `chaos-engine-admin-web-spec-v0.2.md` | 36,262B |
| `chaos-engine-io_uring-ebpf-spec-v0.1.md` | 22,623B |
| `chaos-engine-replication-spec-v0.1.md` | 38,225B |

---

## 16. 其他

- `llm/pull_llm.sh` — LLM 模型拉取脚本
- `src_lua/` — 空, 延后至 v0.3
- `plugins/` — 空, 待填充
