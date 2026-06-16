# Render — 渲染层（RHI 接口、多后端、资源管理、MVP 验收）

> 来源: chaos-engine-spec-v0.1.md | 状态: 已实现

## Requirements

Render 模块是 ChaosEngine 的渲染抽象层（RHI），提供统一的渲染接口，支持多后端切换。首版实现 OpenGL 3.3 后端，接口设计预留 Vulkan/Metal 扩展空间。Render 模块依赖 Core 和 ECS 模块。

---

### Requirement: RHI 接口最小化

RHI（Render Hardware Interface）SHALL 提供最小化的渲染接口集合，首版接口数量 MUST 不超过 30 个函数。

核心接口 MUST 覆盖以下类别：
- **设备管理**：设备创建/销毁
- **资源管理**：Buffer、Texture、Shader、Pipeline 的创建/销毁
- **命令提交**：帧开始/结束、管线绑定、Buffer 绑定、绘制调用、呈现

接口设计 MUST 与具体图形 API 解耦，通过 `CeRhiDevice` 等不透明句柄隐藏后端实现细节。

#### Scenario: 接口数量约束

- **WHEN** 统计 RHI 头文件中声明的公开函数数量
- **THEN** 函数数量不超过 30 个
- **AND** 每个函数有明确的单一职责

#### Scenario: 后端透明切换

- **WHEN** 应用层使用 RHI 接口进行渲染
- **THEN** 切换 OpenGL 后端到 Vulkan 后端时，应用层代码无需修改
- **AND** 仅需在设备创建时指定后端类型

---

### Requirement: 多后端支持

RHI SHALL 支持多图形后端，按以下优先级实现：

| 优先级 | 后端 | 覆盖平台 |
|--------|------|----------|
| P0 | OpenGL 3.3 | Windows / Linux / macOS |
| P0 | OpenGL ES 3.0 | iOS / Android |
| P1 | Vulkan 1.2 | Windows / Linux / Android |
| P2 | Metal 2 | macOS / iOS |

首版（v0.1）MUST 完成 OpenGL 3.3 后端的完整实现。后端选择 MUST 在设备创建时通过配置指定。

#### Scenario: OpenGL 后端渲染三角形

- **WHEN** 使用 OpenGL 3.3 后端创建渲染设备并提交三角形绘制命令
- **THEN** 窗口显示一个正确着色的三角形
- **AND** 帧率稳定在目标刷新率

#### Scenario: 后端配置切换

- **WHEN** 在 `CeRhiConfig` 中指定 `backend = CE_RHI_VULKAN`
- **THEN** 设备创建时初始化 Vulkan 后端（如已实现）
- **AND** OpenGL 后端代码不被链接

---

### Requirement: 渲染资源管理

RHI SHALL 提供以下渲染资源的创建和管理接口：
- **Buffer**：顶点缓冲、索引缓冲、Uniform 缓冲
- **Texture**：2D 纹理、CubeMap、渲染目标
- **Shader**：顶点着色器、片段着色器
- **Pipeline**：渲染管线状态（混合、深度、光栅化）

资源 MUST 支持创建和销毁的完整生命周期管理。资源句柄 MUST 为不透明类型，隐藏后端具体实现。

#### Scenario: 纹理资源生命周期

- **WHEN** 创建纹理、使用纹理渲染一帧、然后销毁纹理
- **THEN** 纹理内存被正确释放，无内存泄漏
- **AND** 销毁后继续使用该纹理句柄不会导致崩溃（返回错误或静默忽略）

#### Scenario: Pipeline 状态配置

- **WHEN** 创建两个不同混合模式的 Pipeline（不透明和半透明）
- **THEN** 分别绑定两个 Pipeline 时渲染结果正确反映各自的混合模式
- **AND** Pipeline 切换开销在可接受范围内

---

### Requirement: MVP 渲染验收

v0.1 MVP 渲染层 MUST 满足以下验收标准：
- Client 模式下打开窗口并显示 Dear ImGui 界面
- Client 模式下渲染一个三角形
- 渲染循环稳定运行，帧率不低于 30 FPS

#### Scenario: MVP 三角形渲染

- **WHEN** 编译 Client 模式并运行
- **THEN** 窗口成功创建，显示 Dear ImGui 演示窗口
- **AND** 3D 视口内渲染一个可见的彩色三角形

#### Scenario: 渲染循环稳定性

- **WHEN** 运行 Client 模式持续 60 秒
- **THEN** 帧率始终不低于 30 FPS
- **AND** 无崩溃、无渲染错误、无内存泄漏
