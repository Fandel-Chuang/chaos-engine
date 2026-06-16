# Core — 引擎核心（愿景、架构、平台、数学、内存、日志、构建、CI）

> 来源: chaos-engine-spec-v0.1.md | 状态: 已实现

## Requirements

Core 模块是 ChaosEngine 的最底层模块，提供平台抽象、数学库、内存管理、日志系统和构建基础设施。所有上层模块（ECS、Render、Network、Plugin、Script）均依赖 Core 模块，Core 不依赖任何其他引擎模块。

---

### Requirement: 引擎愿景与定位

ChaosEngine SHALL 定位为面向 3D 多人游戏的、客户端/服务器同构的、纯 C 内核 + Lua 脚本的轻量级开源游戏引擎。

引擎 MUST 遵循以下核心设计原则：
- **极度简洁**：内核代码量最小化，每个模块只做一件事
- **高效执行**：纯 C 内核，无虚函数开销，Cache-Friendly 数据布局
- **同构逻辑**：同一份战斗/游戏逻辑代码，客户端跑帧同步，服务端跑状态同步
- **严格分层**：C 内核 → public_api → C++ 编辑器，三层隔离，单向依赖
- **全平台**：支持 Windows / Linux / macOS / iOS / Android
- **AI 友好**：代码结构、命名、日志全部面向 AI 可解析设计

引擎 MUST 明确以下边界（不做什么）：
- 不做可视化编程（蓝图）
- 不做完整的物理引擎（集成 Bullet/PhysX）
- 不做资源商店/市场
- 不做 UE/Unity 级别的编辑器
- 首版不做 PBR 完整管线

#### Scenario: 引擎定位验证

- **WHEN** 开发者阅读引擎文档和代码结构
- **THEN** 能清晰识别引擎的定位、目标用户群和明确的能力边界
- **AND** 代码结构体现三层隔离（C 内核 / public_api / C++ 编辑器）

---

### Requirement: 分层架构

引擎 SHALL 采用严格的三层架构，层间单向依赖，禁止反向调用。

架构层次从上到下为：
1. **编辑器层（C++17）**：Dear ImGui UI、资源导入、编辑逻辑、日志观测面板
2. **public_api（纯 C 头文件）**：编辑器唯一入口，通过 `extern "C"` 调用
3. **引擎内核（纯 C99）**：Core、ECS、Render、Network、Plugin、Math、Memory、Log、Lua Runtime
4. **平台抽象层（Platform）**：Windows / Linux / macOS / iOS / Android

编辑器 MUST 仅通过 public_api 访问内核，禁止直接 include 内核内部头文件。

#### Scenario: 分层隔离验证

- **WHEN** 对编辑器代码进行静态检查
- **THEN** 编辑器代码中不存在对内核内部头文件（非 public_api）的直接 include
- **AND** 所有内核调用均通过 `extern "C"` 接口

---

### Requirement: 编译模式

引擎 SHALL 支持四种编译模式，通过 CMake 选项切换：

| 模式 | 包含模块 | 用途 |
|------|----------|------|
| Client | 内核 + 渲染 + 网络(客户端) + Lua | 玩家客户端 |
| Server | 内核 + 网络(服务端) + Lua（无渲染） | 专用服务器 |
| Headless | 内核 + Lua（无渲染/无网络） | 单元测试、CI |
| Editor | 内核 + 渲染 + 编辑器 UI | 开发工具 |

编译模式 MUST 通过编译宏（如 `CE_BUILD_SERVER`）控制模块的条件编译和链接。

#### Scenario: 编译模式切换

- **WHEN** 使用 `cmake -DCE_BUILD_MODE=Server ..` 配置构建
- **THEN** 编译产物不包含渲染模块代码
- **AND** 网络模块以状态同步模式编译

#### Scenario: Headless 模式测试

- **WHEN** 使用 Headless 模式编译并运行单元测试
- **THEN** 所有不依赖渲染和网络的单元测试正常执行并通过

---

### Requirement: 平台抽象层

Core 模块 SHALL 提供统一的平台抽象层，封装以下平台相关功能：
- 窗口创建与管理
- 输入事件（键盘、鼠标、触摸）
- 文件系统访问
- 线程与原子操作
- 高精度计时器

所有平台相关代码 MUST 集中在 `ce_platform.h/c` 中，通过条件编译实现跨平台支持。

#### Scenario: 跨平台窗口创建

- **WHEN** 在 Windows / Linux / macOS 上分别调用窗口创建 API
- **THEN** 使用相同的 API 签名成功创建本地窗口
- **AND** 平台差异对上层模块完全透明

---

### Requirement: 数学库

Core 模块 SHALL 提供完整的 3D 数学库，包含以下类型和运算：
- 向量（Vector2/3/4）：加减乘除、点积、叉积、归一化、插值
- 矩阵（Matrix3x3/4x4）：乘法、转置、逆矩阵、透视/正交投影
- 四元数（Quaternion）：旋转、插值（Slerp）、与矩阵互转

数学库 MUST 使用确定性浮点运算（IEEE 754），禁用快速数学优化，以保证帧同步的确定性。

#### Scenario: 确定性浮点运算

- **WHEN** 在不同平台（x86/ARM）上执行相同的数学运算序列
- **THEN** 浮点运算结果在 IEEE 754 精度范围内完全一致
- **AND** 不因编译器优化选项不同而产生差异

---

### Requirement: 内存管理

Core 模块 SHALL 提供内存池和分配器，支持以下功能：
- 固定大小内存池（用于组件等高频分配的小对象）
- 线性分配器（用于帧内临时数据）
- 对齐分配（支持 SIMD 对齐要求）

内存分配器 MUST 提供调试模式下的内存泄漏检测和越界检查。

#### Scenario: 组件内存池分配

- **WHEN** 高频创建和销毁 ECS 组件
- **THEN** 内存池分配耗时显著低于通用 malloc/free
- **AND** 内存碎片率保持在可接受范围内

---

### Requirement: 日志系统（第八模式）

引擎 SHALL 实现"第八模式"日志系统，支持以下功能：
- 六级日志分级：TRACE、DEBUG、INFO、WARN、ERROR、FATAL
- 四大类日志：插件状态日志、界面变更日志、运行行为日志、异常日志
- 环形缓冲区存储（默认 4096 条），避免日志刷爆内存
- 日志回调机制（`ce_log_add_callback`），编辑器可注册消费者
- 日志同时输出到文件（`logs/YYYY-MM-DD/`）和控制台

第八模式开启时，日志详细度 MUST 提升到 TRACE 级别。

日志格式 MUST 采用结构化 JSON，便于 AI 和工具解析。

#### Scenario: 第八模式日志输出

- **WHEN** 引擎以第八模式运行并执行插件加载
- **THEN** 日志输出包含结构化的插件状态变更记录（JSON 格式）
- **AND** 记录包含时间戳、插件名、状态转换和耗时

#### Scenario: 环形缓冲区溢出保护

- **WHEN** 日志写入速度超过消费速度，缓冲区达到上限
- **THEN** 最旧的日志条目被覆盖，系统不崩溃
- **AND** 记录缓冲区溢出警告

---

### Requirement: 构建系统

引擎 SHALL 使用 CMake 作为构建系统，根 `CMakeLists.txt` 支持：
- 四种编译模式（Client / Server / Headless / Editor）的条件编译
- 第三方依赖管理
- 跨平台编译（Windows / Linux / macOS / iOS / Android）
- 单元测试和集成测试目标

构建系统 MUST 在 Linux 下执行 `cmake --build .` 即可完成全量编译。

#### Scenario: 一键构建

- **WHEN** 在 Linux 环境下执行 `cmake -B build && cmake --build build`
- **THEN** 所有编译模式的目标产物成功生成
- **AND** 无编译错误和警告

---

### Requirement: CI/CD 流水线

项目 SHALL 配置 CI 流水线，包含以下阶段：
1. 静态检查：clang-format、clang-tidy、跨目录引用检测
2. 编译：Client / Server / Headless 三种模式
3. 测试：单元测试 + 集成测试
4. 合并 + 归档：自动 MR 合并，打版本标签

提交 MUST 遵循规范格式：`[类型](作用域): 简述`，类型包括 feat / fix / refactor / docs / perf / plugin / test，作用域包括 core / ecs / render / network / plugin / editor / script。

#### Scenario: MR 触发 CI

- **WHEN** 开发者提交 MR 到 develop 分支
- **THEN** CI 自动执行静态检查、三种模式编译和全量测试
- **AND** 所有阶段通过后才允许合并
