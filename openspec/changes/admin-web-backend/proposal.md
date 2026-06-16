# admin-web-backend

Web 管理后台：独立进程 + Lapis HTTP/WebSocket + Unix Socket IPC + 仪表盘

---

## Why

ChaosEngine v0.1 作为一个游戏服务器引擎，运行时完全是一个"黑盒"——开发者无法直观地观察引擎内部状态。ECS 实体数、AOI 视野变化、Cell 网格负载、网络流量、内存使用、日志输出等关键指标全部不可见。这导致：

1. **调试困难**：出现性能问题时，只能依赖 printf 日志或 gdb 断点，无法快速定位瓶颈。
2. **运维盲区**：服务器上线后，运维人员无法监控实时状态，无法及时发现过载 Cell、内存泄漏、网络异常等问题。
3. **开发体验差**：每次需要查看引擎状态都要修改代码加日志，迭代效率低。

v0.2 需要提供一个轻量级的 Web 管理后台，让开发者通过浏览器即可实时查看引擎全部子系统的运行状态。

## What Changes

### 新增模块

1. **IPC 服务端（C，嵌入 chaos_server）**：`src_c/admin_ipc/` — Unix Domain Socket + JSON-RPC 2.0 协议，仅在 `--admin` 参数下启动。暴露 stats/aoi/cell/network/memory/cpu/log/render/system/health 共 10 个 RPC 方法。

2. **Web 管理后台（Lua，独立进程）**：`src_lua/admin/` — 基于 Lapis 框架的 HTTP 服务，监听 :9090 端口。通过 Unix Socket 与 chaos_server 通信，提供 REST API 和内嵌仪表盘 HTML。

3. **前端仪表盘**：单文件 HTML（~50KB），内嵌 CSS/JS，深色主题。包含顶部指标卡、AOI 散点图、Cell 热力图、网络流量面板、内存面板、CPU 面板、实时日志面板、渲染统计面板、系统信息面板。

4. **一键启动脚本**：`scripts/start_with_admin.sh` — 自动构建、启动 server + admin、清理旧进程。

### 架构决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 进程模型 | 独立 chaos_admin 进程 | 崩溃隔离，不影响 server |
| 通信方式 | Unix Domain Socket | 零网络开销，本机通信延迟 < 1ms |
| 协议 | JSON-RPC 2.0（换行分隔 JSON） | 简单、可调试、无需 protobuf 等依赖 |
| Web 框架 | Lapis (Lua) | 与引擎 Lua 生态一致，轻量 |
| 前端 | 单文件 HTML | 零构建、零依赖、浏览器直接打开 |
| 启动控制 | `--admin` 开关 | 默认关闭，零开销 |

## Impact

### 受影响的代码

| 文件/目录 | 影响类型 | 说明 |
|-----------|----------|------|
| `src_c/admin_ipc/` | **新增** | IPC 服务端（ce_admin_ipc.h/c + CMakeLists.txt） |
| `src_c/runtime/ce_server_main.c` | 修改 | 集成 `--admin` 参数和 IPC 启动 |
| `src_lua/admin/` | **新增** | Lapis Web 服务（6 个文件） |
| `scripts/start_with_admin.sh` | **新增** | 一键启动脚本 |

### 不受影响的模块

- ECS、AOI、Cell、Network、Memory、Log、Render 核心逻辑不变（仅通过 IPC 暴露已有 API）
- 不带 `--admin` 时 server 行为完全不变
- 现有单元测试全部通过

### MVP 范围

**Phase 1（已完成）**：IPC 服务端 — Unix Socket + JSON-RPC 2.0，10 个 RPC 方法全部实现。

**Phase 2（已完成）**：Lapis Web 服务 — HTTP REST API（10 个端点）+ 内嵌仪表盘 HTML。

**Phase 3（已完成）**：前端仪表盘 — 单文件 HTML，指标卡/散点图/热力图/日志面板/系统信息。

**Phase 4（已完成）**：全子系统数据对接 — ECS/AOI/Cell/Network/Memory/CPU/Log/Render/System/Health。

**Phase 5（已完成）**：一键启动脚本 + 进程清理 + 健康检查。

**延后到后续版本**：
- WebSocket 实时推送（当前使用 HTTP 轮询）
- 用户认证（内网部署，暂不需要）
- 数据库持久化（仅内存快照）
- 移动端适配
- 远程管理（仅 localhost）
