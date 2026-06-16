# ChaosEngine Web 管理后台 — 完整软件依赖链

> **日期：** 2026-06-15 | **环境：** Ubuntu 24.04 (Noble Numbat) | **架构：** amd64
>
> 本文档梳理 chaos_admin（Lapis Web 管理后台）从底层系统库到顶层 Lua 包的全部依赖。

---

## 目录

1. [依赖全景图](#1-依赖全景图)
2. [分层依赖树](#2-分层依赖树)
3. [逐包详情](#3-逐包详情)
4. [运行时 vs 编译时](#4-运行时-vs-编译时)
5. [当前环境状态](#5-当前环境状态)
6. [一键安装脚本](#6-一键安装脚本)

---

## 1. 依赖全景图

```
┌─────────────────────────────────────────────────────────────────────┐
│                        chaos_admin (Lapis)                          │
│                     HTTP :9090 + WebSocket /ws                       │
└───────────────────────────────┬─────────────────────────────────────┘
                                │
        ┌───────────────────────┼───────────────────────┐
        │                       │                       │
        ▼                       ▼                       ▼
┌───────────────┐    ┌───────────────────┐    ┌───────────────────┐
│   Lapis 1.18  │    │   IPC Client      │    │   Dashboard HTML  │
│  (Web 框架)    │    │ (Unix Socket RPC) │    │   (内嵌前端)       │
└───────┬───────┘    └────────┬──────────┘    └───────────────────┘
        │                     │
        │              ┌──────┴──────┐
        │              │  luasocket  │── luaossl ── libssl
        │              │  lua-cjson  │
        │              └─────────────┘
        │
   ┌────┴────────────────────────────┐
   │  Lapis 直接依赖 (LuaRocks)       │
   │  ansicolors  argparse  date     │
   │  etlua  loadkit  lpeg           │
   │  lua-cjson  luaossl  luasocket  │
   │  pgmoon                         │
   └────┬────────────────────────────┘
        │
   ┌────┴────────────────────────────┐
   │  运行时引擎 (二选一)              │
   │                                 │
   │  A) OpenResty (nginx + LuaJIT)  │
   │  B) LuaJIT + lua-http + cqueues │
   └────┬────────────────────────────┘
        │
   ┌────┴────────────────────────────┐
   │  系统层                          │
   │  libluajit  libssl  libpcre2    │
   │  zlib1g  libc6  libgcc-s1       │
   └─────────────────────────────────┘
```

---

## 2. 分层依赖树

### 2.1 运行时依赖（生产环境必需）

```
chaos_admin 进程
│
├── LuaJIT 2.1 (OpenResty 分支)                    [运行时 ★★★]
│   ├── libluajit-5.1-2          (共享库)
│   ├── libluajit-5.1-common     (公共文件)
│   └── luajit                   (解释器)
│
├── 运行模式 A：OpenResty (推荐生产方案)            [运行时 ★★★]
│   ├── openresty                (nginx + LuaJIT 集成)
│   │   ├── libpcre2-8-0         (正则引擎，运行时)
│   │   ├── libssl3t64           (SSL/TLS，运行时)
│   │   └── zlib1g               (压缩，运行时)
│   └── Lapis 以 `lapis server production` 启动
│
├── 运行模式 B：纯 LuaJIT + lua-http (轻量方案)    [运行时 ★★★]
│   ├── lua-http                (Lua HTTP/WebSocket 库，需 luarocks 安装)
│   ├── cqueues                 (异步 I/O 框架，需 luarocks 安装)
│   └── Lapis 以 `lapis server` 启动 (cqueues 模式)
│
├── Lapis 框架 + 依赖 (LuaRocks)                    [运行时 ★★★]
│   ├── lapis         1.18.0    Web 框架核心
│   ├── ansicolors    1.0.2    终端颜色输出
│   ├── argparse      0.7.2    CLI 参数解析 (lapis 命令行)
│   ├── date          2.2.1    日期时间处理
│   ├── etlua         1.3.0    嵌入式 Lua 模板引擎
│   ├── loadkit       1.1.0    模块加载工具
│   ├── lpeg          1.1.0    Lua 解析表达式文法 (路由匹配)
│   ├── lua-cjson     2.1.0.10 快速 JSON 编解码
│   ├── luaossl       20250929 OpenSSL 绑定 (HTTPS)
│   ├── luasocket     3.1.0    TCP/Unix Socket (IPC 客户端)
│   └── pgmoon        1.17.0   PostgreSQL 客户端 (ORM 层)
│
├── 系统共享库                                      [运行时 ★★☆]
│   ├── libssl3t64              SSL/TLS 运行时库
│   ├── libpcre2-8-0            PCRE2 正则运行时库
│   ├── zlib1g                  压缩运行时库
│   ├── libc6                   GNU C 库 (基础)
│   └── libgcc-s1               GCC 运行时库
│
└── 系统工具                                       [运行时 ★☆☆]
    └── luarocks                 Lua 包管理器 (用于安装/更新依赖)
```

### 2.2 编译时依赖（仅开发/构建时需要）

```
编译工具链
├── build-essential              [编译时] 元包，包含：
│   ├── gcc / g++                编译器
│   ├── make                     构建工具
│   └── libc6-dev                C 标准库头文件
│
├── libssl-dev       3.5.5       [编译时] OpenSSL 开发头文件
│   └── (luaossl 编译时需要)
│
├── libpcre2-dev     10.46       [编译时] PCRE2 开发头文件
│   └── (OpenResty/lpeg 编译时需要)
│
├── zlib1g-dev       1:1.3       [编译时] zlib 开发头文件
│   └── (OpenResty 编译时需要)
│
└── libluajit-5.1-dev 2.1.0     [编译时] LuaJIT 开发头文件
    └── (C 扩展 luarock 编译时需要，如 lua-cjson, luaossl, lpeg)
```

### 2.3 可选依赖

```
可选组件
├── PostgreSQL Server            [可选] pgmoon 连接目标
│   └── 仅当使用 Lapis ORM + 数据库持久化时需要
│   └── 当前规格：不做数据库持久化 → 实际不需要
│
├── lua-websockets   v2.2        [可选] 纯 Lua WebSocket 库
│   └── 如果不用 OpenResty 内置 WebSocket，可用此库
│   └── Lapis 在 OpenResty 模式下用 nginx 原生 WebSocket
│
└── lua-nginx-redis / lua-nginx-memcached  [可选]
    └── 缓存层，当前规格不需要
```

---

## 3. 逐包详情

### 3.1 系统包（apt）

| 包名 | 版本 | 用途 | 类型 | 安装命令 |
|------|------|------|------|----------|
| `luajit` | 2.1.0+openresty20251030-1 | LuaJIT 解释器（OpenResty 维护分支） | 运行时 | `apt install luajit` |
| `libluajit-5.1-2` | 2.1.0+openresty20251030-1 | LuaJIT 共享库 | 运行时 | `apt install libluajit-5.1-2` |
| `libluajit-5.1-common` | 2.1.0+openresty20251030-1 | LuaJIT 公共文件 | 运行时 | `apt install libluajit-5.1-common` |
| `libluajit-5.1-dev` | 2.1.0+openresty20251030-1 | LuaJIT 开发头文件 | 编译时 | `apt install libluajit-5.1-dev` |
| `libssl3t64` | 3.5.5-1ubuntu3.2 | OpenSSL 运行时库 | 运行时 | `apt install libssl3t64` |
| `libssl-dev` | 3.5.5-1ubuntu3.2 | OpenSSL 开发头文件 | 编译时 | `apt install libssl-dev` |
| `libpcre2-8-0` | 10.46-1build1 | PCRE2 正则运行时 | 运行时 | `apt install libpcre2-8-0` |
| `libpcre2-dev` | 10.46-1build1 | PCRE2 开发头文件 | 编译时 | `apt install libpcre2-dev` |
| `zlib1g` | 1:1.3.dfsg+really1.3.1 | zlib 压缩运行时 | 运行时 | `apt install zlib1g` |
| `zlib1g-dev` | 1:1.3.dfsg+really1.3.1 | zlib 开发头文件 | 编译时 | `apt install zlib1g-dev` |
| `build-essential` | 12.12ubuntu2 | 编译工具链元包 | 编译时 | `apt install build-essential` |
| `luarocks` | 3.8.0 | Lua 包管理器 | 运行时 | `apt install luarocks` |

### 3.2 LuaRocks 包

| 包名 | 版本 | 用途 | 许可 | 安装命令 |
|------|------|------|------|----------|
| `lapis` | 1.18.0-1 | Web 框架核心（路由/中间件/ORM/模板） | MIT | `luarocks install lapis` |
| `ansicolors` | 1.0.2-3 | 终端 ANSI 颜色输出 | MIT | (lapis 自动安装) |
| `argparse` | 0.7.2-1 | CLI 参数解析（`lapis` 命令行工具用） | MIT | (lapis 自动安装) |
| `date` | 2.2.1-2 | 日期/时间解析与格式化 | MIT | (lapis 自动安装) |
| `etlua` | 1.3.0-1 | 嵌入式 Lua 模板引擎（`<% %>` 语法） | MIT | (lapis 自动安装) |
| `loadkit` | 1.1.0-1 | 安全的 Lua 模块加载器 | MIT | (lapis 自动安装) |
| `lpeg` | 1.1.0-2 | Lua 解析表达式文法（路由模式匹配） | MIT | (lapis 自动安装) |
| `lua-cjson` | 2.1.0.10-1 | 快速 C 语言 JSON 编解码器 | MIT | (lapis 自动安装) |
| `luaossl` | 20250929-0 | 全面的 OpenSSL 绑定（SSL/TLS/X.509） | MIT | (lapis 自动安装) |
| `luasocket` | 3.1.0-1 | TCP/UDP/Unix Socket 网络库 | MIT | (lapis 自动安装) |
| `pgmoon` | 1.17.0-1 | PostgreSQL 客户端（Lapis ORM 用） | MIT | (lapis 自动安装) |

> **注意：** 以上 LuaRocks 包中，`pgmoon` 仅在需要 PostgreSQL 数据库时使用。当前规格明确"不做数据库持久化"，因此 `pgmoon` 实际上不需要。但 Lapis 默认会安装它。

### 3.3 Lapis 依赖关系图

```
lapis (1.18.0)
├── ansicolors (1.0.2)      ← 终端输出
├── argparse (0.7.2)        ← CLI 工具
├── date (2.2.1)            ← 时间处理
├── etlua (1.3.0)           ← 模板引擎
├── loadkit (1.1.0)         ← 模块加载
├── lpeg (1.1.0)            ← 路由匹配
├── lua-cjson (2.1.0.10)    ← JSON 编解码
├── luaossl (20250929)      ← SSL/TLS
│   └── (依赖 libssl-dev 编译)
├── luasocket (3.1.0)       ← TCP/Unix Socket
└── pgmoon (1.17.0)         ← PostgreSQL 客户端
```

所有 LuaRocks 叶子包（ansicolors, argparse, date, etlua, loadkit, lpeg, lua-cjson, luaossl, luasocket, pgmoon）均无进一步的 LuaRocks 依赖。它们都是纯 Lua 或 C 扩展，仅依赖系统库。

### 3.4 OpenResty（运行时引擎 A）

OpenResty 是 Lapis 的生产级运行时，将 nginx 与 LuaJIT 深度集成。

| 组件 | 说明 |
|------|------|
| 完整包名 | `openresty` |
| 安装方式 | 从 OpenResty 官方仓库安装 |
| 官方源 | `http://openresty.org/package/ubuntu/` |
| 当前状态 | **未安装**，APT 源未配置 |
| 依赖 | libpcre2, libssl, zlib, libluajit (均自带) |

**安装 OpenResty 的步骤：**
```bash
# 添加 OpenResty 官方 APT 仓库
wget -qO - https://openresty.org/package/pubkey.gpg | sudo apt-key add -
sudo apt-add-repository -y "deb http://openresty.org/package/ubuntu $(lsb_release -sc) main"
sudo apt update
sudo apt install openresty
```

### 3.5 运行模式对比

| 维度 | 模式 A: OpenResty | 模式 B: lua-http + cqueues |
|------|-------------------|---------------------------|
| **安装复杂度** | 高（需添加外部 APT 源） | 中（luarocks 安装） |
| **性能** | 极高（nginx 事件驱动） | 高（cqueues 异步 I/O） |
| **WebSocket** | nginx 原生支持 | 需额外 lua-websockets |
| **生产就绪** | ✅ 是 | ⚠️ 适合开发/轻量部署 |
| **内存占用** | ~50-100MB | ~20-50MB |
| **配置复杂度** | 需 nginx.conf | 仅 Lapis config.lua |
| **推荐场景** | 生产环境 | 开发/测试环境 |

---

## 4. 运行时 vs 编译时

### 4.1 分类总表

| 包名 | 运行时 | 编译时 | 备注 |
|------|:------:|:------:|------|
| `luajit` | ✅ | ❌ | LuaJIT 解释器 |
| `libluajit-5.1-2` | ✅ | ❌ | LuaJIT 共享库 |
| `libluajit-5.1-common` | ✅ | ❌ | LuaJIT 公共文件 |
| `libluajit-5.1-dev` | ❌ | ✅ | C 扩展编译头文件 |
| `libssl3t64` | ✅ | ❌ | OpenSSL 运行时 |
| `libssl-dev` | ❌ | ✅ | luaossl 编译需要 |
| `libpcre2-8-0` | ✅ | ❌ | PCRE2 运行时 |
| `libpcre2-dev` | ❌ | ✅ | lpeg/OpenResty 编译需要 |
| `zlib1g` | ✅ | ❌ | 压缩运行时 |
| `zlib1g-dev` | ❌ | ✅ | OpenResty 编译需要 |
| `build-essential` | ❌ | ✅ | gcc/make 等编译工具 |
| `luarocks` | ✅ | ❌ | 包管理（安装/更新时用） |
| `lapis` | ✅ | ❌ | Web 框架 |
| `lua-cjson` | ✅ | ❌ | JSON 库（含 .so） |
| `luaossl` | ✅ | ❌ | SSL 库（含 .so） |
| `lpeg` | ✅ | ❌ | 解析库（含 .so） |
| `luasocket` | ✅ | ❌ | 网络库（含 .so） |
| `openresty` | ✅ | ❌ | nginx+LuaJIT 运行时 |

### 4.2 精简运行时依赖

生产环境只需安装以下包：

```bash
# === 最小运行时依赖 ===

# 系统包
apt install -y luajit libluajit-5.1-2 libluajit-5.1-common
apt install -y libssl3t64 libpcre2-8-0 zlib1g
apt install -y luarocks

# LuaRocks 包（lapis 会拉取全部依赖）
luarocks install lapis

# 运行模式 A（推荐生产）
# 添加 OpenResty 源后: apt install openresty

# 运行模式 B（轻量开发）
luarocks install lua-http
luarocks install cqueues
luarocks install lua-websockets
```

---

## 5. 当前环境状态

### 5.1 已安装 ✅

| 组件 | 版本 | 状态 |
|------|------|:----:|
| Ubuntu | 24.04 (Noble) | ✅ |
| LuaJIT | 2.1.0+openresty20251030-1 | ✅ |
| LuaRocks | 3.8.0 | ✅ |
| Lua | 5.1.5 | ✅ |
| libluajit-5.1-2 | 2.1.0+openresty20251030-1 | ✅ |
| libluajit-5.1-common | 2.1.0+openresty20251030-1 | ✅ |
| libssl3t64 | 3.5.5-1ubuntu3.2 | ✅ |
| libssl-dev | 3.5.5-1ubuntu3.2 | ✅ |
| libpcre2-8-0 | 10.46-1build1 | ✅ |
| zlib1g | 1:1.3.dfsg+really1.3.1 | ✅ |
| zlib1g-dev | 1:1.3.dfsg+really1.3.1 | ✅ |
| build-essential | 12.12ubuntu2 | ✅ |
| lapis | 1.18.0-1 | ✅ |
| ansicolors | 1.0.2-3 | ✅ |
| argparse | 0.7.2-1 | ✅ |
| date | 2.2.1-2 | ✅ |
| etlua | 1.3.0-1 | ✅ |
| loadkit | 1.1.0-1 | ✅ |
| lpeg | 1.1.0-2 | ✅ |
| lua-cjson | 2.1.0.10-1 | ✅ |
| luaossl | 20250929-0 | ✅ |
| luasocket | 3.1.0-1 | ✅ |
| pgmoon | 1.17.0-1 | ✅ |

### 5.2 待安装 ❌

| 组件 | 用途 | 优先级 |
|------|------|:------:|
| **OpenResty** | Lapis 生产运行时（nginx+LuaJIT） | 🔴 高 |
| **libluajit-5.1-dev** | C 扩展编译头文件 | 🟡 中 |
| **libpcre2-dev** | PCRE2 开发头文件 | 🟡 中 |
| **lua-http** | 模式 B 的 HTTP 库 | 🟢 低 |
| **cqueues** | 模式 B 的异步 I/O | 🟢 低 |
| **lua-websockets** | 模式 B 的 WebSocket | 🟢 低 |

### 5.3 关键发现

1. **OpenResty 未安装。** 系统中没有 `/usr/local/openresty/`，`openresty` 命令不存在。Ubuntu 24.04 默认 APT 源中不包含 `openresty` 包，需要手动添加 OpenResty 官方仓库。

2. **LuaJIT 来自 OpenResty 维护分支。** 虽然完整 OpenResty 未安装，但 LuaJIT 使用的是 OpenResty 团队维护的版本（`2.1.0+openresty20251030-1`），这为后续安装 OpenResty 提供了兼容基础。

3. **所有 Lapis LuaRocks 依赖已就绪。** 11 个 LuaRocks 包全部安装完毕，Lapis 1.18.0 可直接使用。

4. **编译工具链完整。** build-essential、libssl-dev、zlib1g-dev 均已安装，可以编译任何 C 扩展 luarock。

5. **pgmoon 实际不需要。** 当前规格明确"不做数据库持久化"，pgmoon（PostgreSQL 客户端）可以移除，但不移除也不影响运行。

---

## 6. 一键安装脚本

### 6.1 完整安装脚本（生产环境 — OpenResty 模式）

```bash
#!/bin/bash
# ============================================================
# ChaosEngine Web 管理后台 — 一键安装脚本
# 适用于: Ubuntu 24.04 (Noble Numbat)
# 模式: OpenResty (生产推荐)
# ============================================================
set -euo pipefail

echo "=== ChaosEngine Admin 依赖安装 ==="
echo ""

# ---------- 1. 系统包 ----------
echo "[1/5] 安装系统依赖..."
sudo apt update -qq
sudo apt install -y \
    build-essential \
    libssl-dev \
    libpcre2-dev \
    zlib1g-dev \
    libluajit-5.1-dev \
    luajit \
    libluajit-5.1-2 \
    libluajit-5.1-common \
    libssl3t64 \
    libpcre2-8-0 \
    zlib1g \
    luarocks \
    wget \
    gnupg2 \
    lsb-release

# ---------- 2. OpenResty ----------
echo "[2/5] 安装 OpenResty..."
if ! command -v openresty &>/dev/null; then
    wget -qO - https://openresty.org/package/pubkey.gpg | sudo gpg --dearmor -o /usr/share/keyrings/openresty.gpg
    echo "deb [signed-by=/usr/share/keyrings/openresty.gpg] http://openresty.org/package/ubuntu $(lsb_release -sc) main" \
        | sudo tee /etc/apt/sources.list.d/openresty.list
    sudo apt update -qq
    sudo apt install -y openresty
    echo "OpenResty 安装完成: $(openresty -v 2>&1)"
else
    echo "OpenResty 已安装: $(openresty -v 2>&1)"
fi

# ---------- 3. LuaRocks 配置 ----------
echo "[3/5] 配置 LuaRocks..."
# 确保 luarocks 使用 LuaJIT
sudo luarocks config lua_version 5.1
sudo luarocks config lua_interpreter luajit

# ---------- 4. Lapis + 依赖 ----------
echo "[4/5] 安装 Lapis 及 Lua 依赖..."
sudo luarocks install lapis
# 可选：移除不需要的 pgmoon（不做数据库持久化）
# sudo luarocks remove pgmoon 2>/dev/null || true

# ---------- 5. 验证 ----------
echo "[5/5] 验证安装..."
echo ""
echo "--- 系统包 ---"
dpkg -l | grep -E "luajit|libluajit|libssl|libpcre|openresty|zlib" | awk '{printf "  %-40s %s\n", $2, $3}'
echo ""
echo "--- LuaRocks 包 ---"
luarocks list --porcelain 2>/dev/null | awk '{printf "  %-30s %s\n", $1, $2}'
echo ""
echo "--- 版本信息 ---"
echo "  LuaJIT:    $(luajit -v 2>&1)"
echo "  LuaRocks:  $(luarocks --version 2>&1 | head -1)"
echo "  OpenResty: $(openresty -v 2>&1 || echo '未安装')"
echo "  Lapis:     $(luarocks show lapis 2>&1 | grep '^lapis ' | awk '{print $2}')"
echo ""
echo "=== 安装完成 ==="
echo ""
echo "启动管理后台:"
echo "  cd /path/to/chaos-engine/src_lua/admin"
echo "  lapis server production"
```

### 6.2 轻量安装脚本（开发环境 — lua-http 模式）

```bash
#!/bin/bash
# ============================================================
# ChaosEngine Web 管理后台 — 轻量安装脚本
# 适用于: Ubuntu 24.04
# 模式: lua-http + cqueues (开发/测试推荐)
# ============================================================
set -euo pipefail

echo "=== ChaosEngine Admin 轻量依赖安装 ==="

# 系统包
sudo apt update -qq
sudo apt install -y \
    build-essential \
    libssl-dev \
    libluajit-5.1-dev \
    luajit \
    libluajit-5.1-2 \
    libluajit-5.1-common \
    libssl3t64 \
    luarocks

# 配置 LuaRocks
sudo luarocks config lua_version 5.1
sudo luarocks config lua_interpreter luajit

# Lapis + 依赖
sudo luarocks install lapis

# lua-http 模式额外依赖
sudo luarocks install lua-http
sudo luarocks install cqueues
sudo luarocks install lua-websockets

echo "=== 安装完成 ==="
echo "启动管理后台:"
echo "  cd /path/to/chaos-engine/src_lua/admin"
echo "  lapis server"
```

### 6.3 仅验证脚本

```bash
#!/bin/bash
# 验证 ChaosEngine Admin 依赖是否就绪
echo "=== 依赖验证 ==="
echo ""

MISSING=0

check_cmd() {
    if command -v "$1" &>/dev/null; then
        echo "  ✅ $1: $(command -v "$1")"
    else
        echo "  ❌ $1: 未找到"
        MISSING=$((MISSING + 1))
    fi
}

check_pkg() {
    if dpkg -l "$1" 2>/dev/null | grep -q '^ii'; then
        echo "  ✅ $1"
    else
        echo "  ❌ $1: 未安装"
        MISSING=$((MISSING + 1))
    fi
}

check_rock() {
    if luarocks list --porcelain 2>/dev/null | grep -q "^$1"; then
        echo "  ✅ $1"
    else
        echo "  ❌ $1: 未安装"
        MISSING=$((MISSING + 1))
    fi
}

echo "--- 命令行工具 ---"
check_cmd luajit
check_cmd luarocks
check_cmd openresty

echo ""
echo "--- 系统包 ---"
check_pkg libluajit-5.1-2
check_pkg libluajit-5.1-common
check_pkg libssl3t64
check_pkg libpcre2-8-0
check_pkg zlib1g

echo ""
echo "--- LuaRocks 包 ---"
check_rock lapis
check_rock lua-cjson
check_rock luaossl
check_rock luasocket
check_rock lpeg
check_rock etlua
check_rock date
check_rock loadkit
check_rock argparse
check_rock ansicolors

echo ""
if [ $MISSING -eq 0 ]; then
    echo "=== 全部依赖就绪 ✅ ==="
else
    echo "=== 缺少 $MISSING 个依赖 ❌ ==="
fi
```

---

## 附录：依赖来源与许可

| 包名 | 来源 | 许可 |
|------|------|------|
| OpenResty | openresty.org | BSD |
| LuaJIT | openresty.org (fork) | MIT |
| Lapis | leafo.net/lapis | MIT |
| lua-cjson | kyne.com.au | MIT |
| luaossl | 25thandclement.com | MIT |
| luasocket | lunarmodules/luasocket | MIT |
| lpeg | inf.puc-rio.br/~roberto/lpeg | MIT |
| pgmoon | leafo/pgmoon | MIT |
| etlua | leafo/etlua | MIT |
| date | Tieske/date | MIT |
| loadkit | leafo/loadkit | MIT |
| argparse | luarocks/argparse | MIT |
| ansicolors | kikito/ansicolors.lua | MIT |

> 所有依赖均为 MIT/BSD 许可，无商业使用限制。
