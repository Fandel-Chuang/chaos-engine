# ChaosEngine

> 面向 3D 多人游戏的、客户端/服务器同构的、纯 C 内核 + Lua 脚本的轻量级开源游戏引擎。

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Standard](https://img.shields.io/badge/C-99-blue.svg)]()
[![Standard](https://img.shields.io/badge/C%2B%2B-17-blue.svg)]()

## 设计理念

- **极度简洁** — 内核代码量最小化，每个模块只做一件事
- **高效执行** — 纯 C 内核，Cache-Friendly 数据布局
- **同构逻辑** — 同一份战斗代码，客户端帧同步 + 服务端状态同步
- **严格分层** — C 内核 → public_api → C++ 编辑器，三层隔离
- **全平台** — Windows / Linux / macOS / iOS / Android

## 架构概览

```
┌────────────────────────────────────────────┐
│            编辑器层 (C++17)                  │
│    Dear ImGui / 资源导入 / 第八模式观测      │
├────────────────────────────────────────────┤
│           public_api (纯 C 头文件)           │
├────────────────────────────────────────────┤
│            引擎内核 (纯 C99)                 │
│  Core │ ECS │ Render(RHI) │ Network │ Lua  │
├────────────────────────────────────────────┤
│  平台抽象: Win / Linux / macOS / iOS / Android │
└────────────────────────────────────────────┘
```

## 快速开始

```bash
# 克隆
git clone https://gitee.com/zhong-fangdao/chaos-engine.git
cd chaos-engine

# 编译 (需要 CMake 3.20+)
mkdir build && cd build
cmake .. -DCHAOS_BUILD_EDITOR=ON
cmake --build .

# 运行编辑器
./bin/chaos_editor

# 运行无头模式（服务器/测试）
./bin/chaos_headless
```

## 编译选项

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `CHAOS_BUILD_EDITOR` | ON | 编译 C++ 编辑器 |
| `CHAOS_BUILD_TESTS` | OFF | 编译单元测试 |
| `CHAOS_BUILD_SAMPLES` | OFF | 编译示例程序 |

## 参考引擎

| 引擎 | 借鉴内容 |
|------|----------|
| Skynet | Actor 并发模型、Lua 服务化 |
| KBEngine | Cell 空间划分、动态 AOI |
| Ant Engine | 纯 ECS 架构、Archetype 存储 |
| Unreal Engine | Actor/Component 概念、属性复制 |

## 开发状态

当前版本：**v0.1 (MVP)** — 最小可运行骨架

详见 [设计规格说明书](docs/spec/chaos-engine-spec-v0.1.md)

## 许可证

MIT License
