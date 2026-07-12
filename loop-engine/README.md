# LoopEngine

混沌引擎自动化闭环验证工具。围绕 ChaosEngine（客户端 MMO 引擎）的开发-测试-验证-客户端 UI 四阶段闭环，用 AI 驱动需求分析、代码生成、编译错误修复、测试失败分析、集群启动诊断、数据一致性验证、视觉审查等环节，实现"提交代码 → 自动验证 → 自动修复"的无人值守循环。

## 功能

- **dev loop**：需求分析 → 代码生成 → 编译检查 → 错误修复
- **test loop**：测试失败分析 → Lua lint 修复
- **verify loop**：集群启动诊断 → 数据一致性验证 → API 校验
- **client-ui loop**：标题栏分析 → 渲染错误诊断 → 视觉审查（截图比对）

## 安装

```bash
cd /home/zhongfangdao/chaos-engine/loop-engine
pip install -e .
```

## 配置

编辑 `config/loop.yaml` 和 `config/models.yaml`，设置环境变量：

```bash
export ARK_API_KEY=your_key
export GITEE_API_TOKEN=your_token
```

## 基本用法

```bash
# 执行开发闭环（代码生成 + 编译检查）
loopengine run --task "实现新的战斗系统接口"

# 执行测试闭环
loopengine test --task "运行战斗系统单元测试"

# 执行验证闭环（集群 + 数据一致性 + API）
loopengine verify --task "验证新接口集群部署"

# 执行客户端 UI 闭环（截图视觉审查）
loopengine client-ui --task "检查标题栏和渲染状态"

# 查看历史执行记录
loopengine list --limit 20
```

## 架构

```
loop-engine/
├── pyproject.toml          # 项目配置
├── README.md
├── config/
│   ├── loop.yaml           # 主配置（路由、成本、超时、CI）
│   └── models.yaml         # 模型定义与价格
└── loopengine/
    ├── __init__.py          # 公共 API 导出
    ├── cli.py               # CLI 入口（待实现）
    └── data/
        ├── __init__.py
        ├── models.py        # 数据模型（dataclass + Enum）
        └── db.py            # SQLite 异步持久化
```

## 技术栈

- Python 3.11+
- SQLite（aiosqlite 异步驱动）
- ARK API（火山方舟模型服务）
- Gitee CI 触发

## 许可

内部项目，未公开发布。
