# Editor — 编辑器（Dear ImGui、四大面板、隔离规则）

> 来源: chaos-engine-spec-v0.1.md | 状态: 已实现

## Requirements

Editor 模块是 ChaosEngine 的编辑器层，基于 Dear ImGui 构建即时模式 GUI，提供场景编辑、实体管理和日志观测功能。Editor 模块使用 C++17 编写，仅通过 public_api 访问引擎内核，严格遵守分层隔离规则。

---

### Requirement: Dear ImGui 编辑器界面

编辑器 SHALL 基于 Dear ImGui 构建即时模式 GUI 界面。

技术选型 MUST 为：
- **GUI**：Dear ImGui（轻量、即时模式、C 友好）
- **窗口管理**：GLFW（跨平台窗口管理）
- **3D 视口**：引擎自身渲染到 FBO，编辑器作为纹理显示

编辑器 MUST 在 Client 模式下作为独立可执行程序编译和运行。

#### Scenario: Dear ImGui 窗口创建

- **WHEN** 启动编辑器可执行程序
- **THEN** 显示 Dear ImGui 主窗口，包含菜单栏和可停靠面板
- **AND** 窗口可调整大小、移动、最小化

#### Scenario: 3D 视口嵌入

- **WHEN** 编辑器运行并加载场景
- **THEN** 3D 视口面板显示引擎渲染的画面（通过 FBO 纹理）
- **AND** 视口支持鼠标旋转/缩放/平移操作

---

### Requirement: 四大面板布局

编辑器 SHALL 提供四大核心面板：

1. **Hierarchy（层级面板）**：显示场景实体树形结构，支持选择、展开/折叠
2. **3D Viewport（3D 视口）**：引擎实时渲染的 3D 场景视图
3. **Inspector（属性面板）**：显示选中实体的组件和属性，支持编辑
4. **Console（控制台面板）**：显示第八模式结构化日志，支持过滤和搜索

面板 MUST 支持停靠（Docking）布局，用户可自由调整面板位置和大小。

#### Scenario: Hierarchy 实体选择

- **WHEN** 在 Hierarchy 面板中点击实体 "Cube"
- **THEN** 该实体在 3D 视口中高亮显示
- **AND** Inspector 面板显示该实体的所有组件和属性

#### Scenario: Inspector 属性编辑

- **WHEN** 在 Inspector 面板中修改选中实体的 Position 属性
- **THEN** 3D 视口中的实体位置实时更新
- **AND** 修改通过 public_api 的编辑回调安全写入内核

#### Scenario: Console 日志过滤

- **WHEN** 在 Console 面板中设置日志级别过滤为 "ERROR"
- **THEN** 仅显示 ERROR 和 FATAL 级别的日志
- **AND** 过滤实时生效，不影响日志的底层收集

---

### Requirement: 编辑器与内核隔离规则

编辑器 MUST 严格遵守与引擎内核的隔离规则，仅通过 public_api 访问内核功能。

| 允许 | 禁止 |
|------|------|
| ✅ 调用 public_api 函数 | ❌ 直接 include 内核内部头文件 |
| ✅ 读取组件数据（只读） | ❌ 直接修改内核内存 |
| ✅ 通过 edit_component 回调修改 | ❌ 持有内核原生指针长期使用 |
| ✅ 注册日志回调 | ❌ 替换内核函数指针 |
| ✅ 下发编辑指令 | ❌ 注入 C++ 对象到内核 |

#### Scenario: 隔离规则静态检查

- **WHEN** 对编辑器代码执行跨目录引用检测
- **THEN** 编辑器代码中不存在对 `src_c/` 内部头文件（非 public_api）的 include
- **AND** 所有内核交互通过 `extern "C"` 声明的 public_api 函数完成

#### Scenario: 安全属性修改

- **WHEN** 编辑器通过 `edit_component` 回调修改实体组件属性
- **THEN** 修改操作经过内核验证后安全写入
- **AND** 非法修改（如越界值）被内核拒绝并返回错误

---

### Requirement: 编辑器 MVP 功能范围

v0.1 MVP 编辑器 MUST 包含以下功能：
- Dear ImGui 主窗口和菜单栏
- Hierarchy 面板（显示实体列表）
- Console 面板（显示第八模式日志）
- 基础的实体选择和高亮

v0.1 MVP 编辑器 MUST NOT 包含：
- 完整的 3D 视口（后续版本）
- Gizmo 变换工具（后续版本）
- 资源导入器（后续版本）
- 撤销/重做系统（后续版本）

#### Scenario: MVP 编辑器启动

- **WHEN** 编译 Editor 模式并运行
- **THEN** 显示 Dear ImGui 窗口，包含 Hierarchy 和 Console 面板
- **AND** Console 面板实时显示引擎日志输出

#### Scenario: MVP 实体列表

- **WHEN** 引擎中创建若干实体后刷新 Hierarchy 面板
- **THEN** Hierarchy 面板显示所有实体及其名称
- **AND** 点击实体可在面板中高亮选中
