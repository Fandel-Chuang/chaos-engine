--[[
    dashboard_html.lua — Full Single-File HTML Dashboard

    Returns a complete HTML document with embedded CSS and JS.
    Dark theme, modern design, WebSocket real-time updates,
    Canvas scatter plot (AOI) and heatmap (Cell), log viewer,
    and all subsystem panels.

    Usage:
        local dashboard = require("admin.dashboard_html")
        return dashboard.get()  -- returns HTML string
]]

local M = {}

function M.get()
    return [[
<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>ChaosEngine 管理后台 v0.2</title>
<style>
/* ============================================================
   CSS Reset & Base
   ============================================================ */
*, *::before, *::after { box-sizing: border-box; margin: 0; padding: 0; }

:root {
    --bg-primary: #0d1117;
    --bg-secondary: #161b22;
    --bg-tertiary: #21262d;
    --bg-card: #1c2128;
    --border: #30363d;
    --text-primary: #e6edf3;
    --text-secondary: #8b949e;
    --text-muted: #6e7681;
    --accent: #58a6ff;
    --accent-green: #3fb950;
    --accent-yellow: #d29922;
    --accent-orange: #db6d28;
    --accent-red: #f85149;
    --accent-purple: #a371f7;
    --shadow: 0 1px 3px rgba(0,0,0,0.3);
    --radius: 8px;
    --transition: 0.2s ease;
}

body {
    font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', 'Noto Sans SC', sans-serif;
    background: var(--bg-primary);
    color: var(--text-primary);
    line-height: 1.5;
    min-height: 100vh;
    overflow-x: hidden;
}

/* ============================================================
   Header
   ============================================================ */
.header {
    background: var(--bg-secondary);
    border-bottom: 1px solid var(--border);
    padding: 12px 24px;
    display: flex;
    align-items: center;
    justify-content: space-between;
    position: sticky;
    top: 0;
    z-index: 100;
    backdrop-filter: blur(8px);
}
.header-left {
    display: flex;
    align-items: center;
    gap: 12px;
}
.header-logo {
    font-size: 1.4em;
    font-weight: 700;
}
.header-version {
    background: var(--accent);
    color: #fff;
    padding: 2px 8px;
    border-radius: 12px;
    font-size: 0.75em;
    font-weight: 600;
}
.header-right {
    display: flex;
    align-items: center;
    gap: 16px;
    font-size: 0.9em;
    color: var(--text-secondary);
}
.uptime { font-variant-numeric: tabular-nums; }
.connection-status {
    display: flex;
    align-items: center;
    gap: 6px;
}
.status-dot {
    width: 8px;
    height: 8px;
    border-radius: 50%;
    display: inline-block;
}
.status-dot.connected { background: var(--accent-green); box-shadow: 0 0 6px var(--accent-green); }
.status-dot.disconnected { background: var(--accent-red); box-shadow: 0 0 6px var(--accent-red); }

/* ============================================================
   Layout
   ============================================================ */
.container {
    max-width: 1400px;
    margin: 0 auto;
    padding: 20px 24px;
}

/* ============================================================
   Metric Cards Row
   ============================================================ */
.metrics-row {
    display: grid;
    grid-template-columns: repeat(6, 1fr);
    gap: 16px;
    margin-bottom: 20px;
}
.metric-card {
    background: var(--bg-card);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    padding: 16px 20px;
    transition: border-color var(--transition);
}
.metric-card:hover { border-color: var(--accent); }
.metric-label {
    font-size: 0.8em;
    color: var(--text-muted);
    text-transform: uppercase;
    letter-spacing: 0.5px;
    margin-bottom: 4px;
}
.metric-value {
    font-size: 1.8em;
    font-weight: 700;
    font-variant-numeric: tabular-nums;
    transition: color 0.3s ease;
}
.metric-value.updated { color: var(--accent); }
.metric-sub {
    font-size: 0.75em;
    color: var(--text-secondary);
    margin-top: 2px;
}

/* ============================================================
   Panels Grid
   ============================================================ */
.panels-grid {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 16px;
    margin-bottom: 20px;
}
.panel {
    background: var(--bg-card);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    overflow: hidden;
}
.panel-header {
    background: var(--bg-tertiary);
    padding: 10px 16px;
    font-weight: 600;
    font-size: 0.9em;
    border-bottom: 1px solid var(--border);
    display: flex;
    align-items: center;
    gap: 8px;
}
.panel-body {
    padding: 16px;
}

/* Full-width panels */
.panel-full {
    grid-column: 1 / -1;
}

/* ============================================================
   AOI Panel
   ============================================================ */
.aoi-stats {
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    gap: 12px;
    margin-bottom: 12px;
}
.aoi-stat {
    text-align: center;
    padding: 8px;
    background: var(--bg-tertiary);
    border-radius: 6px;
}
.aoi-stat-value { font-size: 1.3em; font-weight: 700; }
.aoi-stat-label { font-size: 0.7em; color: var(--text-muted); }
.aoi-canvas-wrap {
    width: 100%;
    height: 200px;
    background: var(--bg-primary);
    border-radius: 6px;
    overflow: hidden;
    margin-bottom: 12px;
}
.aoi-canvas-wrap canvas { width: 100%; height: 100%; }
.aoi-hot-list { max-height: 120px; overflow-y: auto; }
.aoi-hot-item {
    display: flex;
    justify-content: space-between;
    padding: 4px 8px;
    font-size: 0.8em;
    border-bottom: 1px solid var(--border);
}
.aoi-hot-item:last-child { border-bottom: none; }

/* ============================================================
   Cell Panel
   ============================================================ */
.cell-info {
    display: grid;
    grid-template-columns: repeat(4, 1fr);
    gap: 8px;
    margin-bottom: 12px;
}
.cell-info-item {
    text-align: center;
    padding: 6px;
    background: var(--bg-tertiary);
    border-radius: 6px;
    font-size: 0.8em;
}
.cell-info-value { font-weight: 700; font-size: 1.1em; }
.cell-canvas-wrap {
    width: 100%;
    height: 200px;
    background: var(--bg-primary);
    border-radius: 6px;
    overflow: hidden;
    margin-bottom: 12px;
}
.cell-canvas-wrap canvas { width: 100%; height: 100%; }
.cell-overload { margin-top: 8px; }
.cell-overload-title {
    font-size: 0.8em;
    color: var(--accent-red);
    margin-bottom: 4px;
}
.cell-overload-item {
    font-size: 0.75em;
    padding: 4px 8px;
    background: rgba(248,81,73,0.1);
    border-radius: 4px;
    margin-bottom: 4px;
}

/* ============================================================
   Network Panel
   ============================================================ */
.network-stats {
    display: grid;
    grid-template-columns: repeat(4, 1fr);
    gap: 12px;
}
.network-stat {
    text-align: center;
    padding: 12px 8px;
    background: var(--bg-tertiary);
    border-radius: 6px;
}
.network-stat-value { font-size: 1.3em; font-weight: 700; }
.network-stat-label { font-size: 0.7em; color: var(--text-muted); }
.network-backend {
    margin-top: 12px;
    padding: 8px 12px;
    background: var(--bg-tertiary);
    border-radius: 6px;
    font-size: 0.8em;
    display: flex;
    gap: 16px;
}
.network-backend span { color: var(--text-secondary); }
.network-backend .val { color: var(--accent-green); font-weight: 600; }

/* ============================================================
   Memory Panel
   ============================================================ */
.memory-stats {
    display: grid;
    grid-template-columns: repeat(4, 1fr);
    gap: 12px;
}
.memory-stat {
    text-align: center;
    padding: 12px 8px;
    background: var(--bg-tertiary);
    border-radius: 6px;
}
.memory-stat-value { font-size: 1.3em; font-weight: 700; }
.memory-stat-label { font-size: 0.7em; color: var(--text-muted); }
.memory-bar-wrap {
    margin-top: 12px;
    background: var(--bg-primary);
    border-radius: 4px;
    height: 8px;
    overflow: hidden;
}
.memory-bar {
    height: 100%;
    background: linear-gradient(90deg, var(--accent-green), var(--accent-yellow), var(--accent-red));
    border-radius: 4px;
    transition: width 0.5s ease;
}

/* ============================================================
   Render Panel
   ============================================================ */
.render-stats {
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    gap: 12px;
    margin-bottom: 12px;
}
.render-stat {
    text-align: center;
    padding: 12px 8px;
    background: var(--bg-tertiary);
    border-radius: 6px;
}
.render-stat-value { font-size: 1.3em; font-weight: 700; }
.render-stat-label { font-size: 0.7em; color: var(--text-muted); }
.render-timing {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 12px;
}
.render-timing-item {
    text-align: center;
    padding: 10px;
    background: var(--bg-tertiary);
    border-radius: 6px;
}
.render-timing-value { font-size: 1.1em; font-weight: 700; }
.render-timing-label { font-size: 0.7em; color: var(--text-muted); }

/* ============================================================
   Log Panel
   ============================================================ */
.log-container {
    max-height: 300px;
    overflow-y: auto;
    font-family: 'SF Mono', 'Cascadia Code', 'Fira Code', monospace;
    font-size: 0.78em;
    line-height: 1.6;
    background: var(--bg-primary);
    border-radius: 6px;
    padding: 8px;
}
.log-entry {
    padding: 2px 8px;
    border-radius: 3px;
    white-space: nowrap;
    overflow: hidden;
    text-overflow: ellipsis;
}
.log-entry:hover { background: var(--bg-tertiary); white-space: pre-wrap; overflow: visible; }
.log-time { color: var(--text-muted); margin-right: 8px; }
.log-level { font-weight: 600; margin-right: 8px; min-width: 48px; display: inline-block; }
.log-level.TRACE { color: #6e7681; }
.log-level.DEBUG { color: #58a6ff; }
.log-level.INFO  { color: #3fb950; }
.log-level.WARN  { color: #d29922; }
.log-level.ERROR { color: #f85149; }
.log-level.FATAL { color: #a371f7; }
.log-category { color: var(--text-secondary); margin-right: 8px; }
.log-message { color: var(--text-primary); }

/* ============================================================
   System Info Panel
   ============================================================ */
.system-grid {
    display: grid;
    grid-template-columns: repeat(4, 1fr);
    gap: 12px;
}
.system-item {
    padding: 10px 12px;
    background: var(--bg-tertiary);
    border-radius: 6px;
    font-size: 0.8em;
}
.system-label { color: var(--text-muted); font-size: 0.85em; }
.system-value { font-weight: 600; margin-top: 2px; }
.system-value.yes { color: var(--accent-green); }
.system-value.no { color: var(--accent-red); }

/* ============================================================
   Scrollbar
   ============================================================ */
::-webkit-scrollbar { width: 6px; height: 6px; }
::-webkit-scrollbar-track { background: var(--bg-primary); }
::-webkit-scrollbar-thumb { background: var(--border); border-radius: 3px; }
::-webkit-scrollbar-thumb:hover { background: var(--text-muted); }

/* ============================================================
   Animations
   ============================================================ */
@keyframes pulse {
    0%, 100% { opacity: 1; }
    50% { opacity: 0.5; }
}
@keyframes fadeIn {
    from { opacity: 0; transform: translateY(4px); }
    to { opacity: 1; transform: translateY(0); }
}
.log-entry { animation: fadeIn 0.2s ease; }

/* ============================================================
   Responsive
   ============================================================ */
@media (max-width: 1024px) {
    .metrics-row { grid-template-columns: repeat(3, 1fr); }
    .panels-grid { grid-template-columns: 1fr; }
    .system-grid { grid-template-columns: repeat(2, 1fr); }
}
@media (max-width: 640px) {
    .metrics-row { grid-template-columns: repeat(2, 1fr); }
    .aoi-stats, .cell-info, .network-stats, .memory-stats, .render-stats { grid-template-columns: repeat(2, 1fr); }
}
</style>
</head>
<body>

<!-- ============================================================
     Header
     ============================================================ -->
<header class="header">
    <div class="header-left">
        <span class="header-logo">🔥 ChaosEngine 管理后台</span>
        <span class="header-version">v0.2</span>
    </div>
    <div class="header-right">
        <span class="uptime">运行 <strong id="uptime">--</strong></span>
        <span class="connection-status">
            <span class="status-dot disconnected" id="status-dot"></span>
            <span id="status-text">未连接</span>
        </span>
    </div>
</header>

<div class="container">

<!-- ============================================================
     Metric Cards
     ============================================================ -->
<div class="metrics-row">
    <div class="metric-card">
        <div class="metric-label">实体数</div>
        <div class="metric-value" id="metric-entities">--</div>
        <div class="metric-sub" id="metric-entities-sub"></div>
    </div>
    <div class="metric-card">
        <div class="metric-label">连接数</div>
        <div class="metric-value" id="metric-connections">--</div>
        <div class="metric-sub">活跃连接</div>
    </div>
    <div class="metric-card">
        <div class="metric-label">FPS</div>
        <div class="metric-value" id="metric-fps">--</div>
        <div class="metric-sub" id="metric-fps-sub"></div>
    </div>
    <div class="metric-card">
        <div class="metric-label">CPU</div>
        <div class="metric-value" id="metric-cpu">--</div>
        <div class="metric-sub" id="metric-cpu-sub"></div>
    </div>
    <div class="metric-card">
        <div class="metric-label">内存</div>
        <div class="metric-value" id="metric-memory">--</div>
        <div class="metric-sub" id="metric-memory-sub"></div>
    </div>
    <div class="metric-card">
        <div class="metric-label">网络流量</div>
        <div class="metric-value" id="metric-traffic">--</div>
        <div class="metric-sub" id="metric-traffic-sub"></div>
    </div>
</div>

<!-- ============================================================
     Panels: AOI + Cell
     ============================================================ -->
<div class="panels-grid">
    <!-- AOI Panel -->
    <div class="panel">
        <div class="panel-header">📍 AOI 十字链表</div>
        <div class="panel-body">
            <div class="aoi-stats">
                <div class="aoi-stat">
                    <div class="aoi-stat-value" id="aoi-entities">--</div>
                    <div class="aoi-stat-label">实体数</div>
                </div>
                <div class="aoi-stat">
                    <div class="aoi-stat-value" id="aoi-radius">--</div>
                    <div class="aoi-stat-label">AOI 半径</div>
                </div>
                <div class="aoi-stat">
                    <div class="aoi-stat-value" id="aoi-events">--</div>
                    <div class="aoi-stat-label">事件/秒</div>
                </div>
            </div>
            <div class="aoi-canvas-wrap">
                <canvas id="aoi-canvas"></canvas>
            </div>
            <div style="font-size:0.8em;color:var(--text-muted);margin-bottom:4px;">🔥 热点实体 Top-5</div>
            <div class="aoi-hot-list" id="aoi-hot-list">
                <div style="color:var(--text-muted);text-align:center;padding:20px;">等待数据...</div>
            </div>
        </div>
    </div>

    <!-- Cell Panel -->
    <div class="panel">
        <div class="panel-header">🔲 Cell 网格热力图</div>
        <div class="panel-body">
            <div class="cell-info">
                <div class="cell-info-item">
                    <div class="cell-info-value" id="cell-grid">--</div>
                    <div>网格</div>
                </div>
                <div class="cell-info-item">
                    <div class="cell-info-value" id="cell-total">--</div>
                    <div>总 Cell</div>
                </div>
                <div class="cell-info-item">
                    <div class="cell-info-value" id="cell-active">--</div>
                    <div>活跃</div>
                </div>
                <div class="cell-info-item">
                    <div class="cell-info-value" id="cell-overloaded">--</div>
                    <div>过载</div>
                </div>
            </div>
            <div class="cell-canvas-wrap">
                <canvas id="cell-canvas"></canvas>
            </div>
            <div class="cell-overload" id="cell-overload-section" style="display:none;">
                <div class="cell-overload-title">⚠️ 过载 Cell</div>
                <div id="cell-overload-list"></div>
            </div>
        </div>
    </div>

    <!-- Network Panel -->
    <div class="panel">
        <div class="panel-header">🌐 网络统计</div>
        <div class="panel-body">
            <div class="network-stats">
                <div class="network-stat">
                    <div class="network-stat-value" id="net-connections">--</div>
                    <div class="network-stat-label">连接数</div>
                </div>
                <div class="network-stat">
                    <div class="network-stat-value" id="net-bytes-in">--</div>
                    <div class="network-stat-label">接收</div>
                </div>
                <div class="network-stat">
                    <div class="network-stat-value" id="net-bytes-out">--</div>
                    <div class="network-stat-label">发送</div>
                </div>
                <div class="network-stat">
                    <div class="network-stat-value" id="net-retrans">--</div>
                    <div class="network-stat-label">重传</div>
                </div>
            </div>
            <div class="network-backend" id="net-backend" style="display:none;">
                <span>后端: <span class="val" id="net-backend-name">--</span></span>
                <span>ZCRX: <span class="val" id="net-zcrx">--</span></span>
            </div>
        </div>
    </div>

    <!-- Memory Panel -->
    <div class="panel">
        <div class="panel-header">💾 内存使用</div>
        <div class="panel-body">
            <div class="memory-stats">
                <div class="memory-stat">
                    <div class="memory-stat-value" id="mem-used">--</div>
                    <div class="memory-stat-label">RSS 物理内存</div>
                </div>
                <div class="memory-stat">
                    <div class="memory-stat-value" id="mem-peak">--</div>
                    <div class="memory-stat-label">峰值</div>
                </div>
                <div class="memory-stat">
                    <div class="memory-stat-value" id="mem-heap">--</div>
                    <div class="memory-stat-label">堆 (Data)</div>
                </div>
                <div class="memory-stat">
                    <div class="memory-stat-value" id="mem-stack">--</div>
                    <div class="memory-stat-label">栈</div>
                </div>
            </div>
            <div class="memory-bar-wrap">
                <div class="memory-bar" id="memory-bar" style="width:0%;"></div>
            </div>
            <div style="font-size:0.7em;color:var(--text-muted);margin-top:4px;text-align:right;" id="mem-percent">0%</div>
            <div style="font-size:0.7em;color:var(--text-muted);margin-top:2px;" id="mem-virtual"></div>
        </div>
    </div>

    <!-- CPU Panel -->
    <div class="panel">
        <div class="panel-header">🔥 CPU 占用</div>
        <div class="panel-body">
            <div class="memory-stats">
                <div class="memory-stat">
                    <div class="memory-stat-value" id="cpu-machine-pct">--</div>
                    <div class="memory-stat-label">进程 CPU%</div>
                </div>
                <div class="memory-stat">
                    <div class="memory-stat-value" id="cpu-machine-total">--</div>
                    <div class="memory-stat-label">机器 CPU%</div>
                </div>
                <div class="memory-stat">
                    <div class="memory-stat-value" id="cpu-cores">--</div>
                    <div class="memory-stat-label">核心数</div>
                </div>
                <div class="memory-stat">
                    <div class="memory-stat-value" id="cpu-proc-seconds">--</div>
                    <div class="memory-stat-label">进程 CPU 时间</div>
                </div>
                <div class="memory-stat">
                    <div class="memory-stat-value" id="cpu-io-wait">--</div>
                    <div class="memory-stat-label">IO Wait</div>
                </div>
            </div>
            <div style="margin-top:12px;">
                <div style="font-size:0.8em;color:var(--text-secondary);margin-bottom:4px;">机器级 CPU 分布</div>
                <div style="display:flex;height:24px;border-radius:6px;overflow:hidden;background:var(--bg-primary);">
                    <div id="cpu-bar-user" style="background:var(--accent);height:100%;transition:width 0.5s;"></div>
                    <div id="cpu-bar-system" style="background:var(--accent-purple);height:100%;transition:width 0.5s;"></div>
                    <div id="cpu-bar-iowait" style="background:var(--accent-orange);height:100%;transition:width 0.5s;"></div>
                    <div id="cpu-bar-idle" style="background:var(--bg-tertiary);height:100%;flex:1;"></div>
                </div>
                <div style="display:flex;gap:12px;margin-top:6px;font-size:0.7em;color:var(--text-muted);">
                    <span>🟦 User</span>
                    <span>🟪 System</span>
                    <span>🟧 IO Wait</span>
                    <span>⬛ Idle</span>
                </div>
            </div>
            <div style="margin-top:12px;font-size:0.75em;color:var(--text-muted);" id="cpu-process-detail"></div>
        </div>
    </div>

    <!-- Render Panel -->
    <div class="panel">
        <div class="panel-header">🎮 渲染统计</div>
        <div class="panel-body">
            <div class="render-stats">
                <div class="render-stat">
                    <div class="render-stat-value" id="render-drawcalls">--</div>
                    <div class="render-stat-label">Draw Calls</div>
                </div>
                <div class="render-stat">
                    <div class="render-stat-value" id="render-triangles">--</div>
                    <div class="render-stat-label">Triangles</div>
                </div>
                <div class="render-stat">
                    <div class="render-stat-value" id="render-vertices">--</div>
                    <div class="render-stat-label">Vertices</div>
                </div>
            </div>
            <div class="render-timing">
                <div class="render-timing-item">
                    <div class="render-timing-value" id="render-frame-time">--</div>
                    <div class="render-timing-label">Frame Time (CPU)</div>
                </div>
                <div class="render-timing-item">
                    <div class="render-timing-value" id="render-gpu-time">--</div>
                    <div class="render-timing-label">GPU Time</div>
                </div>
            </div>
            <div style="font-size:0.75em;color:var(--text-muted);margin-top:8px;text-align:center;" id="render-backend"></div>
        </div>
    </div>

    <!-- Log Panel -->
    <div class="panel panel-full">
        <div class="panel-header">📋 最近日志 <span style="font-size:0.75em;color:var(--text-muted);margin-left:8px;">(WebSocket 实时推送)</span></div>
        <div class="panel-body" style="padding:8px;">
            <div class="log-container" id="log-container">
                <div style="color:var(--text-muted);text-align:center;padding:20px;">等待日志数据...</div>
            </div>
        </div>
    </div>

    <!-- System Info Panel -->
    <div class="panel panel-full">
        <div class="panel-header">⚙️ 系统信息</div>
        <div class="panel-body">
            <div class="system-grid">
                <div class="system-item">
                    <div class="system-label">引擎版本</div>
                    <div class="system-value" id="sys-version">--</div>
                </div>
                <div class="system-item">
                    <div class="system-label">编译模式</div>
                    <div class="system-value" id="sys-build-mode">--</div>
                </div>
                <div class="system-item">
                    <div class="system-label">I/O 后端</div>
                    <div class="system-value" id="sys-io-backend">--</div>
                </div>
                <div class="system-item">
                    <div class="system-label">eBPF</div>
                    <div class="system-value" id="sys-ebpf">--</div>
                </div>
                <div class="system-item">
                    <div class="system-label">ZCRX</div>
                    <div class="system-value" id="sys-zcrx">--</div>
                </div>
                <div class="system-item">
                    <div class="system-label">编译器</div>
                    <div class="system-value" id="sys-compiler">--</div>
                </div>
                <div class="system-item">
                    <div class="system-label">平台</div>
                    <div class="system-value" id="sys-platform">--</div>
                </div>
                <div class="system-item">
                    <div class="system-label">PID</div>
                    <div class="system-value" id="sys-pid">--</div>
                </div>
            </div>
        </div>
    </div>
</div>

</div><!-- .container -->

<script>
// ============================================================
// Utility Functions
// ============================================================
function $(id) { return document.getElementById(id); }

function formatBytes(bytes) {
    if (bytes == null || bytes === 0) return '0 B';
    var k = 1024;
    var sizes = ['B', 'KB', 'MB', 'GB'];
    var i = Math.floor(Math.log(bytes) / Math.log(k));
    i = Math.min(i, sizes.length - 1);
    return (bytes / Math.pow(k, i)).toFixed(1) + ' ' + sizes[i];
}

function formatNumber(n) {
    if (n == null) return '--';
    return n.toString().replace(/\B(?=(\d{3})+(?!\d))/g, ',');
}

function formatUptime(seconds) {
    if (seconds == null) return '--';
    var h = Math.floor(seconds / 3600);
    var m = Math.floor((seconds % 3600) / 60);
    var s = Math.floor(seconds % 60);
    if (h > 0) return h + 'h ' + m + 'm ' + s + 's';
    if (m > 0) return m + 'm ' + s + 's';
    return s + 's';
}

function animateValue(el, newVal, formatter) {
    formatter = formatter || formatNumber;
    var oldText = el.textContent;
    var newText = formatter(newVal);
    if (oldText !== newText) {
        el.textContent = newText;
        el.classList.add('updated');
        setTimeout(function() { el.classList.remove('updated'); }, 300);
    }
}

// ============================================================
// WebSocket Connection
// ============================================================
var ws = null;
var reconnectTimer = null;
var logBuffer = [];
var MAX_LOG_LINES = 200;

function connectWebSocket() {
    var protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
    var wsUrl = protocol + '//' + location.host + '/ws';

    try {
        ws = new WebSocket(wsUrl);
    } catch(e) {
        setConnectionStatus(false);
        scheduleReconnect();
        return;
    }

    ws.onopen = function() {
        setConnectionStatus(true);
        console.log('WebSocket connected');
    };

    ws.onmessage = function(event) {
        try {
            var msg = JSON.parse(event.data);
            handleMessage(msg);
        } catch(e) {
            console.error('Failed to parse WS message:', e);
        }
    };

    ws.onclose = function() {
        setConnectionStatus(false);
        ws = null;
        scheduleReconnect();
    };

    ws.onerror = function(err) {
        console.error('WebSocket error:', err);
        ws = null;
        setConnectionStatus(false);
        scheduleReconnect();
    };
}

function scheduleReconnect() {
    if (reconnectTimer) return;
    reconnectTimer = setTimeout(function() {
        reconnectTimer = null;
        console.log('Reconnecting WebSocket...');
        connectWebSocket();
    }, 3000);
}

function setConnectionStatus(connected) {
    var dot = $('status-dot');
    var text = $('status-text');
    if (connected) {
        dot.className = 'status-dot connected';
        text.textContent = '已连接';
    } else {
        dot.className = 'status-dot disconnected';
        text.textContent = '未连接';
    }
}

// ============================================================
// Message Handler
// ============================================================
function handleMessage(msg) {
    switch (msg.type) {
        case 'stats':   updateStats(msg.data); break;
        case 'aoi':     updateAoi(msg.data); break;
        case 'cell':    updateCell(msg.data); break;
        case 'network': updateNetwork(msg.data); break;
        case 'memory':  updateMemory(msg.data); break;
        case 'cpu':     updateCpu(msg.data); break;
        case 'render':  updateRender(msg.data); break;
        case 'log':     appendLog(msg.data); break;
        case 'system':  updateSystem(msg.data); break;
    }
}

// ============================================================
// Stats Panel
// ============================================================
function updateStats(data) {
    if (!data) return;
    if (data.entity_count != null) {
        animateValue($('metric-entities'), data.entity_count);
        $('metric-entities-sub').textContent =
            (data.component_count || 0) + ' 组件';
    }
    if (data.fps != null) {
        animateValue($('metric-fps'), Math.round(data.fps));
        if (data.frame_time_us != null) {
            $('metric-fps-sub').textContent = (data.frame_time_us / 1000).toFixed(2) + ' ms/帧';
        }
    }
    if (data.uptime != null) {
        $('uptime').textContent = formatUptime(data.uptime);
    }
}

// ============================================================
// AOI Panel
// ============================================================
function updateAoi(data) {
    if (!data) return;
    animateValue($('aoi-entities'), data.entity_count);
    if (data.aoi_radius != null) {
        $('aoi-radius').textContent = data.aoi_radius.toFixed(1);
    }
    if (data.events) {
        var total = (data.events.enter || 0) + (data.events.leave || 0) + (data.events.move || 0);
        $('aoi-events').textContent = formatNumber(total);
    }

    // Hot entities list
    if (data.top_entities && data.top_entities.length > 0) {
        var html = '';
        data.top_entities.forEach(function(e) {
            html += '<div class="aoi-hot-item">' +
                '<span>Entity #' + e.id + '</span>' +
                '<span style="color:var(--text-muted);">(' + (e.x||0).toFixed(0) + ', ' + (e.y||0).toFixed(0) + ')</span>' +
                '<span style="color:var(--accent-yellow);">附近: ' + (e.nearby || 0) + '</span>' +
                '</div>';
        });
        $('aoi-hot-list').innerHTML = html;
    }

    // Canvas scatter plot
    drawAoiScatter(data);
}

function drawAoiScatter(data) {
    var canvas = $('aoi-canvas');
    var ctx = canvas.getContext('2d');
    var dpr = window.devicePixelRatio || 1;
    var rect = canvas.parentElement.getBoundingClientRect();
    canvas.width = rect.width * dpr;
    canvas.height = rect.height * dpr;
    ctx.scale(dpr, dpr);
    var w = rect.width;
    var h = rect.height;

    ctx.clearRect(0, 0, w, h);

    // Background grid
    ctx.strokeStyle = 'rgba(48,54,61,0.3)';
    ctx.lineWidth = 0.5;
    for (var x = 0; x < w; x += 40) {
        ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke();
    }
    for (var y = 0; y < h; y += 40) {
        ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
    }

    if (!data.top_entities || data.top_entities.length === 0) {
        ctx.fillStyle = '#6e7681';
        ctx.font = '12px sans-serif';
        ctx.textAlign = 'center';
        ctx.fillText('无实体数据', w/2, h/2);
        return;
    }

    // Find bounds
    var minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
    data.top_entities.forEach(function(e) {
        if (e.x < minX) minX = e.x;
        if (e.x > maxX) maxX = e.x;
        if (e.y < minY) minY = e.y;
        if (e.y > maxY) maxY = e.y;
    });

    // Expand bounds slightly
    var rangeX = maxX - minX || 100;
    var rangeY = maxY - minY || 100;
    var padX = rangeX * 0.1;
    var padY = rangeY * 0.1;
    minX -= padX; maxX += padX;
    minY -= padY; maxY += padY;
    rangeX = maxX - minX;
    rangeY = maxY - minY;

    var margin = 20;
    var plotW = w - margin * 2;
    var plotH = h - margin * 2;

    function toScreenX(x) { return margin + ((x - minX) / rangeX) * plotW; }
    function toScreenY(y) { return margin + ((y - minY) / rangeY) * plotH; }

    // Draw entities
    data.top_entities.forEach(function(e) {
        var sx = toScreenX(e.x);
        var sy = toScreenY(e.y);
        var radius = Math.max(3, Math.min(10, (e.nearby || 1) * 0.5));

        // Glow
        var gradient = ctx.createRadialGradient(sx, sy, 0, sx, sy, radius * 2);
        gradient.addColorStop(0, 'rgba(88,166,255,0.4)');
        gradient.addColorStop(1, 'rgba(88,166,255,0)');
        ctx.fillStyle = gradient;
        ctx.beginPath(); ctx.arc(sx, sy, radius * 2, 0, Math.PI * 2); ctx.fill();

        // Dot
        ctx.fillStyle = '#58a6ff';
        ctx.beginPath(); ctx.arc(sx, sy, radius, 0, Math.PI * 2); ctx.fill();
        ctx.strokeStyle = '#fff';
        ctx.lineWidth = 1;
        ctx.stroke();
    });
}

// ============================================================
// Cell Panel
// ============================================================
function updateCell(data) {
    if (!data) return;
    $('cell-grid').textContent = data.grid || '--';
    $('cell-total').textContent = formatNumber(data.total_cells);
    $('cell-active').textContent = formatNumber(data.active_cells);

    // Overloaded cells
    var overloaded = data.overloaded_cells ? data.overloaded_cells.length : 0;
    $('cell-overloaded').textContent = overloaded;
    if (overloaded > 0) {
        $('cell-overloaded').style.color = 'var(--accent-red)';
        $('cell-overload-section').style.display = 'block';
        var html = '';
        data.overloaded_cells.forEach(function(c) {
            html += '<div class="cell-overload-item">' +
                'Cell #' + c.id + ': ' + c.entities + ' / ' + c.max + ' 实体' +
                (c.bounds ? ' [' + c.bounds.join(',') + ']' : '') +
                '</div>';
        });
        $('cell-overload-list').innerHTML = html;
    } else {
        $('cell-overloaded').style.color = '';
        $('cell-overload-section').style.display = 'none';
    }

    // Canvas heatmap
    drawCellHeatmap(data);
}

function drawCellHeatmap(data) {
    var canvas = $('cell-canvas');
    var ctx = canvas.getContext('2d');
    var dpr = window.devicePixelRatio || 1;
    var rect = canvas.parentElement.getBoundingClientRect();
    canvas.width = rect.width * dpr;
    canvas.height = rect.height * dpr;
    ctx.scale(dpr, dpr);
    var w = rect.width;
    var h = rect.height;

    ctx.clearRect(0, 0, w, h);

    if (!data.cells || data.cells.length === 0) {
        ctx.fillStyle = '#6e7681';
        ctx.font = '12px sans-serif';
        ctx.textAlign = 'center';
        ctx.fillText('无 Cell 数据', w/2, h/2);
        return;
    }

    // Determine grid dimensions
    var maxX = 0, maxY = 0;
    data.cells.forEach(function(c) {
        if (c.x > maxX) maxX = c.x;
        if (c.y > maxY) maxY = c.y;
    });
    var cols = maxX + 1;
    var rows = maxY + 1;

    var margin = 10;
    var cellW = (w - margin * 2) / Math.max(cols, 1);
    var cellH = (h - margin * 2) / Math.max(rows, 1);
    var cellSize = Math.min(cellW, cellH);

    // Find max entities for color scaling
    var maxEntities = 0;
    data.cells.forEach(function(c) {
        if (c.entities > maxEntities) maxEntities = c.entities;
    });
    maxEntities = Math.max(maxEntities, 1);

    data.cells.forEach(function(c) {
        var cx = margin + c.x * cellSize;
        var cy = margin + c.y * cellSize;
        var ratio = c.entities / maxEntities;

        // Color: green -> yellow -> red
        var r, g, b;
        if (ratio < 0.5) {
            // Green to Yellow
            var t = ratio / 0.5;
            r = Math.floor(63 * t + 210 * (1 - t));
            g = Math.floor(185 * t + 210 * (1 - t));
            b = Math.floor(80 * t + 80 * (1 - t));
        } else {
            // Yellow to Red
            var t = (ratio - 0.5) / 0.5;
            r = Math.floor(248 * t + 63 * (1 - t));
            g = Math.floor(81 * t + 185 * (1 - t));
            b = Math.floor(73 * t + 80 * (1 - t));
        }

        ctx.fillStyle = 'rgb(' + r + ',' + g + ',' + b + ')';
        ctx.fillRect(cx + 1, cy + 1, cellSize - 2, cellSize - 2);

        // Border
        ctx.strokeStyle = 'rgba(13,17,23,0.5)';
        ctx.lineWidth = 0.5;
        ctx.strokeRect(cx + 1, cy + 1, cellSize - 2, cellSize - 2);

        // Entity count text (if cell is big enough)
        if (cellSize > 30) {
            ctx.fillStyle = ratio > 0.6 ? '#fff' : '#0d1117';
            ctx.font = Math.max(8, cellSize * 0.35) + 'px sans-serif';
            ctx.textAlign = 'center';
            ctx.textBaseline = 'middle';
            ctx.fillText(c.entities, cx + cellSize/2, cy + cellSize/2);
        }
    });
}

// ============================================================
// Network Panel
// ============================================================
function updateNetwork(data) {
    if (!data) return;
    animateValue($('net-connections'), data.connections);
    $('net-bytes-in').textContent = formatBytes(data.bytes_in);
    $('net-bytes-out').textContent = formatBytes(data.bytes_out);
    animateValue($('net-retrans'), data.retransmits);

    // Update traffic metric card with rate
    var rateInStr = data.rate_in != null ? formatBytes(data.rate_in) + '/s' : '--';
    var rateOutStr = data.rate_out != null ? formatBytes(data.rate_out) + '/s' : '--';
    $('metric-traffic').textContent = '↑' + rateOutStr;
    $('metric-traffic-sub').textContent = '↓' + rateInStr;
    animateValue($('metric-connections'), data.connections);

    // Backend info
    if (data.backend) {
        $('net-backend').style.display = 'flex';
        $('net-backend-name').textContent = data.backend;
        $('net-zcrx').textContent = data.zcrx ? '✅' : '❌';
    }

    // Peak rates
    var peakIn = data.peak_in_rate || 0;
    var peakOut = data.peak_out_rate || 0;
    if (peakIn > 0 || peakOut > 0) {
        $('metric-traffic-sub').textContent +=
            ' | 峰值 ↑' + formatBytes(peakOut) + '/s ↓' + formatBytes(peakIn) + '/s';
    }
}

// ============================================================
// Memory Panel
// ============================================================
function updateMemory(data) {
    if (!data) return;
    $('mem-used').textContent = formatBytes(data.used);
    $('mem-peak').textContent = formatBytes(data.peak);
    $('mem-heap').textContent = formatBytes(data.heap);
    $('mem-stack').textContent = formatBytes(data.stack);

    // Update metric card
    $('metric-memory').textContent = formatBytes(data.used);
    if (data.peak) {
        var pct = data.peak > 0 ? ((data.used / data.peak) * 100).toFixed(1) : 0;
        $('metric-memory-sub').textContent = '峰值 ' + formatBytes(data.peak) + ' (' + pct + '%)';
    }

    // Memory bar
    if (data.peak && data.peak > 0) {
        var pct = Math.min(100, (data.used / data.peak) * 100);
        $('memory-bar').style.width = pct + '%';
        $('mem-percent').textContent = pct.toFixed(1) + '%';
    }

    // Virtual memory info
    if (data.virtual) {
        $('mem-virtual').textContent = '虚拟内存: ' + formatBytes(data.virtual);
    }
}

// ============================================================
// CPU Panel
// ============================================================
function updateCpu(data) {
    if (!data) return;

    // Machine-level CPU percentage
    if (data.machine_pct != null) {
        $('cpu-machine-total').textContent = data.machine_pct.toFixed(1) + '%';
    }

    // Process CPU percentage (delta-based)
    if (data.process_pct != null) {
        $('metric-cpu').textContent = data.process_pct.toFixed(1) + '%';
        $('metric-cpu-sub').textContent = '机器 ' + (data.machine_pct || 0).toFixed(1) + '%';
        $('cpu-machine-pct').textContent = data.process_pct.toFixed(1) + '%';
    }

    // CPU cores
    if (data.cpu_cores != null) {
        $('cpu-cores').textContent = data.cpu_cores + ' 核';
        $('metric-cpu-sub').textContent = data.cpu_cores + ' 核心';
    }

    // Process CPU time
    if (data.process_cpu_seconds != null) {
        var secs = data.process_cpu_seconds;
        if (secs < 60) {
            $('cpu-proc-seconds').textContent = secs.toFixed(1) + 's';
        } else if (secs < 3600) {
            $('cpu-proc-seconds').textContent = (secs / 60).toFixed(1) + 'm';
        } else {
            $('cpu-proc-seconds').textContent = (secs / 3600).toFixed(1) + 'h';
        }
    }

    // IO Wait
    if (data.machine_iowait != null) {
        var total = (data.machine_user || 0) + (data.machine_system || 0) + (data.machine_idle || 0) + (data.machine_iowait || 0);
        if (total > 0) {
            var iowaitPct = ((data.machine_iowait / total) * 100).toFixed(1);
            $('cpu-io-wait').textContent = iowaitPct + '%';
        }
    }

    // CPU distribution bar
    if (data.machine_user != null && data.machine_system != null && data.machine_idle != null) {
        var total = data.machine_user + data.machine_system + data.machine_idle + (data.machine_iowait || 0);
        if (total > 0) {
            var userPct = ((data.machine_user / total) * 100).toFixed(1);
            var sysPct = ((data.machine_system / total) * 100).toFixed(1);
            var ioPct = (((data.machine_iowait || 0) / total) * 100).toFixed(1);
            $('cpu-bar-user').style.width = userPct + '%';
            $('cpu-bar-system').style.width = sysPct + '%';
            $('cpu-bar-iowait').style.width = ioPct + '%';
        }
    }

    // Process detail
    if (data.process_user_sec != null && data.process_sys_sec != null) {
        $('cpu-process-detail').textContent =
            '进程 CPU: user=' + data.process_user_sec.toFixed(2) + 's, sys=' + data.process_sys_sec.toFixed(2) + 's';
    }
}

// ============================================================
// Render Panel
// ============================================================
function updateRender(data) {
    if (!data) return;
    animateValue($('render-drawcalls'), data.draw_calls);
    animateValue($('render-triangles'), data.triangles);
    animateValue($('render-vertices'), data.vertices);

    if (data.frame_time_ms != null) {
        $('render-frame-time').textContent = data.frame_time_ms.toFixed(2) + ' ms';
    }
    if (data.gpu_time_ms != null) {
        $('render-gpu-time').textContent = data.gpu_time_ms.toFixed(2) + ' ms';
    }
    if (data.backend) {
        $('render-backend').textContent = '渲染后端: ' + data.backend;
    }
}

// ============================================================
// Log Panel (增量轮询)
// ============================================================
var lastLogTimestamp = 0;  // 上次拉取的最大时间戳

function appendLog(entry) {
    if (!entry) return;

    var entries = Array.isArray(entry) ? entry : [entry];

    entries.forEach(function(log) {
        logBuffer.push(log);
        if (logBuffer.length > MAX_LOG_LINES) {
            logBuffer.shift();
        }

        var div = document.createElement('div');
        div.className = 'log-entry';

        var time = log.timestamp || log.time || '';
        var level = (log.level || 'INFO').toUpperCase();
        var category = log.category || log.tag || '';
        var message = log.message || log.msg || JSON.stringify(log);

        div.innerHTML =
            '<span class="log-time">' + escapeHtml(time) + '</span>' +
            '<span class="log-level ' + level + '">[' + escapeHtml(level) + ']</span>' +
            (category ? '<span class="log-category">' + escapeHtml(category) + '</span>' : '') +
            '<span class="log-message">' + escapeHtml(message) + '</span>';

        var container = $('log-container');
        var placeholder = container.querySelector('div[style]');
        if (placeholder && logBuffer.length === 1) {
            container.innerHTML = '';
        }
        container.appendChild(div);
        container.scrollTop = container.scrollHeight;
    });
}

function fetchLogsIncremental() {
    var url = '/api/log?lines=50';
    if (lastLogTimestamp > 0) {
        url += '&since_us=' + lastLogTimestamp;
    }
    fetch(url)
        .then(function(r) { return r.json(); })
        .then(function(resp) {
            if (resp.ok && resp.data) {
                if (resp.data.max_timestamp_us) {
                    lastLogTimestamp = resp.data.max_timestamp_us;
                }
                if (resp.data.entries && resp.data.entries.length > 0) {
                    appendLog(resp.data.entries);
                }
            }
        })
        .catch(function() {});
}

function escapeHtml(str) {
    if (!str) return '';
    return String(str)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;');
}

// ============================================================
// System Panel
// ============================================================
function updateSystem(data) {
    if (!data) return;
    $('sys-version').textContent = data.engine_version || '--';
    $('sys-build-mode').textContent = data.build_mode || '--';
    $('sys-io-backend').textContent = data.io_backend || '--';

    var ebpfEl = $('sys-ebpf');
    ebpfEl.textContent = data.ebpf_available ? '✅ 可用' : '❌ 不可用';
    ebpfEl.className = 'system-value ' + (data.ebpf_available ? 'yes' : 'no');

    var zcrxEl = $('sys-zcrx');
    zcrxEl.textContent = data.zcrx_supported ? '✅ 支持' : '❌ 不支持';
    zcrxEl.className = 'system-value ' + (data.zcrx_supported ? 'yes' : 'no');

    $('sys-compiler').textContent = data.compiler || '--';
    $('sys-platform').textContent = data.platform || '--';
    $('sys-pid').textContent = data.pid || '--';
}

// ============================================================
// Initialization
// ============================================================
var pollingActive = false;
var pollingTimer = null;

function startPolling() {
    if (pollingActive) return;
    pollingActive = true;

    function poll() {
        if (ws && ws.readyState === WebSocket.OPEN) return; // WS active, skip

        fetch('/api/stats').then(function(r) { return r.json(); })
            .then(function(resp) { if (resp.ok && resp.data) updateStats(resp.data); })
            .catch(function() {});
        fetch('/api/aoi').then(function(r) { return r.json(); })
            .then(function(resp) { if (resp.ok && resp.data) updateAoi(resp.data); })
            .catch(function() {});
        fetch('/api/cell').then(function(r) { return r.json(); })
            .then(function(resp) { if (resp.ok && resp.data) updateCell(resp.data); })
            .catch(function() {});
        fetch('/api/network').then(function(r) { return r.json(); })
            .then(function(resp) { if (resp.ok && resp.data) updateNetwork(resp.data); })
            .catch(function() {});
        fetch('/api/memory').then(function(r) { return r.json(); })
            .then(function(resp) { if (resp.ok && resp.data) updateMemory(resp.data); })
            .catch(function() {});
        fetch('/api/cpu').then(function(r) { return r.json(); })
            .then(function(resp) { if (resp.ok && resp.data) updateCpu(resp.data); })
            .catch(function() {});
        fetch('/api/render').then(function(r) { return r.json(); })
            .then(function(resp) { if (resp.ok && resp.data) updateRender(resp.data); })
            .catch(function() {});

        pollingTimer = setTimeout(poll, 1000);
    }
    poll();
}

function stopPolling() {
    pollingActive = false;
    if (pollingTimer) { clearTimeout(pollingTimer); pollingTimer = null; }
}

(function init() {
    connectWebSocket();

    // HTTP polling fallback (starts immediately, pauses when WS connected)
    startPolling();

    // Log polling (separate, runs always for incremental tail)
    setInterval(fetchLogsIncremental, 2000);

    // Fetch system info via HTTP on load (one-time)
    fetch('/api/system')
        .then(function(r) { return r.json(); })
        .then(function(resp) {
            if (resp.ok && resp.data) {
                updateSystem(resp.data);
            }
        })
        .catch(function() {});

    // Handle window resize for canvas redraws
    var resizeTimeout;
    window.addEventListener('resize', function() {
        clearTimeout(resizeTimeout);
        resizeTimeout = setTimeout(function() {}, 250);
    });
})();
</script>

</body>
</html>]]
end

return M
