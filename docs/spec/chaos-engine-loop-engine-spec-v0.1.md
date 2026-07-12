# LoopEngine 设计规格说明书 v0.1

> **状态：** 草案 | **日期：** 2026-07-12 | **作者：** 老陈（zhongfangdao）
>
> **主题：** AI 驱动的 ChaosEngine 全流程闭环验证平台——开发 / 测试 / 验证 / 客户端界面表现，四域闭环

---

## 目录

1. [概述与目标](#1-概述与目标)
2. [总体架构](#2-总体架构)
3. [闭环域设计](#3-闭环域设计)
4. [客户端界面表现验证层（重点）](#4-客户端界面表现验证层重点)
5. [AI 调度层](#5-ai-调度层)
6. [与 ChaosEngine 现有基建的对接](#6-与-chaosengine-现有基建的对接)
7. [API 设计](#7-api-设计)
8. [MVP 定义](#8-mvp-定义)
9. [路线图](#9-路线图)
10. [风险与约束](#10-风险与约束)
11. [附录](#11-附录)

---

## 1. 概述与目标

### 1.1 一句话定义

**LoopEngine** 是一个独立于 ChaosEngine 内核的外部 AI 驱动验证平台，通过多模型 AI 调度 + 结构化断言 + 视觉复核的三层验证机制，将 ChaosEngine 的开发（写码/编译）、测试（单元/集成/压测）、验证（服务端逻辑/数据一致性）、客户端界面表现（渲染/交互）四个环节串联为完整的自动化闭环。

### 1.2 解决什么问题

ChaosEngine 目前的质量保障体系存在以下断层：

| 断层 | 现状 | LoopEngine 解决方案 |
|------|------|---------------------|
| **开发→测试断裂** | 人工写码 → 手动 `build_and_test.sh` → 看结果 | AI 接收开发任务 → 写码 → 自动编译 → 自动跑测试 → 失败时 AI 分析修复 |
| **测试→验证断裂** | 单元测试通过 ≠ 集群逻辑正确 | 测试通过后自动启动全集群 → 验证服务端逻辑/数据一致性 |
| **验证→客户端断裂** | 服务端正确 ≠ 客户端渲染正确 | 验证通过后自动启动客户端 → 结构化断言 + 截图视觉复核 |
| **全流程无人值守** | 每个环节需人工介入判断 | AI 编排四个闭环域，端到端自动执行+判定+修复迭代 |

### 1.3 与 ChaosEngine 的关系

```
┌──────────────────────────────────────────────────────────────────┐
│                        LoopEngine (外部平台)                      │
│                                                                  │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────────┐ │
│  │ 开发闭环  │  │ 测试闭环  │  │ 验证闭环  │  │ 客户端界面闭环    │ │
│  │ Dev Loop │  │ Test Loop│  │ Verify   │  │ Client UI Loop   │ │
│  │          │  │          │  │ Loop     │  │                  │ │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────────┬─────────┘ │
│       │             │             │                 │           │
│  ┌────┴─────────────┴─────────────┴─────────────────┴────────┐  │
│  │                    AI 调度层 (Orchestrator)                │  │
│  │         flash / glm / pro 多模型路由 + 子 agent 编排        │  │
│  └───────────────────────────┬────────────────────────────────┘  │
│                              │ 调用                               │
└──────────────────────────────┼──────────────────────────────────┘
                               │
                    ┌──────────▼──────────┐
                    │   ChaosEngine 内核   │
                    │   (不修改，只调用)    │
                    │                      │
                    │  scripts/*.sh        │
                    │  .gitee-ci.yml       │
                    │  openspec/           │
                    │  src_c/ / src_lua/   │
                    └──────────────────────┘
```

**核心原则：**

- **不改内核**：LoopEngine 不修改 ChaosEngine 的任何 C/Lua 代码，只通过外部脚本调用、进程管理、日志采集、截图采集等方式交互
- **复用基建**：直接调用 ChaosEngine 现有的 24 个脚本、5 个 CI job、6 份 spec、openspec 体系
- **外部观察**：LoopEngine 以"外部观察者"身份运行，通过进程输出、窗口标题栏、日志文件、截图等可观察信号判定结果
- **AI 闭环**：每个闭环域内，AI 可以接收任务 → 执行 → 判定 → 失败时分析 → 修复 → 重试，形成自动迭代

### 1.4 设计原则

| 原则 | 说明 |
|------|------|
| **不改内核** | LoopEngine 是纯外部平台，ChaosEngine 内核零侵入 |
| **脚本优先** | 优先复用 ChaosEngine 现有脚本，不重复造轮子 |
| **结构化断言先行** | 客户端验证先用结构化数据快速过滤，避免每帧都调视觉模型 |
| **视觉复核兜底** | 结构化断言失败时才截图送视觉模型，控制成本 |
| **多模型分级** | 按任务复杂度路由模型——日常用 flash、关键判定用 glm/pro |
| **成本可控** | AI 调用有预算控制，视觉模型调用有频率限制 |
| **可观测** | LoopEngine 自身运行状态全程可观测（日志/仪表盘） |
| **幂等可重试** | 每个闭环步骤可独立重跑，不依赖前序状态 |

### 1.5 不做什么（明确边界）

- ❌ 不修改 ChaosEngine 内核代码（C/Lua 均不改）
- ❌ 不替代 ChaosEngine 的 CI（LoopEngine 可触发 CI，但不替换 `.gitee-ci.yml`）
- ❌ 不做 ChaosEngine 的功能开发（开发闭环中的"写码"是 AI agent 的行为，不是 LoopEngine 内置功能）
- ❌ 不做通用的 AI Agent 框架（LoopEngine 专注 ChaosEngine 验证场景）
- ❌ 不做生产环境监控（仅开发/测试阶段使用）
- ❌ v0.1 不做分布式执行（单机运行，v0.3 考虑多机）

---

## 2. 总体架构

### 2.1 分层设计

```
┌──────────────────────────────────────────────────────────────────┐
│                    用户交互层 (User Interface)                    │
│  ┌────────────┐  ┌────────────┐  ┌──────────────────────────┐   │
│  │ CLI 命令行  │  │ 配置文件    │  │ 运行报告 (Markdown/JSON)  │   │
│  │ loopengine │  │ loop.yaml  │  │ 报告自动生成              │   │
│  └────────────┘  └────────────┘  └──────────────────────────┘   │
├──────────────────────────────────────────────────────────────────┤
│                    闭环编排层 (Loop Orchestrator)                 │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────────┐ │
│  │ 开发闭环  │  │ 测试闭环  │  │ 验证闭环  │  │ 客户端界面闭环    │ │
│  │ DevLoop  │  │ TestLoop │  │ VerifLoop│  │ ClientUILoop     │ │
│  └──────────┘  └──────────┘  └──────────┘  └──────────────────┘ │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │              闭环状态机 (Loop State Machine)                │ │
│  │   PENDING → RUNNING → VERIFYING → PASSED / FAILED /         │ │
│  │   RETRYING → ... → PASSED / FAILED                          │ │
│  └────────────────────────────────────────────────────────────┘ │
├──────────────────────────────────────────────────────────────────┤
│                    AI 调度层 (AI Orchestrator)                   │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────────┐  │
│  │ 任务拆解器    │  │ 模型路由器    │  │ 子 agent 编排器       │  │
│  │ TaskSplitter │  │ ModelRouter  │  │ SubAgentOrchestrator │  │
│  └──────────────┘  └──────────────┘  └──────────────────────┘  │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │              ARK 模型池 (Model Pool)                        │ │
│  │  deepseek-v4-flash (省) │ glm-5-2 (通用) │ deepseek-v4-pro │ │
│  │  qwen3-14b (备选)       │ vision provider (截图复核)        │ │
│  └────────────────────────────────────────────────────────────┘ │
├──────────────────────────────────────────────────────────────────┤
│                    对接适配层 (ChaosEngine Adapter)               │
│  ┌────────────┐  ┌────────────┐  ┌────────────┐  ┌──────────┐  │
│  │ 脚本执行器  │  │ 进程管理器  │  │ 日志采集器  │  │ 截图器    │  │
│  │ ScriptExec │  │ ProcMgr   │  │ LogCollect │  │ Capture  │  │
│  └────────────┘  └────────────┘  └────────────┘  └──────────┘  │
│  ┌────────────┐  ┌────────────┐  ┌──────────────────────────┐  │
│  │ CI 触发器   │  │ Spec 联动   │  │ Admin API 查询器          │  │
│  │ CITrigger  │  │ SpecLink   │  │ AdminQuery               │  │
│  └────────────┘  └────────────┘  └──────────────────────────┘  │
├──────────────────────────────────────────────────────────────────┤
│                    数据层 (Data Layer)                           │
│  ┌────────────┐  ┌────────────┐  ┌────────────┐  ┌──────────┐  │
│  │ 任务存储    │  │ 结果存储    │  │ 截图存储    │  │ 日志存储  │  │
│  │ SQLite     │  │ SQLite     │  │ 文件系统    │  │ 文件系统  │  │
│  └────────────┘  └────────────┘  └────────────┘  └──────────┘  │
└──────────────────────────────────────────────────────────────────┘
```

### 2.2 核心组件

#### 2.2.1 闭环编排器 (Loop Orchestrator)

整个系统的中枢，负责：

1. **接收任务**：从 CLI/配置文件接收验证任务
2. **拆解为闭环域**：将任务映射到开发/测试/验证/客户端界面四个闭环域
3. **编排执行顺序**：按依赖关系串联各闭环域（开发→测试→验证→客户端）
4. **管理状态流转**：跟踪每个闭环域的状态（PENDING/RUNNING/VERIFYING/PASSED/FAILED/RETRYING）
5. **触发重试**：闭环域失败时，调用 AI 分析原因并决定是否重试
6. **生成报告**：汇总所有闭环域的结果，生成最终报告

#### 2.2.2 AI 调度器 (AI Orchestrator)

负责所有 AI 相关的调度决策：

1. **任务拆解**：将复杂任务拆分为可独立执行的子任务
2. **模型路由**：根据任务复杂度选择合适的 ARK 模型
3. **子 agent 编排**：为复杂任务启动多个子 agent 并行/串行执行
4. **结果判定**：AI 判定任务结果是否合格（结构化断言失败的复杂场景）
5. **失败分析**：分析失败原因，生成修复建议或直接修复

#### 2.2.3 对接适配层 (ChaosEngine Adapter)

封装所有与 ChaosEngine 的交互：

1. **ScriptExec**：执行 `scripts/` 下的 24 个脚本，捕获 stdout/stderr/exit code
2. **ProcMgr**：管理 ChaosEngine 集群进程的启动/停止/状态查询
3. **LogCollect**：采集 `logs/*.log` 日志文件内容
4. **Capture**：对客户端窗口截图（X11 `import`/`xwd` + 转换）
5. **CITrigger**：触发 Gitee CI 流水线
6. **SpecLink**：读取 openspec/ 体系，关联验证任务与 spec 条目
7. **AdminQuery**：通过 Admin API（HTTP :9090）查询引擎运行时状态

### 2.3 数据流

```
用户下达验证任务
    │
    ▼
Loop Orchestrator
    │ 解析任务 → 拆解为闭环域序列
    │
    ├──► [1] 开发闭环 (DevLoop)
    │       │ AI agent 接收开发需求
    │       │ AI 写码/修改代码
    │       │ ScriptExec: build_and_test.sh --no-test
    │       │ 判定: 编译是否通过?
    │       │   ├─ 通过 → 进入测试闭环
    │       │   └─ 失败 → AI 分析编译错误 → 修复 → 重试 (max 3)
    │       │
    │       ▼
    ├──► [2] 测试闭环 (TestLoop)
    │       │ CITrigger: 推送分支 -> 触发 .gitee-ci.yml 流水线
    │       │ CITrigger: 轮询 CI 状态 (build-test/smoke/lua-lint/valgrind)
    │       │ 判定: CI 各 job 是否全 passed?（本地不跑测试）
    │       │   ├─ 全 passed -> 进入验证闭环
    │       │   └─ 任一 failed -> AI 拉取 CI 日志分析 -> 修复 -> 重试 (max 3)
    │       │
    │       ▼
    ├──► [3] 验证闭环 (VerifyLoop)
    │       │ ProcMgr: start_cluster.sh --all
    │       │ ScriptExec: test_client.sh (TCP/WS/Admin 冒烟)
    │       │ ScriptExec: verify_client_sync.sh (同步验证)
    │       │ ScriptExec: test_sync.sh / test_save.sh / test_dbproxy.sh
    │       │ AdminQuery: GET /api/stats, /api/aoi, /api/cell ...
    │       │ 判定: 服务端逻辑+数据一致性是否正确?
    │       │   ├─ 正确 → 进入客户端界面闭环
    │       │   └─ 异常 → AI 分析日志 → 修复 → 重试 (max 3)
    │       │
    │       ▼
    └──► [4] 客户端界面闭环 (ClientUILoop)
            │ ProcMgr: start_client.sh --vulkan
            │ LogCollect: 采集客户端 stdout 日志
            │ Capture: xwd/import 截图
            │ 结构化断言:
            │   - 标题栏解析: FPS/坐标/visible数/连接状态
            │   - 日志断言: "Vulkan device created" / "ENTITY_UPDATE" / "Spawn pos"
            │   - 窗口存在性: xwininfo 确认窗口已创建
            │ 判定: 结构化断言是否全通过?
            │   ├─ 全通过 → 客户端验证 PASSED
            │   ├─ 部分失败 → 触发截图送视觉模型复核
            │   │   ├─ 视觉确认正常 → PASSED (结构化断言误报)
            │   │   └─ 视觉确认异常 → AI 分析 → 修复 → 重试 (max 2)
            │   └─ 全失败 → AI 分析 → 修复 → 重试 (max 2)
            │
            ▼
    生成最终报告 (Markdown + JSON)
```

### 2.4 目录结构

```
loop-engine/                          # 独立于 chaos-engine 的项目
├── loopengine/                       # Python 主包
│   ├── __init__.py
│   ├── cli.py                        # CLI 入口 (loopengine 命令)
│   ├── orchestrator/                 # 闭环编排层
│   │   ├── __init__.py
│   │   ├── loop_state.py             # 闭环状态机
│   │   ├── dev_loop.py               # 开发闭环
│   │   ├── test_loop.py              # 测试闭环
│   │   ├── verify_loop.py            # 验证闭环
│   │   └── client_ui_loop.py         # 客户端界面闭环
│   ├── ai/                           # AI 调度层
│   │   ├── __init__.py
│   │   ├── task_splitter.py          # 任务拆解器
│   │   ├── model_router.py           # 模型路由器
│   │   ├── sub_agent.py              # 子 agent 编排器
│   │   └── ark_client.py             # ARK API 客户端
│   ├── adapter/                      # ChaosEngine 对接适配层
│   │   ├── __init__.py
│   │   ├── script_exec.py            # 脚本执行器
│   │   ├── proc_manager.py           # 进程管理器
│   │   ├── log_collector.py          # 日志采集器
│   │   ├── capture.py                # 截图器
│   │   ├── ci_trigger.py             # CI 触发器
│   │   ├── spec_link.py              # Spec 联动
│   │   └── admin_query.py            # Admin API 查询
│   ├── assertion/                    # 断言引擎
│   │   ├── __init__.py
│   │   ├── structured.py             # 结构化断言引擎
│   │   ├── title_bar_parser.py       # 标题栏解析器
│   │   ├── log_assertion.py          # 日志断言
│   │   └── vision_review.py          # 视觉复核
│   ├── data/                         # 数据层
│   │   ├── __init__.py
│   │   ├── db.py                     # SQLite 存储
│   │   └── models.py                 # 数据模型
│   └── report/                       # 报告生成
│       ├── __init__.py
│       ├── markdown.py               # Markdown 报告
│       └── json_report.py            # JSON 报告
├── config/                           # 配置文件
│   ├── loop.yaml                     # 默认配置
│   └── models.yaml                   # 模型配置
├── tests/                            # LoopEngine 自身测试
│   ├── test_assertion.py
│   ├── test_model_router.py
│   └── test_loop_state.py
├── docs/                             # 文档
│   └── architecture.md
├── pyproject.toml                    # Python 项目配置
└── README.md
```

---

## 3. 闭环域设计

### 3.1 闭环域通用模型

每个闭环域遵循统一的状态机模型：

```
                    ┌──────────┐
                    │ PENDING  │
                    └────┬─────┘
                         │ start
                         ▼
                    ┌──────────┐
              ┌─────│ RUNNING  │
              │     └────┬─────┘
              │          │ execute
              │          ▼
              │     ┌──────────┐
              │     │VERIFYING │
              │     └────┬─────┘
              │          │
              │     ┌────┴────┐
              │     │         │
              │     ▼         ▼
              │ ┌──────┐  ┌──────┐
              │ │PASSED│  │FAILED│
              │ └──────┘  └──┬───┘
              │              │ retry < max_retries?
              │              ├─ 是 ──► RUNNING (RETRYING)
              │              └─ 否 ──► FAILED (final)
              │
              └─► (timeout/cancel) ──► FAILED
```

**通用接口定义（Python Protocol）：**

```python
from typing import Protocol
from dataclasses import dataclass
from enum import Enum

class LoopStatus(Enum):
    PENDING = "pending"
    RUNNING = "running"
    VERIFYING = "verifying"
    PASSED = "passed"
    FAILED = "failed"
    RETRYING = "retrying"

@dataclass
class LoopResult:
    status: LoopStatus
    retry_count: int
    duration_sec: float
    artifacts: dict          # 截图路径、日志路径等
    assertions: list         # 断言结果列表
    ai_analysis: str | None  # AI 分析文本（失败时）
    error: str | None        # 错误信息

class LoopDomain(Protocol):
    """闭环域通用接口"""
    
    @property
    def name(self) -> str: ...
    
    @property
    def max_retries(self) -> int: ...
    
    async def execute(self, context: "LoopContext") -> LoopResult: ...
    
    async def verify(self, context: "LoopContext") -> LoopResult: ...
    
    async def retry(self, context: "LoopContext", reason: str) -> LoopResult: ...
```

### 3.2 开发闭环 (DevLoop)

#### 3.2.1 定位

接收开发需求（新功能/Bug 修复/重构），由 AI agent 完成代码编写，自动编译验证。

#### 3.2.2 输入

| 输入 | 来源 | 说明 |
|------|------|------|
| 开发需求描述 | 用户/CI/Spec | 自然语言描述或 openspec proposal |
| 关联 Spec | openspec/ | 对应的 spec 文件路径 |
| ChaosEngine 代码库 | 文件系统 | `/home/zhongfangdao/chaos-engine/` |

#### 3.2.3 输出

| 输出 | 说明 |
|------|------|
| 代码变更 | git diff / patch 文件 |
| 编译结果 | exit code + stdout/stderr |
| LoopResult | 状态/断言/AI分析 |

#### 3.2.4 AI 介入点

| 介入点 | 模型 | 说明 |
|--------|------|------|
| 需求理解+任务拆解 | glm-5-2 | 解析需求，拆解为文件级修改任务 |
| 代码编写 | deepseek-v4-pro | 顶尖推理模型写核心逻辑 |
| 简单修改/补丁 | deepseek-v4-flash | 省钱模型做简单替换 |
| 编译错误分析 | glm-5-2 | 分析编译失败日志，定位原因 |
| 修复建议 | glm-5-2 | 生成修复 diff |

#### 3.2.5 判定逻辑

```
执行流程:
  1. AI agent 接收需求 → 分析关联 spec → 拆解为子任务
  2. AI agent 逐个执行子任务（写码/修改文件）
  3. ScriptExec: ./scripts/build_and_test.sh --no-test
  4. 判定:
     - exit_code == 0  →  编译通过  →  PASSED
     - exit_code != 0  →  编译失败  →  AI 分析 stderr → 修复 → RETRYING

断言规则:
  ASSERT compile_exit_code == 0
  ASSERT binary_exists("chaos_server")
  ASSERT binary_exists("chaos_client")
  ASSERT binary_exists("chaos_headless")
  ASSERT binary_exists("chaos_gateway")
  ASSERT binary_exists("chaos_router")
  ASSERT binary_exists("chaos_dbproxy")
```

#### 3.2.6 数据流

```
开发需求
    │
    ▼
AI TaskSplitter (glm-5-2)
    │ 拆解为子任务列表
    │
    ├──► SubAgent 1: 修改 ce_aoi.c (deepseek-v4-pro)
    ├──► SubAgent 2: 修改 ce_cell.h (deepseek-v4-pro)
    └──► SubAgent 3: 更新 Lua 脚本 (deepseek-v4-flash)
    │
    ▼
ScriptExec: build_and_test.sh --no-test
    │
    ├─ exit_code == 0 → PASSED → 传递给 TestLoop
    └─ exit_code != 0 → AI 分析 (glm-5-2)
         │
         ├─ 可自动修复 → 修复 → RETRYING (回到编译)
         └─ 不可自动修复 → FAILED → 报告错误
```

### 3.3 测试闭环 (TestLoop)

#### 3.3.1 定位

在编译通过后，**不本地执行测试脚本**，而是驱动 ChaosEngine 的 Git CI 流水线完成测试，回采 CI 结果做判定。核心原则：测试流程必须通过 Git CI 完成，LoopEngine 只负责触发、轮询、解析、判定，不重复编译、不重复执行测试，复用 CI 已有构建产物和 job。

#### 3.3.2 输入

| 输入 | 来源 | 说明 |
|------|------|------|
| 代码变更 | DevLoop 输出 | 已提交到分支的代码改动（含 commit SHA） |
| CI 配置 | `.gitee-ci.yml` | 5 个 job：build-test/smoke/release/lua-lint/valgrind |
| CI 凭证 | config/loop.yaml | Gitee API token（触发流水线+读日志） |
| 测试配置 | config/loop.yaml | 测试过滤规则、CI 超时时间、目标 job 列表 |

#### 3.3.3 输出

| 输出 | 说明 |
|------|------|
| CI 流水线状态 | run_id / 各 job 状态（running/passed/failed/skipped） |
| 测试结果 | 从 CI 日志解析的 ctest 通过/失败数、lua-lint 结果、valgrind 结果 |
| 构建产物引用 | CI job 产出的二进制 artifact 引用（不下载，按需拉取） |
| LoopResult | 状态/断言/AI分析 |

#### 3.3.4 AI 介入点

| 介入点 | 模型 | 说明 |
|--------|------|------|
| CI 流水线失败分析 | glm-5-2 | 拉取失败 job 的日志，定位失败原因（编译/测试/内存） |
| 测试用例失败分析 | glm-5-2 | 解析 ctest --output-on-failure 输出，定位失败用例 |
| Lua 语法错误分析 | deepseek-v4-flash | 简单语法错误快速定位（lua-lint job 失败时） |
| Valgrind 泄漏分析 | deepseek-v4-pro | 深度推理内存泄漏根因（valgrind job 失败时） |
| 修复建议 | glm-5-2 | 生成修复 diff，提交到分支后重新触发 CI |
| 新增测试用例建议 | deepseek-v4-pro | 深度推理需要什么测试覆盖 |

#### 3.3.5 判定逻辑

```
执行流程（CI 驱动模式，本地不跑测试）:
  1. 确认 DevLoop 的代码变更已提交到 CI 触发分支（如 develop/test-loop）
  2. CITrigger.trigger_pipeline(branch)
     - Gitee Go API 触发流水线，返回 run_id
     - 复用 .gitee-ci.yml 现有 job，不新增、不修改
  3. CITrigger.poll_until_complete(run_id, timeout=1800s)
     - 轮询 GET /repos/{owner}/{repo}/actions/runs/{run_id}
     - 间隔 15s，超时则标记 TIMEOUT
  4. CITrigger.parse_test_results(run_id)
     - 拉取各 job 日志，解析结构化结果:
       build-test job  -> ctest 通过/失败数、冒烟测试项数
       lua-lint job    -> 语法错误数
       valgrind job    -> 内存泄漏数
       smoke job       -> 集群冒烟通过/失败
  5. 判定:
     - 所有目标 job 全 passed  ->  PASSED -> 传递给 VerifyLoop
     - 任一 job failed  ->  AI 分析失败 job 日志 -> 修复 -> 提交 -> 重新触发 CI (RETRYING, max 3)

断言规则（基于 CI 结果，非本地执行）:
  ASSERT build_test_job.status == "passed"
  ASSERT ctest_pass_count >= 18            # CI 日志解析: 至少 18 个测试通过
  ASSERT smoke_job.status == "passed"
  ASSERT lua_lint_job.status == "passed"
  ASSERT valgrind_job.status in ("passed", "skipped")  # 可选 job
  ASSERT ci_total_duration < 1800          # CI 总时长不超 30 分钟

CI job 复用映射（不新增 job，全部复用现有 5 job）:
  - build-test   -> 编译 + 18 单元测试 + 冒烟 + 客户端同步验证（复用构建产物）
  - smoke        -> 集群冒烟（复用 build-test 产物）
  - lua-lint     -> Lua 语法检查
  - valgrind     -> Valgrind 内存检测
  - release      -> Release 编译验证（可选，仅发版前触发）

关键测试用例（从 CI build-test job 日志解析）:
  - test_math / test_memory / test_ecs / test_network
  - test_aoi / test_cell / test_net_base
```

#### 3.3.6 数据流

```
DevLoop PASSED（代码已提交到分支）
    │
    ▼
CITrigger.trigger_pipeline(branch)
    │  推送/提交触发 .gitee-ci.yml 流水线
    │  （不本地执行 build_and_test.sh）
    │
    ▼
CITrigger.poll_until_complete(run_id)
    │  轮询 Gitee Go API，等待 CI 完成
    │
    ├─ 所有目标 job passed
    │   -> 解析 ctest/冒烟/lint/valgrind 结果
    │   -> PASSED -> 传递给 VerifyLoop
    │
    └─ 任一 job failed
        │
        ▼
    CITrigger.fetch_job_logs(run_id, failed_job)
        │  拉取失败 job 的完整日志
        │
        ▼
    AI 分析 (glm-5-2 / deepseek-v4-pro)
        │ 读取 CI 日志
        │ 读取 ctest --output-on-failure 输出
        │
        ├─ 可修复 -> 生成 patch -> 提交到分支 -> 重新触发 CI -> RETRYING
        │                                                          (max 3 次重试)
        └─ 无法自动修复 -> FAILED（附 AI 分析报告）
```

#### 3.3.7 CI 驱动模式的技术约束

1. **不本地执行测试脚本**：build_and_test.sh / test_*.sh 等脚本仅由 CI 执行，LoopEngine 不重复调用
2. **不新增 CI job**：复用 .gitee-ci.yml 现有 5 job，LoopEngine 只触发和解析，不修改 CI 配置
3. **复用构建产物**：CI 产出的二进制 artifact 按需通过 Gitee artifact API 拉取（如客户端界面闭环需要客户端二进制），不重新编译
4. **CI 凭证隔离**：Gitee API token 存 config/loop.yaml，与 ARK 模型 key 分离
5. **CI 失败重试上限**：AI 修复后重新触发 CI，最多 3 次，超过则标记 FAILED 交人工

### 3.4 验证闭环 (VerifyLoop)

#### 3.4.1 定位

测试通过后，启动全集群，验证服务端逻辑正确性和数据一致性。

#### 3.4.2 输入

| 输入 | 来源 | 说明 |
|------|------|------|
| 编译产物 | DevLoop 输出 | 全部二进制就绪 |
| 集群配置 | start_cluster.sh 默认 | 端口/PID 文件路径 |

#### 3.4.3 输出

| 输出 | 说明 |
|------|------|
| 集群运行日志 | `logs/*.log` 全部服务日志 |
| Admin API 快照 | 各端点 JSON 响应 |
| 同步验证结果 | verify_client_sync.sh 输出 |
| LoopResult | 状态/断言/AI分析 |

#### 3.4.4 AI 介入点

| 介入点 | 模型 | 说明 |
|--------|------|------|
| 集群启动失败分析 | glm-5-2 | 分析启动日志，定位端口冲突/依赖缺失 |
| 数据一致性异常分析 | deepseek-v4-pro | 深度推理 AOI/Cell/同步逻辑的异常原因 |
| Admin API 数据校验 | deepseek-v4-flash | 快速比对 API 返回值是否符合预期 |
| 修复建议 | glm-5-2 | 生成修复 diff |

#### 3.4.5 判定逻辑

```
执行流程:
  1. ProcMgr: ./scripts/start_cluster.sh --all
     - 启动 DBProxy (sync:9001, db:9003)
     - 启动 Game Server (port:7777, admin:/tmp/chaos_admin.sock)
     - 启动 Router (game:9100, cluster:9101)
     - 启动 Gateway (tcp:9000, kcp:9001, ws:9002)
     - 启动 Admin Web (http://9090)
  2. 等待全部服务就绪 (status.sh --json 检查)
  3. ScriptExec: ./scripts/test_client.sh (TCP/WS/Admin 冒烟)
  4. ScriptExec: ./scripts/test_sync.sh (同步测试)
  5. ScriptExec: ./scripts/test_save.sh (存档测试)
  6. ScriptExec: ./scripts/test_dbproxy.sh (DBProxy 测试)
  7. AdminQuery: GET /api/stats, /api/aoi, /api/cell, /api/network ...
  8. ProcMgr: ./scripts/stop_cluster.sh (清理)

断言规则:
  # 集群启动
  ASSERT all_services_running == true
    services = [dbproxy, game, router, gateway, admin]
  
  # 连通性
  ASSERT tcp_connect("127.0.0.1:7777") == success
  ASSERT tcp_connect("127.0.0.1:9000") == success
  ASSERT ws_handshake("127.0.0.1:9002") == HTTP 101
  ASSERT http_get("http://127.0.0.1:9090/api/health") == 200
  
  # Admin API 数据有效性
  ASSERT api_stats.contains("fps")
  ASSERT api_stats.contains("uptime")
  ASSERT api_aoi.contains("entities")
  ASSERT api_cell.contains("cells")
  
  # 同步验证
  ASSERT verify_client_sync_exit_code == 0
  ASSERT log_contains("Received ENTITY_UPDATE")
  
  # 数据一致性 (AI 判定)
  AI_CHECK api_aoi.entity_count == api_stats.visible_entities
  AI_CHECK no_error_in_logs("logs/*.log")
```

#### 3.4.6 数据流

```
TestLoop PASSED
    │
    ▼
ProcMgr: start_cluster.sh --all
    │
    ├─ 全部服务 RUNNING
    │   │
    │   ▼
    │ ScriptExec: test_client.sh + test_sync.sh + test_save.sh + test_dbproxy.sh
    │ AdminQuery: /api/stats, /api/aoi, /api/cell, /api/network
    │   │
    │   ├─ 全部断言通过 → PASSED → 传递给 ClientUILoop
    │   └─ 部分失败 → AI 分析 (glm-5-2 / pro)
    │       │ 读取 logs/*.log
    │       │ 读取 Admin API 快照
    │       │
    │       ├─ 可修复 → 修复 → RETRYING (回到 TestLoop)
    │       └─ 不可修复 → FAILED → 报告
    │
    └─ 启动失败 → AI 分析启动日志 → FAILED/RETRYING
```

### 3.5 客户端界面表现闭环 (ClientUILoop)

#### 3.5.1 定位

验证闭环通过后，启动 Vulkan 客户端，验证渲染正确性和交互响应性。这是 LoopEngine 最核心的差异化能力——**用 AI 闭环客户端界面表现验证**。

#### 3.5.2 输入

| 输入 | 来源 | 说明 |
|------|------|------|
| 集群运行状态 | VerifyLoop 输出 | 全集群已就绪 |
| 显示环境 | 环境变量 | DISPLAY / XAUTHORITY |
| 客户端二进制 | build/bin/chaos_client | Vulkan 渲染客户端 |

#### 3.5.3 输出

| 输出 | 说明 |
|------|------|
| 客户端日志 | stdout 采集 |
| 标题栏数据序列 | 每秒采样的标题栏文本 |
| 截图文件 | PNG/JPEG 格式 |
| LoopResult | 状态/断言/AI分析/截图路径 |

#### 3.5.4 核心验证策略：两层过滤

```
客户端运行
    │
    ▼
┌────────────────────────────────────────┐
│  第一层：结构化断言 (快速过滤，零成本)    │
│                                        │
│  采集信号:                              │
│  1. 窗口标题栏 (xprop -name ... WM_NAME)│
│     → 解析 FPS / 坐标 / visible数 /     │
│       连接状态 / keys 状态              │
│  2. stdout 日志                         │
│     → "Vulkan device created"          │
│     → "Spawn pos: (x, y, z)"           │
│     → "Received ENTITY_UPDATE"          │
│     → "[Client] Local pos: ..."         │
│  3. 窗口存在性                          │
│     → xwininfo 确认窗口已创建           │
│  4. 进程存活                            │
│     → kill -0 检查进程未崩溃            │
│                                        │
│  判定:                                  │
│  ├─ 全通过 → PASSED (无需视觉模型)      │
│  ├─ 部分失败 → 触发第二层              │
│  └─ 全失败 → 直接 FAILED               │
└──────────────────┬─────────────────────┘
                   │ 部分失败
                   ▼
┌────────────────────────────────────────┐
│  第二层：截图视觉复核 (精准判定，有成本)  │
│                                        │
│  1. 截图: import -window <id> out.png  │
│     或: xwd -id <id> → convert png     │
│  2. 送视觉模型:                         │
│     "这是 ChaosEngine Vulkan 客户端截图  │
│      请检查: 1)窗口是否有渲染内容       │
│              2)是否有三角形/球体         │
│              3)是否黑屏/白屏/花屏        │
│              4)文字是否清晰"            │
│  3. 视觉模型返回结构化判定              │
│                                        │
│  判定:                                  │
│  ├─ 视觉确认正常 → PASSED (误报修正)    │
│  └─ 视觉确认异常 → FAILED → AI 分析    │
└────────────────────────────────────────┘
```

#### 3.5.5 AI 介入点

| 介入点 | 模型 | 说明 |
|--------|------|------|
| 标题栏数据异常分析 | deepseek-v4-flash | 快速判断 FPS 低/visible=0 是否合理 |
| 截图视觉判定 | vision provider | 判断渲染是否正常（黑屏/花屏/正常） |
| 渲染异常根因分析 | deepseek-v4-pro | 深度推理 Vulkan 管线/着色器问题 |
| 修复建议 | glm-5-2 | 生成修复 diff |

#### 3.5.6 判定逻辑（详细）

详见 [第 4 节：客户端界面表现验证层](#4-客户端界面表现验证层重点)。

---

## 4. 客户端界面表现验证层（重点）

本节是 LoopEngine 最核心的差异化设计，详细阐述结构化断言 + 截图视觉复核的两层验证机制。

### 4.1 设计动机

ChaosEngine 客户端是基于 Vulkan 1.4 的 X11 窗口应用，运行在 Wayland + XWayland 环境下。传统验证方式的痛点：

| 痛点 | 传统方式 | LoopEngine 方案 |
|------|----------|-----------------|
| 渲染是否正常 | 人眼看截图 | AI 视觉模型判定 |
| FPS 是否达标 | 无自动采集 | 标题栏解析 FPS 值 |
| 球体是否可见 | 人眼看截图 | 标题栏 visible 数 + 视觉复核 |
| 网络是否连通 | 看 stdout 日志 | 标题栏连接状态 + 日志断言 |
| 输入是否响应 | 人手按键测试 | 标题栏 keys 字段 + xdotool 模拟 |
| 成本控制 | 每帧截图送AI = 破产 | 结构化断言快速过滤，仅失败时截图 |

### 4.2 客户端可观察信号清单

基于 ChaosEngine 客户端源码 (`src_c/runtime/ce_client_main.c`) 的实际行为分析：

#### 4.2.1 标题栏信号

客户端每帧更新窗口标题栏（通过 `rhi_set_window_title` → `XStoreName`），格式为：

**已连接状态：**
```
ChaosEngine Vulkan | pos=(%.2f, %.2f, %.2f) | visible=%d | keys=%s
```

**未连接状态：**
```
ChaosEngine Vulkan | pos=(%.2f, %.2f, %.2f) | disconnected | keys=%s
```

可解析字段：

| 字段 | 正则 | 含义 | 正常值范围 |
|------|------|------|-----------|
| `pos` | `pos=\(([\d.-]+), ([\d.-]+), ([\d.-]+)\)` | 球体坐标 (x, y, z) | x,z ∈ [-2.5, 2.5]（spawn半径=10*0.25=2.5） |
| `visible` | `visible=(\d+)` | 可见实体数量 | ≥0，连接后应 >0 |
| `连接状态` | `disconnected` 或 `visible=` | 网络连接状态 | 应为 `visible=`（已连接） |
| `keys` | `keys=([WSAD\-]{4})` | 按键状态 | ---- 表示无按键，如 W-- 表示按了W |

**注意：** 标题栏更新频率为每帧，但采样频率应控制在每秒一次（与标题栏数据更新逻辑一致），避免过度采集。

#### 4.2.2 stdout 日志信号

客户端 stdout 输出（精简设计，不刷屏）：

| 时机 | 输出内容 | 正则 | 用途 |
|------|----------|------|------|
| 启动 | `========...` 横幅 | `^=+$` | 确认客户端启动 |
| 启动 | `ChaosEngine Vulkan Client v0.1.0` | `ChaosEngine Vulkan Client` | 版本确认 |
| 启动 | `Gateway: <host>:<port>` | `Gateway: (.+):(\d+)` | 连接目标确认 |
| Vulkan | `Vulkan device created successfully` | `Vulkan device created` | 渲染初始化成功 |
| 网络 | `No network connection (Gateway may be offline)` | `No network connection` | 网络连接失败 |
| 出生点 | `[Client] Spawn pos: (x, y, z) within r=2.50` | `Spawn pos: \(([\d.-]+), ([\d.-]+), ([\d.-]+)\) within r=([\d.]+)` | 出生点验证 |
| 同步 | `[Client] Local pos: (x, y, z), visible=N` | `Local pos:.*visible=(\d+)` | 帧级状态（每60帧） |
| 同步 | `Received ENTITY_UPDATE` | `Received ENTITY_UPDATE` | 实体同步成功 |
| 实体 | `Entity %u: (x, y, z)` | `Entity (\d+): \(([\d.-]+), ([\d.-]+), ([\d.-]+)\)` | 实体详情 |
| 关闭 | `[Client] Network error, continuing without network` | `Network error` | 网络异常（非致命） |

#### 4.2.3 进程/窗口信号

| 信号 | 采集方式 | 判定 |
|------|----------|------|
| 进程存活 | `kill -0 <pid>` | 进程未崩溃 |
| 窗口存在 | `xwininfo -name "ChaosEngine Vulkan"` | 窗口已创建 |
| 窗口尺寸 | `xwininfo` 解析 Width/Height | 应为 1280x720 |
| 窗口ID | `xdotool search --name "ChaosEngine Vulkan"` | 获取窗口ID用于截图 |

### 4.3 结构化断言机制

#### 4.3.1 数据采集器 (DataCollector)

```python
import re
import subprocess
import time
from dataclasses import dataclass, field
from typing import Optional

@dataclass
class TitleBarSnapshot:
    """标题栏快照"""
    timestamp: float
    raw_text: str
    pos_x: Optional[float] = None
    pos_y: Optional[float] = None
    pos_z: Optional[float] = None
    visible: Optional[int] = None
    connected: bool = False
    keys: str = "----"

@dataclass
class LogSnapshot:
    """日志快照"""
    timestamp: float
    lines: list[str]
    vulkan_initialized: bool = False
    spawn_pos: Optional[tuple[float, float, float]] = None
    entity_update_received: bool = False
    network_error: bool = False
    local_pos_entries: list[dict] = field(default_factory=list)

@dataclass 
class ProcessSnapshot:
    """进程/窗口快照"""
    timestamp: float
    process_alive: bool = False
    window_exists: bool = False
    window_id: Optional[str] = None
    window_width: Optional[int] = None
    window_height: Optional[int] = None

class ClientDataCollector:
    """客户端数据采集器"""
    
    TITLE_BAR_PATTERNS = {
        'pos': re.compile(r'pos=\(([\d.-]+), ([\d.-]+), ([\d.-]+)\)'),
        'visible': re.compile(r'visible=(\d+)'),
        'disconnected': re.compile(r'disconnected'),
        'keys': re.compile(r'keys=([WSAD\-]{4})'),
    }
    
    LOG_PATTERNS = {
        'vulkan_init': re.compile(r'Vulkan device created'),
        'spawn_pos': re.compile(
            r'Spawn pos: \(([\d.-]+), ([\d.-]+), ([\d.-]+)\) within r=([\d.]+)'
        ),
        'entity_update': re.compile(r'Received ENTITY_UPDATE'),
        'network_error': re.compile(r'Network error'),
        'local_pos': re.compile(
            r'Local pos: \(([\d.-]+), ([\d.-]+), ([\d.-]+)\), visible=(\d+)'
        ),
    }
    
    def __init__(self, process_pid: int, window_name: str = "ChaosEngine Vulkan"):
        self.process_pid = process_pid
        self.window_name = window_name
    
    def collect_title_bar(self) -> TitleBarSnapshot:
        """采集窗口标题栏（通过 xprop）"""
        try:
            # xprop 获取窗口标题
            result = subprocess.run(
                ['xprop', '-name', self.window_name, 'WM_NAME'],
                capture_output=True, text=True, timeout=2
            )
            raw = result.stdout
            
            # 解析: WM_NAME(STRING) = "ChaosEngine Vulkan | pos=(...) | ..."
            match = re.search(r'WM_NAME\([^)]*\) = "([^"]*)"', raw)
            title_text = match.group(1) if match else ""
            
            snapshot = TitleBarSnapshot(
                timestamp=time.time(),
                raw_text=title_text,
            )
            
            # 解析各字段
            if pos_match := self.TITLE_BAR_PATTERNS['pos'].search(title_text):
                snapshot.pos_x = float(pos_match.group(1))
                snapshot.pos_y = float(pos_match.group(2))
                snapshot.pos_z = float(pos_match.group(3))
            
            if vis_match := self.TITLE_BAR_PATTERNS['visible'].search(title_text):
                snapshot.visible = int(vis_match.group(1))
                snapshot.connected = True
            elif self.TITLE_BAR_PATTERNS['disconnected'].search(title_text):
                snapshot.connected = False
            
            if keys_match := self.TITLE_BAR_PATTERNS['keys'].search(title_text):
                snapshot.keys = keys_match.group(1)
            
            return snapshot
            
        except Exception as e:
            return TitleBarSnapshot(
                timestamp=time.time(),
                raw_text="",
            )
    
    def collect_log(self, log_lines: list[str]) -> LogSnapshot:
        """从日志行列表采集信号"""
        snapshot = LogSnapshot(
            timestamp=time.time(),
            lines=log_lines,
        )
        
        for line in log_lines:
            if self.LOG_PATTERNS['vulkan_init'].search(line):
                snapshot.vulkan_initialized = True
            
            if m := self.LOG_PATTERNS['spawn_pos'].search(line):
                snapshot.spawn_pos = (
                    float(m.group(1)), float(m.group(2)), float(m.group(3))
                )
            
            if self.LOG_PATTERNS['entity_update'].search(line):
                snapshot.entity_update_received = True
            
            if self.LOG_PATTERNS['network_error'].search(line):
                snapshot.network_error = True
            
            if m := self.LOG_PATTERNS['local_pos'].search(line):
                snapshot.local_pos_entries.append({
                    'x': float(m.group(1)),
                    'y': float(m.group(2)),
                    'z': float(m.group(3)),
                    'visible': int(m.group(4)),
                })
        
        return snapshot
    
    def collect_process(self) -> ProcessSnapshot:
        """采集进程/窗口状态"""
        snapshot = ProcessSnapshot(timestamp=time.time())
        
        # 进程存活
        try:
            subprocess.run(
                ['kill', '-0', str(self.process_pid)],
                check=True, capture_output=True
            )
            snapshot.process_alive = True
        except subprocess.CalledProcessError:
            snapshot.process_alive = False
        
        # 窗口存在
        try:
            result = subprocess.run(
                ['xwininfo', '-name', self.window_name],
                capture_output=True, text=True, timeout=2
            )
            if result.returncode == 0:
                snapshot.window_exists = True
                # 解析窗口ID和尺寸
                if id_match := re.search(r'Window id: (0x[\da-f]+)', result.stdout):
                    snapshot.window_id = id_match.group(1)
                if w_match := re.search(r'Width: (\d+)', result.stdout):
                    snapshot.window_width = int(w_match.group(1))
                if h_match := re.search(r'Height: (\d+)', result.stdout):
                    snapshot.window_height = int(h_match.group(1))
        except Exception:
            pass
        
        return snapshot
```

#### 4.3.2 断言规则引擎 (AssertionEngine)

```python
from dataclasses import dataclass
from enum import Enum
from typing import Any, Callable

class AssertionSeverity(Enum):
    CRITICAL = "critical"    # 失败则整体 FAILED
    WARNING = "warning"      # 失败触发视觉复核
    INFO = "info"            # 仅记录

class AssertionResult(Enum):
    PASSED = "passed"
    FAILED = "failed"
    SKIPPED = "skipped"

@dataclass
class Assertion:
    """单条断言规则"""
    name: str
    description: str
    severity: AssertionSeverity
    check: Callable[[dict], bool]
    extract: Callable[[dict], Any]  # 提取断言相关数据用于报告

@dataclass 
class AssertionReport:
    """断言执行报告"""
    name: str
    description: str
    severity: AssertionSeverity
    result: AssertionResult
    actual_value: Any
    expected: str
    timestamp: float

class ClientAssertionEngine:
    """客户端断言引擎"""
    
    def __init__(self):
        self.assertions: list[Assertion] = []
        self._setup_default_assertions()
    
    def _setup_default_assertions(self):
        """注册默认断言规则"""
        
        # === CRITICAL 断言 ===
        
        self.assertions.append(Assertion(
            name="process_alive",
            description="客户端进程存活",
            severity=AssertionSeverity.CRITICAL,
            check=lambda ctx: ctx['process'].process_alive,
            extract=lambda ctx: ctx['process'].process_alive,
        ))
        
        self.assertions.append(Assertion(
            name="window_exists",
            description="Vulkan 窗口已创建",
            severity=AssertionSeverity.CRITICAL,
            check=lambda ctx: ctx['process'].window_exists,
            extract=lambda ctx: ctx['process'].window_exists,
        ))
        
        self.assertions.append(Assertion(
            name="vulkan_initialized",
            description="Vulkan 设备初始化成功",
            severity=AssertionSeverity.CRITICAL,
            check=lambda ctx: ctx['log'].vulkan_initialized,
            extract=lambda ctx: ctx['log'].vulkan_initialized,
        ))
        
        self.assertions.append(Assertion(
            name="window_size",
            description="窗口尺寸为 1280x720",
            severity=AssertionSeverity.CRITICAL,
            check=lambda ctx: (
                ctx['process'].window_width == 1280 and
                ctx['process'].window_height == 720
            ),
            extract=lambda ctx: (
                f"{ctx['process'].window_width}x{ctx['process'].window_height}"
            ),
        ))
        
        # === WARNING 断言（失败触发视觉复核） ===
        
        self.assertions.append(Assertion(
            name="title_bar_has_pos",
            description="标题栏包含坐标信息",
            severity=AssertionSeverity.WARNING,
            check=lambda ctx: ctx['title'].pos_x is not None,
            extract=lambda ctx: ctx['title'].raw_text,
        ))
        
        self.assertions.append(Assertion(
            name="spawn_within_radius",
            description="出生点在 spawn 半径内 (r=2.5)",
            severity=AssertionSeverity.WARNING,
            check=lambda ctx: (
                ctx['log'].spawn_pos is not None and
                (ctx['log'].spawn_pos[0]**2 + ctx['log'].spawn_pos[2]**2) <= 2.5**2
            ),
            extract=lambda ctx: ctx['log'].spawn_pos,
        ))
        
        self.assertions.append(Assertion(
            name="network_connected",
            description="客户端已连接服务器 (标题栏非 disconnected)",
            severity=AssertionSeverity.WARNING,
            check=lambda ctx: ctx['title'].connected is True,
            extract=lambda ctx: ctx['title'].connected,
        ))
        
        self.assertions.append(Assertion(
            name="entity_update_received",
            description="收到实体同步消息 (ENTITY_UPDATE)",
            severity=AssertionSeverity.WARNING,
            check=lambda ctx: ctx['log'].entity_update_received,
            extract=lambda ctx: ctx['log'].entity_update_received,
        ))
        
        self.assertions.append(Assertion(
            name="visible_entities_positive",
            description="可见实体数 > 0 (连接后)",
            severity=AssertionSeverity.WARNING,
            check=lambda ctx: (
                ctx['title'].connected and
                ctx['title'].visible is not None and
                ctx['title'].visible > 0
            ),
            extract=lambda ctx: ctx['title'].visible,
        ))
        
        self.assertions.append(Assertion(
            name="no_network_error",
            description="无网络错误日志",
            severity=AssertionSeverity.WARNING,
            check=lambda ctx: not ctx['log'].network_error,
            extract=lambda ctx: ctx['log'].network_error,
        ))
        
        # === INFO 断言（仅记录） ===
        
        self.assertions.append(Assertion(
            name="title_bar_keys_responsive",
            description="标题栏 keys 字段存在（输入系统工作）",
            severity=AssertionSeverity.INFO,
            check=lambda ctx: ctx['title'].keys is not None,
            extract=lambda ctx: ctx['title'].keys,
        ))
    
    def run_all(self, context: dict) -> list[AssertionReport]:
        """执行所有断言"""
        reports = []
        for assertion in self.assertions:
            try:
                passed = assertion.check(context)
                actual = assertion.extract(context)
            except Exception as e:
                passed = False
                actual = f"ERROR: {e}"
            
            reports.append(AssertionReport(
                name=assertion.name,
                description=assertion.description,
                severity=assertion.severity,
                result=AssertionResult.PASSED if passed else AssertionResult.FAILED,
                actual_value=actual,
                expected=assertion.description,
                timestamp=time.time(),
            ))
        
        return reports
    
    def should_trigger_vision_review(self, reports: list[AssertionReport]) -> bool:
        """根据断言结果决定是否触发视觉复核
        
        触发条件：
        - 无 CRITICAL 失败（CRITICAL 失败直接 FAILED，不需视觉复核）
        - 有 WARNING 失败
        """
        has_critical_fail = any(
            r.result == AssertionResult.FAILED and r.severity == AssertionSeverity.CRITICAL
            for r in reports
        )
        has_warning_fail = any(
            r.result == AssertionResult.FAILED and r.severity == AssertionSeverity.WARNING
            for r in reports
        )
        return has_warning_fail and not has_critical_fail
    
    def overall_result(self, reports: list[AssertionReport]) -> AssertionResult:
        """总体判定"""
        for r in reports:
            if r.result == AssertionResult.FAILED:
                if r.severity == AssertionSeverity.CRITICAL:
                    return AssertionResult.FAILED
        # 无 CRITICAL 失败 → 交给视觉复核决定
        return AssertionResult.PASSED
```

#### 4.3.3 采样策略

```python
class SamplingStrategy:
    """采样策略——控制数据采集频率"""
    
    # 标题栏采样间隔（秒）
    TITLE_BAR_INTERVAL = 1.0      # 每秒一次（与标题栏更新逻辑匹配）
    
    # 日志采样间隔（秒）
    LOG_INTERVAL = 2.0            # 每2秒读一次新行
    
    # 进程/窗口检查间隔（秒）
    PROCESS_INTERVAL = 1.0        # 每秒一次
    
    # 总采样持续时间（秒）
    SAMPLE_DURATION = 10.0        # 默认采样10秒
    
    # 最小采样次数
    MIN_SAMPLES = 5               # 至少采集5次才判定
    
    # 截图触发后的额外采样时间（秒）
    POST_SCREENSHOT_DURATION = 3.0  # 截图后再采样3秒看是否恢复
```

### 4.4 截图复核机制

#### 4.4.1 截图触发条件

截图**仅在**以下情况触发：

1. **结构化断言部分失败**（WARNING 级断言失败，无 CRITICAL 失败）
2. **用户显式请求**（CLI `--screenshot` 模式）
3. **AI 主动触发**（AI 分析后认为需要视觉确认）

**不触发截图的情况：**

- 所有断言全通过（PASS，无需视觉确认）
- CRITICAL 断言失败（进程崩溃/窗口不存在，截图无意义）

#### 4.4.2 截图采集

```python
import subprocess
import os
from datetime import datetime

class ScreenshotCapture:
    """截图采集器"""
    
    def __init__(self, screenshot_dir: str = "screenshots"):
        self.screenshot_dir = screenshot_dir
        os.makedirs(screenshot_dir, exist_ok=True)
    
    def capture_by_window_id(self, window_id: str) -> str | None:
        """通过窗口ID截图
        
        优先使用 import (ImageMagick)，备选 xwd + convert
        """
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
        output_path = os.path.join(self.screenshot_dir, f"client_{timestamp}.png")
        
        # 方案1: ImageMagick import（最简单）
        try:
            result = subprocess.run(
                ['import', '-window', window_id, output_path],
                capture_output=True, timeout=5
            )
            if result.returncode == 0 and os.path.exists(output_path):
                return output_path
        except (subprocess.TimeoutExpired, FileNotFoundError):
            pass
        
        # 方案2: xwd → convert (netpbm/imagemagick)
        xwd_path = output_path.replace('.png', '.xwd')
        try:
            result = subprocess.run(
                ['xwd', '-id', window_id, '-out', xwd_path],
                capture_output=True, timeout=5
            )
            if result.returncode == 0:
                subprocess.run(
                    ['convert', xwd_path, output_path],
                    capture_output=True, timeout=5
                )
                os.remove(xwd_path)
                if os.path.exists(output_path):
                    return output_path
        except (subprocess.TimeoutExpired, FileNotFoundError):
            pass
        
        # 方案3: Xvfb + xwd（CI 环境，虚拟显示）
        try:
            result = subprocess.run(
                ['xwd', '-root', '-out', xwd_path],
                capture_output=True, timeout=5,
                env={**os.environ, 'DISPLAY': os.environ.get('DISPLAY', ':99')}
            )
            if result.returncode == 0:
                subprocess.run(
                    ['convert', xwd_path, output_path],
                    capture_output=True, timeout=5
                )
                os.remove(xwd_path) if os.path.exists(xwd_path) else None
                if os.path.exists(output_path):
                    return output_path
        except Exception:
            pass
        
        return None
    
    def capture_full_screen(self) -> str | None:
        """全屏截图（备用方案）"""
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
        output_path = os.path.join(self.screenshot_dir, f"fullscreen_{timestamp}.png")
        
        try:
            subprocess.run(
                ['import', '-window', 'root', output_path],
                capture_output=True, timeout=5
            )
            if os.path.exists(output_path):
                return output_path
        except Exception:
            pass
        return None
```

#### 4.4.3 视觉模型判定

```python
from dataclasses import dataclass
from enum import Enum

class VisionVerdict(Enum):
    NORMAL = "normal"          # 渲染正常
    BLACK_SCREEN = "black"     # 黑屏
    WHITE_SCREEN = "white"     # 白屏
    GARBLED = "garbled"        # 花屏/乱码
    NO_CONTENT = "no_content"  # 窗口存在但无渲染内容
    ERROR = "error"            # 截图失败或模型异常

@dataclass
class VisionReviewResult:
    verdict: VisionVerdict
    confidence: float           # 0.0 ~ 1.0
    description: str            # 详细描述
    issues: list[str]           # 发现的问题列表
    screenshot_path: str        # 截图路径

class VisionReviewer:
    """视觉复核器——将截图送视觉模型判定"""
    
    # 视觉判定 prompt 模板
    REVIEW_PROMPT = """你是一个游戏引擎渲染验证专家。
这是 ChaosEngine Vulkan 客户端的截图。请检查以下方面并给出判定：

1. 窗口内是否有渲染内容（不是纯黑/纯白）？
2. 是否能看到三角形或球体等 3D 渲染图元？
3. 画面是否有明显的花屏/撕裂/乱码？
4. 渲染内容是否在窗口区域内（没有溢出或错位）？

请以 JSON 格式返回：
{
  "verdict": "normal" | "black" | "white" | "garbled" | "no_content",
  "confidence": 0.0-1.0,
  "description": "简要描述你看到的内容",
  "issues": ["问题1", "问题2"]
}

当前上下文信息（结构化断言部分失败）：
- 失败的断言: {failed_assertions}
- 标题栏数据: {title_bar_text}
- 客户端日志摘要: {log_summary}
"""
    
    def __init__(self, ark_client: "ArkClient"):
        self.ark_client = ark_client
    
    async def review(
        self,
        screenshot_path: str,
        failed_assertions: list[dict],
        title_bar_text: str,
        log_summary: str,
    ) -> VisionReviewResult:
        """送截图给视觉模型复核"""
        
        prompt = self.REVIEW_PROMPT.format(
            failed_assertions=failed_assertions,
            title_bar_text=title_bar_text,
            log_summary=log_summary,
        )
        
        # 调用视觉模型（ARK API，图片 + 文本输入）
        response = await self.ark_client.vision_chat(
            image_path=screenshot_path,
            prompt=prompt,
            model="glm-5-2-260617",  # 使用通用旗舰模型做视觉判定
        )
        
        # 解析 JSON 响应
        try:
            import json
            result = json.loads(response)
            return VisionReviewResult(
                verdict=VisionVerdict(result.get("verdict", "error")),
                confidence=float(result.get("confidence", 0.0)),
                description=result.get("description", ""),
                issues=result.get("issues", []),
                screenshot_path=screenshot_path,
            )
        except (json.JSONDecodeError, ValueError) as e:
            return VisionReviewResult(
                verdict=VisionVerdict.ERROR,
                confidence=0.0,
                description=f"视觉模型响应解析失败: {e}",
                issues=[],
                screenshot_path=screenshot_path,
            )
```

#### 4.4.4 vision provider 对接方案

LoopEngine 需要对接视觉模型来判定截图。对接方案如下：

**方案 A：ARK API 视觉接口（推荐）**

```python
class ArkVisionProvider:
    """ARK 视觉模型 provider"""
    
    def __init__(self, api_key: str, base_url: str):
        self.api_key = api_key
        self.base_url = base_url
    
    async def vision_chat(
        self, image_path: str, prompt: str, model: str
    ) -> str:
        """调用 ARK 视觉接口
        
        ARK API 兼容 OpenAI Vision API 格式：
        POST /v1/chat/completions
        {
          "model": "glm-5-2-260617",
          "messages": [{
            "role": "user",
            "content": [
              {"type": "text", "text": prompt},
              {"type": "image_url", "image_url": {
                "url": "data:image/png;base64,{base64_encoded_image}"
              }}
            ]
          }]
        }
        """
        import base64
        import httpx
        
        with open(image_path, 'rb') as f:
            image_b64 = base64.b64encode(f.read()).decode('utf-8')
        
        async with httpx.AsyncClient(timeout=30) as client:
            resp = await client.post(
                f"{self.base_url}/v1/chat/completions",
                headers={
                    "Authorization": f"Bearer {self.api_key}",
                    "Content-Type": "application/json",
                },
                json={
                    "model": model,
                    "messages": [{
                        "role": "user",
                        "content": [
                            {"type": "text", "text": prompt},
                            {"type": "image_url", "image_url": {
                                "url": f"data:image/png;base64,{image_b64}"
                            }}
                        ]
                    }],
                    "max_tokens": 500,
                    "temperature": 0.1,  # 低温度保证判定稳定性
                }
            )
            resp.raise_for_status()
            return resp.json()["choices"][0]["message"]["content"]
```

**方案 B：本地视觉模型（备选，离线场景）**

当网络不可用或成本敏感时，使用本地轻量级视觉检查：

```python
class LocalVisionProvider:
    """本地视觉检查（无 AI，纯像素分析）"""
    
    def check_image(self, image_path: str) -> VisionReviewResult:
        """本地像素级检查"""
        from PIL import Image
        import numpy as np
        
        img = Image.open(image_path)
        arr = np.array(img)
        
        # 检查黑屏
        if arr.mean() < 5:
            return VisionReviewResult(
                verdict=VisionVerdict.BLACK_SCREEN,
                confidence=0.95,
                description="画面平均亮度极低，判定为黑屏",
                issues=["black_screen"],
                screenshot_path=image_path,
            )
        
        # 检查白屏
        if arr.mean() > 250:
            return VisionReviewResult(
                verdict=VisionVerdict.WHITE_SCREEN,
                confidence=0.95,
                description="画面平均亮度极高，判定为白屏",
                issues=["white_screen"],
                screenshot_path=image_path,
            )
        
        # 检查颜色方差（花屏/纯色）
        std = arr.std()
        if std < 2:
            return VisionReviewResult(
                verdict=VisionVerdict.NO_CONTENT,
                confidence=0.80,
                description="画面颜色方差极低，可能为纯色无内容",
                issues=["low_variance"],
                screenshot_path=image_path,
            )
        
        # 正常情况
        return VisionReviewResult(
            verdict=VisionVerdict.NORMAL,
            confidence=0.70,  # 本地检查置信度较低
            description=f"画面平均亮度 {arr.mean():.1f}，方差 {std:.1f}，有渲染内容",
            issues=[],
            screenshot_path=image_path,
        )
```

**方案选择策略：**

```python
class VisionProviderSelector:
    """视觉 provider 选择器"""
    
    def __init__(self, config: dict):
        self.config = config
    
    def select(self) -> VisionReviewer:
        """根据配置选择 provider"""
        mode = self.config.get('vision', {}).get('mode', 'ark')
        
        if mode == 'ark':
            return VisionReviewer(ArkVisionProvider(
                api_key=self.config['ark']['api_key'],
                base_url=self.config['ark']['base_url'],
            ))
        elif mode == 'local':
            return LocalVisionProvider()
        elif mode == 'hybrid':
            # 混合模式：先用本地快速检查，不确定时再调 ARK
            return HybridVisionProvider(
                local=LocalVisionProvider(),
                ark=ArkVisionProvider(
                    api_key=self.config['ark']['api_key'],
                    base_url=self.config['ark']['base_url'],
                ),
            )
        else:
            raise ValueError(f"未知 vision mode: {mode}")
```

### 4.5 两层验证完整流程

```python
class ClientUIVerificationFlow:
    """客户端界面验证完整流程"""
    
    def __init__(
        self,
        collector: ClientDataCollector,
        assertion_engine: ClientAssertionEngine,
        screenshot_capture: ScreenshotCapture,
        vision_reviewer: VisionReviewer,
        sampling_config: SamplingStrategy,
    ):
        self.collector = collector
        self.assertion_engine = assertion_engine
        self.screenshot_capture = screenshot_capture
        self.vision_reviewer = vision_reviewer
        self.sampling_config = sampling_config
    
    async def run(self, client_process: subprocess.Popen) -> LoopResult:
        """执行完整验证流程"""
        import asyncio
        
        context = {
            'process_pid': client_process.pid,
            'title': None,
            'log': None,
            'process': None,
        }
        
        all_log_lines: list[str] = []
        title_snapshots: list[TitleBarSnapshot] = []
        
        # ── 阶段1：数据采集 ──
        start_time = time.time()
        while time.time() - start_time < self.sampling_config.SAMPLE_DURATION:
            # 采集标题栏
            title_snap = self.collector.collect_title_bar()
            title_snapshots.append(title_snap)
            context['title'] = title_snap
            
            # 采集进程/窗口
            proc_snap = self.collector.collect_process()
            context['process'] = proc_snap
            
            # 检查进程是否已退出
            if not proc_snap.process_alive:
                break
            
            await asyncio.sleep(self.sampling_config.TITLE_BAR_INTERVAL)
        
        # 采集最终日志
        context['log'] = self.collector.collect_log(all_log_lines)
        
        # ── 阶段2：结构化断言 ──
        assertion_reports = self.assertion_engine.run_all(context)
        overall = self.assertion_engine.overall_result(assertion_reports)
        
        # ── 阶段3：判定 ──
        if overall == AssertionResult.PASSED:
            # 全部 CRITICAL + WARNING 断言通过
            return LoopResult(
                status=LoopStatus.PASSED,
                retry_count=0,
                duration_sec=time.time() - start_time,
                artifacts={'title_snapshots': title_snapshots},
                assertions=[r.__dict__ for r in assertion_reports],
                ai_analysis=None,
                error=None,
            )
        
        # 检查是否需要视觉复核
        if not self.assertion_engine.should_trigger_vision_review(assertion_reports):
            # CRITICAL 断言失败，直接 FAILED
            failed_critical = [
                r.name for r in assertion_reports
                if r.result == AssertionResult.FAILED
                and r.severity == AssertionSeverity.CRITICAL
            ]
            return LoopResult(
                status=LoopStatus.FAILED,
                retry_count=0,
                duration_sec=time.time() - start_time,
                artifacts={'title_snapshots': title_snapshots},
                assertions=[r.__dict__ for r in assertion_reports],
                ai_analysis=f"CRITICAL 断言失败: {failed_critical}",
                error=f"Critical assertions failed: {failed_critical}",
            )
        
        # ── 阶段4：截图视觉复核 ──
        window_id = context['process'].window_id
        if not window_id:
            # 无法获取窗口ID，全屏截图
            screenshot_path = self.screenshot_capture.capture_full_screen()
        else:
            screenshot_path = self.screenshot_capture.capture_by_window_id(window_id)
        
        if not screenshot_path:
            return LoopResult(
                status=LoopStatus.FAILED,
                retry_count=0,
                duration_sec=time.time() - start_time,
                artifacts={'title_snapshots': title_snapshots},
                assertions=[r.__dict__ for r in assertion_reports],
                ai_analysis="截图失败，无法进行视觉复核",
                error="Screenshot capture failed",
            )
        
        # 准备视觉复核上下文
        failed_assertions = [
            {'name': r.name, 'description': r.description, 'actual': str(r.actual_value)}
            for r in assertion_reports
            if r.result == AssertionResult.FAILED
        ]
        
        vision_result = await self.vision_reviewer.review(
            screenshot_path=screenshot_path,
            failed_assertions=failed_assertions,
            title_bar_text=context['title'].raw_text if context['title'] else "",
            log_summary=f"vulkan_init={context['log'].vulkan_initialized}, "
                       f"entity_update={context['log'].entity_update_received}",
        )
        
        # ── 阶段5：综合判定 ──
        if vision_result.verdict == VisionVerdict.NORMAL:
            # 视觉确认正常 → 结构化断言为误报
            return LoopResult(
                status=LoopStatus.PASSED,
                retry_count=0,
                duration_sec=time.time() - start_time,
                artifacts={
                    'title_snapshots': title_snapshots,
                    'screenshot': screenshot_path,
                    'vision_result': vision_result.__dict__,
                },
                assertions=[r.__dict__ for r in assertion_reports],
                ai_analysis=f"结构化断言部分失败，但视觉复核确认渲染正常 "
                           f"(confidence={vision_result.confidence})。"
                           f"失败的断言: {failed_assertions}",
                error=None,
            )
        else:
            # 视觉确认异常
            return LoopResult(
                status=LoopStatus.FAILED,
                retry_count=0,
                duration_sec=time.time() - start_time,
                artifacts={
                    'title_snapshots': title_snapshots,
                    'screenshot': screenshot_path,
                    'vision_result': vision_result.__dict__,
                },
                assertions=[r.__dict__ for r in assertion_reports],
                ai_analysis=f"视觉复核确认渲染异常: {vision_result.verdict}. "
                           f"描述: {vision_result.description}. "
                           f"问题: {vision_result_issues}. "
                           f"失败断言: {failed_assertions}",
                error=f"Vision review failed: {vision_result.verdict}",
            )
```

### 4.6 输入模拟（可选，v0.2）

v0.1 仅做被动观察（不模拟按键），v0.2 增加输入模拟验证交互响应：

```python
class InputSimulator:
    """输入模拟器（v0.2）"""
    
    async def press_key(self, key: str, duration: float = 0.1):
        """模拟按键
        
        使用 xdotool:
          xdotool keydown W; sleep 0.1; xdotool keyup W
        """
        subprocess.run(['xdotool', 'keydown', key])
        await asyncio.sleep(duration)
        subprocess.run(['xdotool', 'keyup', key])
    
    async def test_movement_response(self, window_id: str) -> bool:
        """测试按键移动是否响应
        
        1. 记录当前标题栏坐标
        2. 按住 W 键 1 秒
        3. 检查标题栏 z 坐标是否变化
        """
        # ... 实现略
        pass
```

---

## 5. AI 调度层

### 5.1 模型池与路由策略

#### 5.1.1 ARK 可用模型

| 模型 | 标识 | 输入价格 | 输出价格 | 定位 | 适用场景 |
|------|------|----------|----------|------|----------|
| deepseek-v4-flash-260425 | `flash` | ¥1/M | ¥2/M | 最省 | 日常任务、简单分析、日志解析 |
| glm-5-2-260617 | `glm` | ¥8/M | ¥28/M | 通用旗舰 | 复杂分析、代码审查、视觉判定 |
| deepseek-v4-pro-260425 | `pro` | ¥12/M | ¥24/M | 顶尖推理 | 深度推理、架构设计、根因分析 |
| qwen3-14b-20250429 | `qwen` | - | - | 14B备选 | 备选/降级方案 |

#### 5.1.2 模型路由规则

```python
from enum import Enum
from dataclasses import dataclass

class TaskComplexity(Enum):
    SIMPLE = "simple"        # 日志解析、简单替换、格式检查
    MODERATE = "moderate"    # 编译错误分析、测试失败分析、代码审查
    COMPLEX = "complex"      # 架构设计、根因分析、多文件修改
    VISION = "vision"        # 截图视觉判定

@dataclass
class RoutingRule:
    complexity: TaskComplexity
    primary_model: str
    fallback_model: str
    max_tokens: int
    temperature: float

ROUTING_RULES: dict[str, RoutingRule] = {
    # ── 开发闭环 ──
    'dev.requirement_analysis': RoutingRule(
        TaskComplexity.COMPLEX, 'glm', 'pro', 4096, 0.3
    ),
    'dev.code_generation': RoutingRule(
        TaskComplexity.COMPLEX, 'pro', 'glm', 8192, 0.2
    ),
    'dev.simple_edit': RoutingRule(
        TaskComplexity.SIMPLE, 'flash', 'glm', 2048, 0.1
    ),
    'dev.compile_error_analysis': RoutingRule(
        TaskComplexity.MODERATE, 'glm', 'pro', 4096, 0.2
    ),
    
    # ── 测试闭环 ──
    'test.failure_analysis': RoutingRule(
        TaskComplexity.MODERATE, 'glm', 'pro', 4096, 0.2
    ),
    'test.lua_lint_fix': RoutingRule(
        TaskComplexity.SIMPLE, 'flash', 'glm', 1024, 0.1
    ),
    'test.coverage_suggestion': RoutingRule(
        TaskComplexity.COMPLEX, 'pro', 'glm', 4096, 0.3
    ),
    
    # ── 验证闭环 ──
    'verify.cluster_startup_analysis': RoutingRule(
        TaskComplexity.MODERATE, 'glm', 'pro', 4096, 0.2
    ),
    'verify.data_consistency': RoutingRule(
        TaskComplexity.COMPLEX, 'pro', 'glm', 4096, 0.2
    ),
    'verify.api_validation': RoutingRule(
        TaskComplexity.SIMPLE, 'flash', 'glm', 2048, 0.1
    ),
    
    # ── 客户端界面闭环 ──
    'client.title_bar_analysis': RoutingRule(
        TaskComplexity.SIMPLE, 'flash', 'glm', 1024, 0.1
    ),
    'client.vision_review': RoutingRule(
        TaskComplexity.VISION, 'glm', 'pro', 1024, 0.1
    ),
    'client.render_error_analysis': RoutingRule(
        TaskComplexity.COMPLEX, 'pro', 'glm', 4096, 0.2
    ),
}

class ModelRouter:
    """模型路由器"""
    
    def __init__(self, ark_client: "ArkClient", rules: dict = ROUTING_RULES):
        self.ark_client = ark_client
        self.rules = rules
    
    def route(self, task_type: str) -> RoutingRule:
        """根据任务类型路由到合适的模型"""
        if task_type not in self.rules:
            # 默认使用 glm
            return RoutingRule(
                TaskComplexity.MODERATE, 'glm', 'pro', 4096, 0.2
            )
        return self.rules[task_type]
    
    async def execute(
        self, task_type: str, prompt: str, context: dict = None
    ) -> str:
        """执行 AI 任务，自动路由模型"""
        rule = self.route(task_type)
        
        try:
            response = await self.ark_client.chat(
                model=self._model_id(rule.primary_model),
                prompt=prompt,
                max_tokens=rule.max_tokens,
                temperature=rule.temperature,
                context=context,
            )
            return response
        except Exception as e:
            # 主模型失败，降级到 fallback
            if rule.fallback_model != rule.primary_model:
                response = await self.ark_client.chat(
                    model=self._model_id(rule.fallback_model),
                    prompt=prompt,
                    max_tokens=rule.max_tokens,
                    temperature=rule.temperature,
                    context=context,
                )
                return response
            raise
    
    def _model_id(self, alias: str) -> str:
        """别名 → 完整模型 ID"""
        MODEL_MAP = {
            'flash': 'deepseek-v4-flash-260425',
            'glm': 'glm-5-2-260617',
            'pro': 'deepseek-v4-pro-260425',
            'qwen': 'qwen3-14b-20250429',
        }
        return MODEL_MAP.get(alias, alias)
```

#### 5.1.3 成本控制

```python
@dataclass
class CostBudget:
    """成本预算控制"""
    daily_budget_cny: float = 10.0        # 每日预算 ¥10
    per_loop_budget_cny: float = 2.0      # 单次闭环预算 ¥2
    vision_daily_limit: int = 50          # 视觉模型每日最多 50 次
    flash_daily_limit: int = 500          # flash 每日最多 500 次
    
    # 运行时状态
    daily_spent: float = 0.0
    vision_count_today: int = 0
    flash_count_today: int = 0
    
    def can_spend(self, estimated_cost: float, model: str) -> bool:
        """检查是否还能消费"""
        if self.daily_spent + estimated_cost > self.daily_budget_cny:
            return False
        if model == 'glm' and self.vision_count_today >= self.vision_daily_limit:
            return False
        if model == 'flash' and self.flash_count_today >= self.flash_daily_limit:
            return False
        return True
    
    def estimate_cost(self, model: str, input_tokens: int, output_tokens: int) -> float:
        """估算单次调用成本（元）"""
        PRICING = {
            'flash': (1.0, 2.0),    # (input/M, output/M)
            'glm': (8.0, 28.0),
            'pro': (12.0, 24.0),
            'qwen': (0.0, 0.0),     # 自部署
        }
        in_price, out_price = PRICING.get(model, (8.0, 28.0))
        return (input_tokens / 1_000_000 * in_price +
                output_tokens / 1_000_000 * out_price)
```

### 5.2 任务拆解器

```python
class TaskSplitter:
    """任务拆解器——将复杂任务拆为子任务"""
    
    def __init__(self, router: ModelRouter):
        self.router = router
    
    async def split(
        self, task_description: str, context: dict
    ) -> list["SubTask"]:
        """拆解任务
        
        示例:
          输入: "为 ChaosEngine 添加 AOI 范围查询功能"
          输出: [
            SubTask("修改 ce_aoi.h 添加 ce_aoi_query_range 声明", 'dev.code_generation'),
            SubTask("实现 ce_aoi_query_range 函数", 'dev.code_generation'),
            SubTask("添加 test_aoi_range 测试用例", 'dev.code_generation'),
            SubTask("编译验证", 'dev.compile_check'),
            SubTask("运行测试", 'test.run'),
          ]
        """
        prompt = f"""你是一个游戏引擎开发任务拆解专家。
请将以下任务拆解为可独立执行的子任务列表。

任务描述: {task_description}

上下文:
- 项目: ChaosEngine（纯C内核 + Lua脚本的游戏引擎）
- 代码目录: {context.get('project_dir', 'chaos-engine/')}
- 相关 spec: {context.get('spec_path', 'N/A')}

要求:
1. 每个子任务可以在一个 AI agent session 内完成
2. 标注每个子任务的类型（code_generation/simple_edit/compile_check/test_run）
3. 标注子任务之间的依赖关系
4. 子任务粒度适中（不过细也不过粗）

以 JSON 数组格式返回:
[{{"description": "...", "task_type": "...", "depends_on": [0, 2]}}]
"""
        response = await self.router.execute(
            'dev.requirement_analysis', prompt, context
        )
        
        import json
        try:
            tasks_data = json.loads(response)
            return [
                SubTask(
                    description=t['description'],
                    task_type=t['task_type'],
                    depends_on=t.get('depends_on', []),
                )
                for t in tasks_data
            ]
        except json.JSONDecodeError:
            # 降级：单任务
            return [SubTask(description=task_description, task_type='dev.code_generation')]
```

### 5.3 子 agent 编排器

```python
import asyncio
from dataclasses import dataclass, field

@dataclass
class SubTask:
    description: str
    task_type: str
    depends_on: list[int] = field(default_factory=list)
    status: str = "pending"    # pending / running / done / failed
    result: str | None = None

class SubAgentOrchestrator:
    """子 agent 编排器"""
    
    def __init__(self, router: ModelRouter, max_parallel: int = 3):
        self.router = router
        self.max_parallel = max_parallel
    
    async def execute_tasks(self, tasks: list[SubTask]) -> list[SubTask]:
        """按依赖关系执行子任务"""
        completed: set[int] = set()
        results: list[SubTask] = list(tasks)
        
        while len(completed) < len(tasks):
            # 找出可执行的任务（依赖已完成）
            runnable = [
                i for i, t in enumerate(tasks)
                if i not in completed
                and t.status == "pending"
                and all(d in completed for d in t.depends_on)
            ]
            
            if not runnable:
                # 死锁检测
                remaining = [i for i, t in enumerate(tasks) if i not in completed]
                if remaining:
                    raise RuntimeError(f"任务依赖死锁: {remaining}")
                break
            
            # 并行执行（受 max_parallel 限制）
            batch = runnable[:self.max_parallel]
            
            async def run_one(idx: int):
                tasks[idx].status = "running"
                try:
                    result = await self._execute_single(tasks[idx])
                    tasks[idx].result = result
                    tasks[idx].status = "done"
                    completed.add(idx)
                except Exception as e:
                    tasks[idx].result = str(e)
                    tasks[idx].status = "failed"
                    completed.add(idx)  # 失败也标记完成，让其他任务继续
            
            await asyncio.gather(*[run_one(i) for i in batch])
        
        return results
    
    async def _execute_single(self, task: SubTask) -> str:
        """执行单个子任务"""
        # 根据 task_type 路由到不同处理逻辑
        if task.task_type == 'dev.code_generation':
            return await self.router.execute(
                'dev.code_generation',
                f"请执行以下开发任务:\n{task.description}\n"
                f"返回代码变更（unified diff 格式）。",
            )
        elif task.task_type == 'dev.simple_edit':
            return await self.router.execute(
                'dev.simple_edit',
                f"请执行以下简单修改:\n{task.description}\n"
                f"返回 unified diff。",
            )
        elif task.task_type == 'dev.compile_check':
            # 不调 AI，直接编译
            return "compile_check: handled by ScriptExec"
        elif task.task_type == 'test.run':
            return "test_run: handled by ScriptExec"
        else:
            return await self.router.execute(
                'dev.requirement_analysis',
                task.description,
            )
```

### 5.4 ARK API 客户端

```python
import httpx
from typing import Optional

class ArkClient:
    """ARK API 客户端"""
    
    def __init__(
        self,
        api_key: str,
        base_url: str = "https://ark.cn-beijing.volces.com/api",
    ):
        self.api_key = api_key
        self.base_url = base_url
        self._client: Optional[httpx.AsyncClient] = None
    
    async def _get_client(self) -> httpx.AsyncClient:
        if self._client is None or self._client.is_closed:
            self._client = httpx.AsyncClient(
                base_url=self.base_url,
                headers={
                    "Authorization": f"Bearer {self.api_key}",
                    "Content-Type": "application/json",
                },
                timeout=60,
            )
        return self._client
    
    async def chat(
        self,
        model: str,
        prompt: str,
        max_tokens: int = 4096,
        temperature: float = 0.2,
        context: dict = None,
    ) -> str:
        """文本对话"""
        client = await self._get_client()
        
        messages = []
        if context and context.get('system_prompt'):
            messages.append({
                "role": "system",
                "content": context['system_prompt']
            })
        messages.append({"role": "user", "content": prompt})
        
        resp = await client.post("/v1/chat/completions", json={
            "model": model,
            "messages": messages,
            "max_tokens": max_tokens,
            "temperature": temperature,
        })
        resp.raise_for_status()
        return resp.json()["choices"][0]["message"]["content"]
    
    async def vision_chat(
        self,
        image_path: str,
        prompt: str,
        model: str = "glm-5-2-260617",
    ) -> str:
        """视觉对话（图片 + 文本）"""
        import base64
        
        with open(image_path, 'rb') as f:
            image_b64 = base64.b64encode(f.read()).decode('utf-8')
        
        client = await self._get_client()
        resp = await client.post("/v1/chat/completions", json={
            "model": model,
            "messages": [{
                "role": "user",
                "content": [
                    {"type": "text", "text": prompt},
                    {"type": "image_url", "image_url": {
                        "url": f"data:image/png;base64,{image_b64}"
                    }},
                ],
            }],
            "max_tokens": 500,
            "temperature": 0.1,
        })
        resp.raise_for_status()
        return resp.json()["choices"][0]["message"]["content"]
    
    async def close(self):
        if self._client and not self._client.is_closed:
            await self._client.aclose()
```

---

## 6. 与 ChaosEngine 现有基建的对接

### 6.1 脚本调用接口

LoopEngine 通过 `ScriptExec` 组件调用 ChaosEngine 的 24 个脚本，统一封装为结构化接口。

#### 6.1.1 脚本映射表

| 闭环域 | 脚本 | 用途 | 调用方式 | 关键判定 |
|--------|------|------|----------|----------|
| DevLoop | `build_and_test.sh --no-test` | 仅编译 | 同步等待 | exit_code==0 |
| TestLoop | `build_and_test.sh` | 编译+18测试+冒烟 | 同步等待 | exit_code==0 |
| TestLoop | (lua-lint 内联) | Lua 语法检查 | 同步等待 | 0 errors |
| VerifyLoop | `start_cluster.sh --all` | 启动全集群 | 异步启动 | status.sh 确认全部 RUNNING |
| VerifyLoop | `test_client.sh` | TCP/WS/Admin 冒烟 | 同步等待 | PASS/FAIL 统计 |
| VerifyLoop | `verify_client_sync.sh` | 客户端同步验证 | 同步等待 | exit_code==0 |
| VerifyLoop | `test_sync.sh` | 同步测试 | 同步等待 | exit_code==0 |
| VerifyLoop | `test_save.sh` | 存档测试 | 同步等待 | exit_code==0 |
| VerifyLoop | `test_dbproxy.sh` | DBProxy 测试 | 同步等待 | exit_code==0 |
| VerifyLoop | `joint_client_test.sh` | 联调测试 | 同步等待 | 日志含 ENTITY_UPDATE |
| VerifyLoop | `stop_cluster.sh` | 停止集群 | 同步等待 | 全部停止 |
| VerifyLoop | `status.sh --json` | 状态查询 | 同步查询 | JSON 解析 |
| ClientUILoop | `start_client.sh --vulkan` | 启动客户端 | 异步启动 | 进程存活+窗口存在 |
| ClientUILoop | `start_client.sh --vulkan --connect host:port` | 连接服务器 | 异步启动 | 标题栏 connected |
| ClientUILoop | `stop_cluster.sh` | 停止客户端 | 清理 | 进程退出 |
| 压测(可选) | `stress_client.py` | Python 压测 | 同步等待 | 吞吐量/错误率 |
| 压测(可选) | `stress_client_v2.py` | 多维度压测 | 同步等待 | 吞吐量/错误率 |
| 压测(可选) | `test_gateway_stress.sh` | Gateway 压测 | 同步等待 | exit_code==0 |
| 压测(可选) | `test_gateway_tcp.sh` | Gateway TCP 测试 | 同步等待 | exit_code==0 |
| 压测(可选) | `test_gateway_ws.sh` | Gateway WS 测试 | 同步等待 | exit_code==0 |
| 基准(可选) | `bench_async_io.py` | io_uring 基准 | 同步等待 | 性能数据采集 |

#### 6.1.2 ScriptExec 实现

```python
import subprocess
import os
from dataclasses import dataclass
from pathlib import Path

@dataclass
class ScriptResult:
    exit_code: int
    stdout: str
    stderr: str
    duration_sec: float
    success: bool

class ScriptExec:
    """ChaosEngine 脚本执行器"""
    
    def __init__(self, chaos_engine_dir: str):
        self.project_dir = Path(chaos_engine_dir)
        self.scripts_dir = self.project_dir / "scripts"
    
    def run(
        self,
        script_name: str,
        args: list[str] = None,
        timeout: int = 300,
        env_extra: dict = None,
    ) -> ScriptResult:
        """同步执行脚本"""
        script_path = self.scripts_dir / script_name
        if not script_path.exists():
            return ScriptResult(
                exit_code=-1, stdout="", stderr=f"脚本不存在: {script_path}",
                duration_sec=0, success=False,
            )
        
        cmd = ['bash', str(script_path)] + (args or [])
        
        env = {**os.environ, **(env_extra or {})}
        # 确保 PATH 包含必要的路径
        env.setdefault('DISPLAY', ':0')
        
        import time
        start = time.time()
        
        try:
            result = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=timeout,
                env=env,
                cwd=str(self.project_dir),
            )
            duration = time.time() - start
            return ScriptResult(
                exit_code=result.returncode,
                stdout=result.stdout,
                stderr=result.stderr,
                duration_sec=duration,
                success=result.returncode == 0,
            )
        except subprocess.TimeoutExpired as e:
            duration = time.time() - start
            return ScriptResult(
                exit_code=-1,
                stdout=e.stdout or "",
                stderr=f"超时({timeout}s): {e.stderr or ''}",
                duration_sec=duration,
                success=False,
            )
    
    def run_async(
        self,
        script_name: str,
        args: list[str] = None,
        env_extra: dict = None,
    ) -> subprocess.Popen:
        """异步执行脚本（不等待完成）"""
        script_path = self.scripts_dir / script_name
        cmd = ['bash', str(script_path)] + (args or [])
        
        env = {**os.environ, **(env_extra or {})}
        
        return subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
            cwd=str(self.project_dir),
        )
```

### 6.2 CI 集成

#### 6.2.1 现有 CI 结构

ChaosEngine 的 `.gitee-ci.yml` 定义了 5 个 CI job：

| Job | 用途 | LoopEngine 集成方式 |
|-----|------|---------------------|
| `build-and-test` | 编译+全量测试+冒烟+客户端同步验证 | LoopEngine 可触发此 job 并解析结果 |
| `release-build` | Release 编译验证 | 可选触发 |
| `lua-lint` | Lua 语法检查 | 结果解析 |
| `memcheck` | Valgrind 内存检查 | 结果解析 |

#### 6.2.2 CI 触发与结果解析

```python
class CITrigger:
    """CI 触发器——通过 Gitee API 触发和查询 CI"""
    
    def __init__(self, gitee_token: str, repo: str = "zhong-fangdao/chaos-engine"):
        self.gitee_token = gitee_token
        self.repo = repo
        self.api_base = "https://gitee.com/api/v5"
    
    async def trigger_pipeline(
        self, branch: str = "develop"
    ) -> dict:
        """触发 CI 流水线
        
        POST /repos/{owner}/{repo}/actions/runs
        """
        # Gitee Go API 触发流水线
        # ... 实现略
        pass
    
    async def get_pipeline_status(self, run_id: str) -> dict:
        """查询流水线状态"""
        # GET /repos/{owner}/{repo}/actions/runs/{run_id}
        # ... 实现略
        pass
    
    async def parse_test_results(self, run_id: str) -> dict:
        """解析 CI 测试结果"""
        # 获取 CI 日志，解析测试通过/失败数
        # ... 实现略
        pass
```

#### 6.2.3 LoopEngine 自身作为 CI 步骤

LoopEngine 可以作为 ChaosEngine CI 的一个额外步骤：

```yaml
# .gitee-ci.yml 新增 job（由 LoopEngine 维护，不修改原有 job）
loop-engine-verify:
  name: LoopEngine 全流程验证
  runs-on: ubuntu-latest
  timeout-minutes: 30
  needs: build-and-test
  steps:
    - name: 检出代码
      uses: actions/checkout@v4
    
    - name: 安装 LoopEngine
      run: pip install loopengine
    
    - name: 运行 LoopEngine 全流程
      env:
        ARK_API_KEY: ${{ secrets.ARK_API_KEY }}
        DISPLAY: :99
      run: |
        # 启动虚拟显示
        Xvfb :99 -screen 0 1280x720x24 &
        sleep 1
        # 运行 LoopEngine
        loopengine run --config config/loop.yaml --report report.md
    
    - name: 上传报告
      uses: actions/upload-artifact@v3
      with:
        name: loop-engine-report
        path: report.md
```

### 6.3 Spec 体系联动

#### 6.3.1 OpenSpec 联动

ChaosEngine 使用 OpenSpec 四阶段流程（propose → specs → design → tasks），LoopEngine 可以：

1. **读取 proposal**：了解变更意图，作为开发闭环的输入
2. **读取 specs**：提取验收标准，作为断言规则
3. **读取 design**：了解架构决策，辅助 AI 分析
4. **读取 tasks**：将 task 列表作为子任务输入

```python
class SpecLink:
    """Spec 体系联动"""
    
    def __init__(self, chaos_engine_dir: str):
        self.project_dir = Path(chaos_engine_dir)
        self.openspec_dir = self.project_dir / "openspec"
        self.docs_spec_dir = self.project_dir / "docs" / "spec"
    
    def list_changes(self) -> list[dict]:
        """列出所有 openspec changes"""
        changes = []
        changes_dir = self.openspec_dir / "changes"
        if not changes_dir.exists():
            return changes
        
        for change_dir in changes_dir.iterdir():
            if not change_dir.is_dir():
                continue
            proposal_file = change_dir / "proposal.md"
            if proposal_file.exists():
                changes.append({
                    'name': change_dir.name,
                    'path': str(change_dir),
                    'proposal': proposal_file.read_text(encoding='utf-8'),
                })
        return changes
    
    def get_spec_requirements(self, spec_name: str) -> list[dict]:
        """从 openspec specs 提取验收标准"""
        # 查找所有 changes 下的 spec 文件
        requirements = []
        for change_dir in (self.openspec_dir / "changes").iterdir():
            spec_file = change_dir / "specs" / spec_name / "spec.md"
            if spec_file.exists():
                content = spec_file.read_text(encoding='utf-8')
                # 解析 SHALL/MUST 语句作为验收标准
                import re
                shalls = re.findall(
                    r'(?:SHALL|MUST|应当|必须)(.+?)(?:\n\n|\n##|\Z)',
                    content, re.DOTALL
                )
                requirements.extend([{
                    'change': change_dir.name,
                    'spec': spec_name,
                    'requirement': s.strip(),
                } for s in shalls])
        return requirements
    
    def get_docs_specs(self) -> list[dict]:
        """列出 docs/spec/ 下的规格书"""
        specs = []
        if not self.docs_spec_dir.exists():
            return specs
        
        for spec_file in self.docs_spec_dir.glob("*.md"):
            specs.append({
                'name': spec_file.stem,
                'path': str(spec_file),
                'size': spec_file.stat().st_size,
            })
        return specs
    
    def extract_acceptance_criteria(self, spec_path: str) -> list[str]:
        """从规格书提取验收标准"""
        content = Path(spec_path).read_text(encoding='utf-8')
        
        # 查找 "验收标准" 或 "验收" 章节
        import re
        criteria = []
        
        # 匹配 "## 验收标准" 或 "## N. 验收标准" 章节
        section_match = re.search(
            r'##\s*(?:\d+\.\s*)?验收标准.*?\n(.*?)(?=\n##|\Z)',
            content, re.DOTALL
        )
        if section_match:
            section_text = section_match.group(1)
            # 提取列表项
            for line in section_text.split('\n'):
                line = line.strip()
                if line.startswith('-') or line.startswith('*'):
                    criteria.append(line.lstrip('-* ').strip())
                elif re.match(r'^\d+\.', line):
                    criteria.append(re.sub(r'^\d+\.\s*', '', line))
        
        return criteria
```

#### 6.3.2 现有 Spec 文件映射

| Spec 文件 | 对应闭环域 | 验收标准提取点 |
|-----------|-----------|---------------|
| `chaos-engine-spec-v0.1.md` | DevLoop | MVP v0.1 范围 |
| `chaos-engine-spec-v0.2.md` | DevLoop | Render 完整化 + AOI/Cell 验收标准 |
| `chaos-engine-replication-spec-v0.1.md` | VerifyLoop | 属性复制系统验收 |
| `chaos-engine-io_uring-ebpf-spec-v0.1.md` | TestLoop | io_uring/eBPF 测试验收 |
| `chaos-engine-admin-web-spec-v0.1.md` | VerifyLoop | Admin Web v0.1 验收 |
| `chaos-engine-admin-web-spec-v0.2.md` | VerifyLoop | Admin Web v0.2 验收 |

### 6.4 进程管理器

```python
import subprocess
import time
import json
from pathlib import Path

class ProcessManager:
    """ChaosEngine 集群进程管理器"""
    
    # 服务定义（与 status.sh 一致）
    SERVICES = [
        {"name": "dbproxy", "port": 9003, "pid_file": ".pids/dbproxy.pid"},
        {"name": "game",    "port": 7777, "pid_file": ".pids/game.pid"},
        {"name": "router",  "port": 9100, "pid_file": ".pids/router.pid"},
        {"name": "gateway", "port": 9000, "pid_file": ".pids/gateway.pid"},
        {"name": "admin",   "port": 9090, "pid_file": ".pids/admin.pid"},
    ]
    
    def __init__(self, chaos_engine_dir: str):
        self.project_dir = Path(chaos_engine_dir)
    
    def start_cluster(self, services: list[str] = None) -> bool:
        """启动集群"""
        script = self.project_dir / "scripts" / "start_cluster.sh"
        args = []
        if services:
            for svc in services:
                args.extend([f"--{svc}"])
        
        result = subprocess.run(
            ['bash', str(script)] + args,
            capture_output=True, text=True, timeout=30,
            cwd=str(self.project_dir),
        )
        return result.returncode == 0
    
    def stop_cluster(self) -> bool:
        """停止集群"""
        script = self.project_dir / "scripts" / "stop_cluster.sh"
        result = subprocess.run(
            ['bash', str(script)],
            capture_output=True, text=True, timeout=15,
            cwd=str(self.project_dir),
        )
        return result.returncode == 0
    
    def get_status(self) -> dict:
        """获取集群状态（JSON）"""
        script = self.project_dir / "scripts" / "status.sh"
        result = subprocess.run(
            ['bash', str(script), '--json'],
            capture_output=True, text=True, timeout=5,
            cwd=str(self.project_dir),
        )
        if result.returncode == 0:
            try:
                return json.loads(result.stdout)
            except json.JSONDecodeError:
                return {}
        return {}
    
    def wait_for_services(
        self, timeout: int = 30, services: list[str] = None
    ) -> bool:
        """等待所有服务就绪"""
        target_services = services or [s['name'] for s in self.SERVICES]
        
        start = time.time()
        while time.time() - start < timeout:
            status = self.get_status()
            all_ready = True
            
            for svc in status.get('services', []):
                if svc['name'] in target_services:
                    if svc['status'] != 'running':
                        all_ready = False
                        break
            
            if all_ready and status.get('services'):
                return True
            
            time.sleep(1)
        
        return False
    
    def start_client(
        self,
        mode: str = "vulkan",
        connect: str = None,
        env_extra: dict = None,
    ) -> subprocess.Popen:
        """启动客户端"""
        script = self.project_dir / "scripts" / "start_client.sh"
        args = [f"--{mode}"]
        if connect:
            args.extend(["--connect", connect])
        
        env = {**subprocess.os.environ, **(env_extra or {})}
        
        return subprocess.Popen(
            ['bash', str(script)] + args,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
            cwd=str(self.project_dir),
        )
```

### 6.5 Admin API 查询

```python
import httpx

class AdminQuery:
    """ChaosEngine Admin API 查询器"""
    
    ADMIN_URL = "http://127.0.0.1:9090"
    
    ENDPOINTS = [
        "/api/health",    # 健康检查
        "/api/stats",     # 统计信息（fps, uptime, entities...）
        "/api/aoi",       # AOI 状态
        "/api/cell",      # Cell 网格
        "/api/network",   # 网络统计
        "/api/memory",    # 内存使用
        "/api/cpu",       # CPU 使用
        "/api/render",    # 渲染统计
        "/api/system",    # 系统信息
        "/api/log",       # 日志查询
    ]
    
    async def query(self, endpoint: str) -> dict | None:
        """查询单个端点"""
        async with httpx.AsyncClient(timeout=5) as client:
            try:
                resp = await client.get(f"{self.ADMIN_URL}{endpoint}")
                if resp.status_code == 200:
                    return resp.json()
            except httpx.RequestError:
                pass
        return None
    
    async def query_all(self) -> dict:
        """查询所有端点"""
        import asyncio
        results = {}
        tasks = {ep: self.query(ep) for ep in self.ENDPOINTS}
        responses = await asyncio.gather(*tasks.values(), return_exceptions=True)
        
        for ep, resp in zip(tasks.keys(), responses):
            if isinstance(resp, Exception) or resp is None:
                results[ep] = None
            else:
                results[ep] = resp
        
        return results
    
    async def health_check(self) -> bool:
        """健康检查"""
        data = await self.query("/api/health")
        return data is not None
```

---

## 7. API 设计

### 7.1 CLI 接口

```bash
# 完整闭环验证
loopengine run [--config loop.yaml] [--report report.md]

# 仅开发闭环
loopengine dev --task "添加 AOI 范围查询功能" [--spec openspec/changes/xxx/]

# 仅测试闭环
loopengine test [--filter "test_aoi"]

# 仅验证闭环
loopengine verify [--services all]

# 仅客户端界面闭环
loopengine client-ui [--duration 10] [--screenshot]

# 查询状态
loopengine status

# 查看 AI 调度统计
loopengine ai-stats

# 生成报告
loopengine report [--format markdown|json] [--output report.md]
```

### 7.2 Python API

```python
from loopengine import LoopEngine, LoopConfig, LoopResult

# 初始化
config = LoopConfig(
    chaos_engine_dir="/home/zhongfangdao/chaos-engine",
    ark_api_key="your-key",
    ark_base_url="https://ark.cn-beijing.volces.com/api",
    model_routing="auto",         # auto / flash-only / glm-only
    vision_mode="ark",            # ark / local / hybrid
    cost_budget_daily=10.0,
    max_retries=3,
    report_format="markdown",
)

engine = LoopEngine(config)

# 完整闭环
result = await engine.run_full_loop(
    task="验证 ChaosEngine 客户端渲染和网络同步",
)

# 单闭环域
dev_result = await engine.run_dev_loop(
    task="修复 AOI 查询性能问题",
    spec_path="openspec/changes/xxx/",
)

test_result = await engine.run_test_loop()
verify_result = await engine.run_verify_loop()
client_result = await engine.run_client_ui_loop(duration=10)

# 查看结果
print(f"状态: {result.status}")
print(f"耗时: {result.duration_sec}s")
print(f"重试: {result.retry_count}")
for assertion in result.assertions:
    print(f"  [{assertion['severity']}] {assertion['name']}: {assertion['result']}")
```

### 7.3 配置文件格式

```yaml
# config/loop.yaml
chaos_engine:
  dir: /home/zhongfangdao/chaos-engine
  build_dir: build

ark:
  api_key: ${ARK_API_KEY}
  base_url: https://ark.cn-beijing.volces.com/api

model_routing:
  dev.requirement_analysis: glm
  dev.code_generation: pro
  dev.simple_edit: flash
  dev.compile_error_analysis: glm
  test.failure_analysis: glm
  test.lua_lint_fix: flash
  verify.cluster_startup_analysis: glm
  verify.data_consistency: pro
  verify.api_validation: flash
  client.title_bar_analysis: flash
  client.vision_review: glm
  client.render_error_analysis: pro

vision:
  mode: ark                     # ark / local / hybrid
  model: glm-5-2-260617
  max_daily_calls: 50
  prompt_template: default      # default / custom

cost:
  daily_budget_cny: 10.0
  per_loop_budget_cny: 2.0
  flash_daily_limit: 500
  vision_daily_limit: 50

loops:
  dev:
    max_retries: 3
    timeout_sec: 300
  test:
    max_retries: 3
    timeout_sec: 300
  verify:
    max_retries: 3
    timeout_sec: 120
    services: [dbproxy, game, router, gateway, admin]
  client_ui:
    max_retries: 2
    timeout_sec: 60
    sample_duration_sec: 10
    sample_interval_sec: 1.0
    screenshot_dir: screenshots

report:
  format: markdown
  output: report.md
  include_screenshots: true
  include_logs: true

# 环境
environment:
  display: ${DISPLAY:-:0}
  xvfb: false                   # CI 环境设为 true
  vulkan_icd: /usr/share/vulkan/icd.d/lvp_icd.json
```

### 7.4 数据模型

```python
from dataclasses import dataclass, field
from datetime import datetime
from enum import Enum
from typing import Optional

class LoopDomainName(Enum):
    DEV = "dev"
    TEST = "test"
    VERIFY = "verify"
    CLIENT_UI = "client_ui"

class LoopStatus(Enum):
    PENDING = "pending"
    RUNNING = "running"
    VERIFYING = "verifying"
    PASSED = "passed"
    FAILED = "failed"
    RETRYING = "retrying"

@dataclass
class LoopExecution:
    """单次闭环执行记录"""
    id: str                          # UUID
    domain: LoopDomainName
    status: LoopStatus
    started_at: datetime
    finished_at: Optional[datetime] = None
    retry_count: int = 0
    duration_sec: float = 0.0
    task_description: str = ""
    
    # 断言结果
    assertions: list[dict] = field(default_factory=list)
    
    # 产物
    artifacts: dict = field(default_factory=dict)
    # artifacts 可能包含:
    #   - screenshot_path: str
    #   - log_paths: list[str]
    #   - title_snapshots: list[dict]
    #   - admin_api_snapshot: dict
    #   - vision_result: dict
    #   - git_diff: str
    
    # AI 分析
    ai_analysis: Optional[str] = None
    ai_model_used: Optional[str] = None
    ai_cost_cny: float = 0.0
    
    # 错误
    error: Optional[str] = None

@dataclass
class FullLoopReport:
    """完整闭环报告"""
    id: str
    started_at: datetime
    finished_at: Optional[datetime] = None
    task_description: str = ""
    
    dev_loop: Optional[LoopExecution] = None
    test_loop: Optional[LoopExecution] = None
    verify_loop: Optional[LoopExecution] = None
    client_ui_loop: Optional[LoopExecution] = None
    
    overall_status: LoopStatus = LoopStatus.PENDING
    total_duration_sec: float = 0.0
    total_cost_cny: float = 0.0
    
    def to_markdown(self) -> str:
        """生成 Markdown 报告"""
        # ... 实现略
        pass
    
    def to_json(self) -> str:
        """生成 JSON 报告"""
        import json
        return json.dumps(self.__dict__, default=str, indent=2)
```

---

## 8. MVP 定义

### 8.1 v0.1 MVP 范围

v0.1 的目标是**跑通最小可用闭环**，验证核心设计可行。

#### 8.1.1 包含

| 功能 | 说明 | 优先级 |
|------|------|--------|
| CLI 命令 `loopengine run` | 触发完整闭环 | P0 |
| 开发闭环（简化版） | 仅编译验证，不含 AI 写码 | P1 |
| 测试闭环 | CITrigger 驱动 Gitee CI 流水线 | P0 |
| 验证闭环 | start_cluster + test_client + admin query | P0 |
| 客户端界面闭环 | 结构化断言 + 截图视觉复核 | P0 |
| AI 调度层 | flash/glm/pro 三模型路由 | P1 |
| Markdown 报告 | 自动生成验证报告 | P1 |
| SQLite 存储 | 执行记录持久化 | P2 |
| 配置文件 | loop.yaml | P1 |

#### 8.1.2 不包含

| 功能 | 延后版本 | 原因 |
|------|----------|------|
| AI 自动写码 | v0.2 | 复杂度高，v0.1 先验证闭环机制 |
| 子 agent 并行编排 | v0.2 | v0.1 串行执行即可 |
| 输入模拟（xdotool） | v0.2 | v0.1 被动观察 |
| 多机分布式 | v0.3 | v0.1 单机 |
| Web UI | v0.3 | v0.1 CLI 足够 |
| 压测集成 | v0.2 | v0.1 专注功能验证 |
| 规格书自动提取验收标准 | v0.2 | v0.1 手动配置断言 |

#### 8.1.3 MVP 验收标准

```
验收1: loopengine run --config loop.yaml 能完成四域闭环
  - 开发闭环: build_and_test.sh --no-test 编译通过
  - 测试闭环: CITrigger 触发 Gitee CI，所有目标 job passed
  - 验证闭环: 集群启动+冒烟测试通过
  - 客户端闭环: 客户端启动+结构化断言+（必要时）视觉复核

验收2: 客户端界面验证能区分三种结果
  - 全通过: PASSED（不调视觉模型）
  - 部分失败+视觉正常: PASSED（误报修正）
  - 部分失败+视觉异常: FAILED

验收3: AI 调度正确路由模型
  - 简单任务 → flash
  - 复杂任务 → glm/pro
  - 视觉判定 → glm

验收4: 成本可控
  - 单次完整闭环 AI 成本 < ¥2
  - 视觉模型调用 ≤ 5 次/闭环

验收5: 报告生成
  - Markdown 报告包含所有闭环域结果
  - 失败时包含 AI 分析和截图
```

### 8.2 MVP 技术选型

| 组件 | 选型 | 原因 |
|------|------|------|
| 语言 | Python 3.11+ | 快速开发，httpx/aiohttp 生态好 |
| 异步框架 | asyncio + httpx | 原生异步，适合 IO 密集 |
| 数据存储 | SQLite | 轻量，无需额外服务 |
| 配置 | YAML (pyyaml) | 人类可读 |
| CLI | argparse / click | 标准库够用 |
| 截图 | ImageMagick import / xwd | 系统自带 |
| HTTP 客户端 | httpx | 支持 async |
| 日志 | logging | 标准库 |
| 测试 | pytest | 标准 |

---

## 9. 路线图

### 9.1 v0.1：最小可用闭环（2-3 周）

**目标：** 跑通四域闭环，验证核心设计可行。

| 阶段 | 任务 | 预计工时 |
|------|------|----------|
| W1 | 项目骨架 + 配置系统 + CLI | 2d |
| W1 | ScriptExec + ProcMgr + LogCollect | 2d |
| W1 | AdminQuery + ScreenshotCapture | 1d |
| W2 | 结构化断言引擎（标题栏解析 + 日志断言） | 2d |
| W2 | 视觉复核机制（ARK vision + local fallback） | 2d |
| W2 | 客户端界面闭环完整流程 | 1d |
| W3 | AI 调度层（模型路由 + ARK 客户端） | 2d |
| W3 | 四域闭环串联 + 状态机 | 2d |
| W3 | Markdown 报告生成 | 1d |

**v0.1 交付物：**
- `loopengine` CLI 工具
- 四域闭环可运行
- 结构化断言 + 视觉复核
- Markdown 验证报告

### 9.2 v0.2：AI 增强 + 深度集成（3-4 周）

**目标：** AI 能真正写码修复，深度集成 CI 和 Spec。

| 功能 | 说明 |
|------|------|
| AI 自动写码 | AI agent 接收需求 → 写码 → 编译 → 修复迭代 |
| 子 agent 编排 | 多 agent 并行处理独立子任务 |
| 输入模拟 | xdotool 模拟 WASD/方向键，验证交互响应 |
| CI 集成 | Gitee API 触发/查询 CI，解析测试结果 |
| Spec 联动 | 自动从 openspec 提取验收标准 → 生成断言规则 |
| 压测集成 | 调用 stress_client_v2.py + bench_async_io.py |
| 成本仪表盘 | AI 调用成本实时统计 |
| 失败修复闭环 | AI 分析失败 → 生成 patch → 应用 → 重试 |

**v0.2 交付物：**
- AI 能自动完成简单开发任务（单文件修改级别）
- 输入模拟验证交互
- CI 自动触发和结果解析
- Spec → 断言自动生成

### 9.3 v0.3：规模化 + 智能化（4-6 周）

**目标：** 支持复杂场景验证，向持续验证平台演进。

| 功能 | 说明 |
|------|------|
| 多机分布式 | 支持多台机器并行验证不同闭环域 |
| Web UI | 浏览器仪表盘，实时查看验证进度 |
| 持续验证 | 定时执行全流程闭环，监控代码质量趋势 |
| 性能回归检测 | 记录 FPS/延迟/内存历史数据，自动检测回归 |
| AI 模型微调 | 基于 ChaosEngine 历史数据微调模型，提升判定准确率 |
| 插件系统 | 支持自定义闭环域、断言规则、AI 分析器 |
| 多版本并行 | 同时验证多个分支/版本的 ChaosEngine |
| 自动 PR | AI 修复后自动提交 PR（带验证报告） |

**v0.3 交付物：**
- Web UI 仪表盘
- 分布式验证
- 性能回归检测
- 插件系统

### 9.4 版本对照表

| 维度 | v0.1 | v0.2 | v0.3 |
|------|------|------|------|
| **闭环域** | 四域串行 | 四域+AI修复 | 四域+分布式 |
| **AI 写码** | ❌ | ✅ 简单任务 | ✅ 复杂任务 |
| **输入模拟** | ❌ 被动观察 | ✅ xdotool | ✅ + 录制回放 |
| **CI 集成** | ❌ 本地 | ✅ Gitee API | ✅ + 自动PR |
| **Spec 联动** | ❌ 手动配置 | ✅ 自动提取 | ✅ + 双向同步 |
| **压测** | ❌ | ✅ 集成 | ✅ + 回归检测 |
| **UI** | CLI | CLI | CLI + Web |
| **执行** | 单机 | 单机 | 多机分布式 |
| **成本** | <¥2/闭环 | <¥5/闭环 | 按预算动态调节 |

---

## 10. 风险与约束

### 10.1 技术风险

| 风险 | 概率 | 影响 | 缓解措施 |
|------|------|------|----------|
| **Vulkan 在 CI 虚拟环境渲染异常** | 高 | 高 | 使用 lavapipe (lvp) 软件渲染；hybrid vision mode 本地像素检查兜底 |
| **X11 窗口标题栏采集不稳定** | 中 | 中 | 多种采集方式备选（xprop / xdotool / xwininfo）；超时重试 |
| **ARK 视觉模型判定不稳定** | 中 | 高 | 低 temperature (0.1) + 结构化 JSON 输出；本地像素检查交叉验证 |
| **AI 生成的代码有语法/逻辑错误** | 高 | 中 | 编译验证 + 测试闭环兜底；max_retries 限制 |
| **集群进程残留导致端口冲突** | 中 | 低 | stop_cluster.sh 强制清理；启动前检查端口占用 |
| **AI 调用成本失控** | 低 | 高 | 每日预算硬限制；flash 优先；视觉调用频率限制 |
| **Lua 压测机器人与 C 绑定不兼容** | 中 | 中 | v0.1 用 Python 压测；v0.2 共享 C 绑定 Lua 机器人 |

### 10.2 环境约束

| 约束 | 说明 | 应对 |
|------|------|------|
| **显示环境** | Vulkan 客户端需要 X11 DISPLAY + XAUTHORITY | 本地：自动检测；CI：Xvfb 虚拟显示 |
| **Vulkan 驱动** | CI 环境通常无 GPU | 使用 lavapipe (lvp) 软件渲染 ICD |
| **ARK API 网络** | 需要访问 ARK API | 免费 ARK 额度足够日常使用；离线降级到 local vision |
| **Python 环境** | PEP 668 限制系统 pip | 使用 venv 或 uv |
| **进程权限** | xdotool/xprop 需要访问 X11 | 确保 DISPLAY/XAUTHORITY 正确设置 |
| **磁盘空间** | 截图/日志积累 | 定期清理；截图压缩 |
| **ChaosEngine 编译依赖** | cmake/liburing/libbpf/lua5.4/vulkan | 假设 ChaosEngine 已编译通过 |

### 10.3 成本约束

| 约束 | 预算 | 控制 |
|------|------|------|
| 单次完整闭环 | < ¥2 | flash 优先；视觉 ≤5 次 |
| 每日总预算 | < ¥10 | 硬限制，超限停止 |
| 视觉模型 | ≤ 50 次/天 | 结构化断言先行过滤 |
| flash 模型 | ≤ 500 次/天 | 合并请求，减少调用次数 |

### 10.4 依赖清单

| 依赖 | 版本 | 用途 | 必需 |
|------|------|------|------|
| Python | 3.11+ | LoopEngine 运行时 | ✅ |
| httpx | 0.27+ | ARK API 客户端 | ✅ |
| pyyaml | 6.0+ | 配置文件解析 | ✅ |
| Pillow | 10.0+ | 本地视觉检查（像素分析） | ✅ |
| numpy | 1.24+ | 像素数据分析 | ✅ |
| ImageMagick | 7.0+ | 截图采集 (import) | ✅ |
| xdotool | 3.x | 窗口管理/输入模拟(v0.2) | v0.2 |
| Xvfb | 21.x | CI 虚拟显示 | CI |
| SQLite | 3.x | 数据存储（Python 内置） | ✅ |

---

## 11. 附录

### 11.1 ChaosEngine 客户端标题栏格式参考

来源：`src_c/runtime/ce_client_main.c` 第 154-164 行

```c
char title[256];
if (net && ce_client_net_is_connected(net)) {
    int ec = ce_client_net_entity_count(net);
    snprintf(title, sizeof(title),
             "ChaosEngine Vulkan | pos=(%.2f, %.2f, %.2f) | visible=%d | keys=%s",
             local_x, 0.0f, local_z, ec, key_hint);
} else {
    snprintf(title, sizeof(title),
             "ChaosEngine Vulkan | pos=(%.2f, %.2f, %.2f) | disconnected | keys=%s",
             local_x, 0.0f, local_z, key_hint);
}
rhi_set_window_title(rhi, title);
```

标题栏通过 `rhi_set_window_title` → `XStoreName` 设置到 X11 窗口。

### 11.2 ChaosEngine 集群端口表

来源：`scripts/start_cluster.sh`

| 服务 | 端口 | 说明 |
|------|------|------|
| Game Server | 7777 | TCP 游戏服务 |
| Gateway TCP | 9000 | TCP 网关 |
| Gateway KCP | 9001 | KCP 网关 |
| Gateway WS | 9002 | WebSocket 网关 |
| DBProxy Sync | 9001 | DB 同步 |
| DBProxy DB | 9003 | 数据库 |
| Router Game | 9100 | 路由游戏端口 |
| Router Cluster | 9101 | 集群路由端口 |
| Admin Web | 9090 | HTTP 管理后台 |
| Admin IPC | /tmp/chaos_admin.sock | Unix Socket |

### 11.3 ChaosEngine 测试用例清单

来源：`scripts/build_and_test.sh` 冒烟测试

| 测试 | 说明 |
|------|------|
| test_math | 数学库 |
| test_memory | 内存管理 |
| test_ecs | ECS 实体系统 |
| test_network | 网络层 |
| test_aoi | AOI 十字链表 |
| test_cell | Cell 大地图 |
| test_net_base | 网络基础 |

（共 18 个测试，冒烟测试验证其中 7 个关键测试）

### 11.4 ChaosEngine 二进制产物

| 二进制 | 来源 | 用途 |
|--------|------|------|
| chaos_server | Game Server | 游戏服务端 |
| chaos_client | Vulkan 客户端 | 渲染客户端 |
| chaos_headless | Headless | 无渲染测试 |
| chaos_gateway | 网关 | 网络网关 |
| chaos_router | 路由 | 集群路由 |
| chaos_dbproxy | DB 代理 | 数据库代理 |
| chaos_net_client | 网络客户端 | TCP 网络测试 |
| bench_client | 压测客户端 | 性能压测 |
| chaos_async_bench | 异步 IO 基准 | io_uring 基准测试 |

### 11.5 术语表

| 术语 | 说明 |
|------|------|
| LoopEngine | 本项目——AI 驱动的 ChaosEngine 验证平台 |
| ChaosEngine | 被验证对象——纯 C 游戏引擎 |
| 闭环域 (Loop Domain) | 四个验证域：开发/测试/验证/客户端界面 |
| 结构化断言 | 通过解析标题栏/日志等结构化数据进行断言判定 |
| 视觉复核 | 截图送 AI 视觉模型进行渲染正确性判定 |
| ARK | 火山方舟 AI 模型服务平台 |
| AOI | Area of Interest，兴趣区域 |
| Cell | 大地图空间划分单元 |
| RHI | Rendering Hardware Interface，渲染硬件抽象层 |
| Xvfb | X Virtual Framebuffer，虚拟 X 服务器 |
| lavapipe | Vulkan 软件渲染器（Mesa 项目） |

### 11.6 参考文档

| 文档 | 路径 |
|------|------|
| ChaosEngine 设计规格 v0.1 | `docs/spec/chaos-engine-spec-v0.1.md` |
| ChaosEngine 设计规格 v0.2 | `docs/spec/chaos-engine-spec-v0.2.md` |
| 复制系统规格 | `docs/spec/chaos-engine-replication-spec-v0.1.md` |
| io_uring + eBPF 规格 | `docs/spec/chaos-engine-io_uring-ebpf-spec-v0.1.md` |
| Admin Web 规格 v0.1 | `docs/spec/chaos-engine-admin-web-spec-v0.1.md` |
| Admin Web 规格 v0.2 | `docs/spec/chaos-engine-admin-web-spec-v0.2.md` |
| OpenSpec 配置 | `openspec/config.yaml` |
| CI 配置 | `.gitee-ci.yml` |

---

> **文档结束**
>
> 本规格书为 LoopEngine v0.1 草案，遵循 spec-first 原则，待确认后进入实现阶段。
>
> 提交格式：`docs(spec): add loop-engine spec v0.1`
