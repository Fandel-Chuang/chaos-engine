--[[
    init.lua — Lua Gateway Entry Point

    Standalone Lua process that provides:
    - Port 9000: TCP client connection gateway
    - Port 9001: KCP client connection gateway (reserved)
    - Port 9002: WebSocket client connection gateway (reserved)

    Architecture:
    - Uses LuaSocket for TCP networking
    - Coroutine-based concurrency: each client connection runs in its own coroutine
    - Non-blocking I/O with select()-based event loop
    - Client heartbeat monitoring via periodic timer coroutine
    - Backend health checks
    - Message routing to backend Game services

    Usage:
        lua init.lua [--port PORT] [--backend HOST:PORT]
]]

-- ============================================================
-- Bootstrap: Setup package path
-- ============================================================

-- Add parent directory to package.path so 'require("gateway.xxx")' works
local script_dir = debug.getinfo(1, "S").source:match("@(.*/)") or "."
-- Remove trailing slash
script_dir = script_dir:gsub("/$", "")
local parent_dir = script_dir:gsub("/[^/]+$", "")
package.path = parent_dir .. "/?.lua;" .. parent_dir .. "/?/init.lua;" .. package.path

-- ============================================================
-- Imports
-- ============================================================

local socket    = require("socket")
local config    = require("gateway.config")
local protocol  = require("gateway.protocol")
local conn_mgr  = require("gateway.connection")
local router    = require("gateway.router")
local heartbeat = require("gateway.heartbeat")
local server    = require("gateway.server")
local game_conn = require("gateway.game_connector")

-- ============================================================
-- Argument Parsing
-- ============================================================

-- Capture top-level varargs before defining functions
local cli_args = {...}

local function parse_args()
    local args = cli_args
    local i = 1
    while i <= #args do
        local arg = args[i]
        if arg == "--port" and args[i + 1] then
            config.tcp_port = tonumber(args[i + 1]) or config.tcp_port
            i = i + 1
        elseif arg == "--backend" and args[i + 1] then
            local host, port = args[i + 1]:match("^([^:]+):(%d+)$")
            if host and port then
                config.backends = { { host = host, port = tonumber(port) } }
            end
            i = i + 1
        elseif arg == "--game-server" and args[i + 1] then
            local host, port = args[i + 1]:match("^([^:]+):(%d+)$")
            if host and port then
                config.game_servers = { { host = host, port = tonumber(port) } }
            end
            i = i + 1
        elseif arg == "--max-connections" and args[i + 1] then
            config.max_connections = tonumber(args[i + 1]) or config.max_connections
            i = i + 1
        elseif arg == "--log-level" and args[i + 1] then
            config.log_level = args[i + 1]
            i = i + 1
        elseif arg == "--help" or arg == "-h" then
            print_usage()
            os.exit(0)
        end
        i = i + 1
    end
end

local function print_usage()
    io.stderr:write([[
Usage: lua init.lua [OPTIONS]

Options:
  --port PORT               TCP listen port (default: 9000)
  --backend HOST:PORT       Backend Game service address (default: 127.0.0.1:7777)
  --game-server HOST:PORT   Game server connection (default: 127.0.0.1:9100)
  --max-connections N       Max client connections (default: 10000)
  --log-level LEVEL         Log level: trace, debug, info, warn, error (default: info)
  --help, -h                Show this help

]])
end

-- ============================================================
-- Logging
-- ============================================================

local LOG_LEVELS = { trace = 0, debug = 1, info = 2, warn = 3, error = 4 }

local function log(level, mod, msg)
    local current = LOG_LEVELS[config.log_level] or 2
    local lvl = LOG_LEVELS[level] or 2
    if lvl < current then return end

    local ts = os.date("%Y-%m-%d %H:%M:%S")
    io.stderr:write(string.format("[%s] [%s] [%s] %s\n", ts, level:upper(), mod, msg))
    io.stderr:flush()
end

-- ============================================================
-- Coroutine Scheduler
-- ============================================================

local coroutines = {}        -- List of active coroutines
local sockets_to_watch = {}  -- socket → coroutine mapping

--- Spawn a new coroutine.
local function spawn(fn, name)
    local co = coroutine.create(function()
        local ok, err = pcall(fn)
        if not ok then
            log("error", "COROUTINE", string.format("[%s] crashed: %s", name or "anon", tostring(err)))
        end
    end)
    table.insert(coroutines, { co = co, name = name or "anon" })
    return co
end

--- Yield the current coroutine until a socket is readable.
local function wait_readable(sock)
    local co = coroutine.running()
    sockets_to_watch[sock] = co
    coroutine.yield()
    sockets_to_watch[sock] = nil
end

--- Yield the current coroutine for a specified duration (seconds).
local function sleep(seconds)
    local deadline = socket.gettime() + seconds
    local co = coroutine.running()
    local timer_id = tostring(co):match("0x(%x+)") or tostring(co)
    _timers = _timers or {}
    _timers[timer_id] = { co = co, deadline = deadline }
    coroutine.yield()
end

-- ============================================================
-- Connection Handler Spawner
-- ============================================================

--- Spawn handler coroutines for newly accepted connections.
local function spawn_pending_handlers()
    local handlers = server.drain_pending_handlers()
    local handler_fn = server.get_handler_fn()

    for _, conn in ipairs(handlers) do
        spawn(function()
            handler_fn(conn)
        end, "client_" .. conn.id)
    end
end

-- ============================================================
-- Heartbeat Checker Coroutine
-- ============================================================

--- Periodic coroutine that checks heartbeats and pings clients.
local function heartbeat_loop()
    heartbeat.start()

    while true do
        local events = heartbeat.tick()

        -- Log timeout events
        if events.client_timeouts then
            for _, conn_id in ipairs(events.client_timeouts) do
                log("warn", "HEARTBEAT", string.format("Client %d timed out", conn_id))
            end
        end

        if events.backend_checks then
            for _, r in ipairs(events.backend_checks) do
                if not r.alive then
                    log("warn", "HEARTBEAT", string.format("Backend %s:%d is down", r.host, r.port))
                end
            end
        end

        -- Send PING to all clients
        server.ping_all_clients()

        sleep(config.heartbeat_interval)
    end
end

-- ============================================================
-- Game Server Connections
-- ============================================================

-- Table of active GameConnector instances, indexed by server index.
local game_connections = {}

--- Connect to all configured game servers.
local function connect_to_game_servers()
    local servers = config.game_servers or {}
    if #servers == 0 then
        log("info", "GAME", "No game servers configured, skipping game connections")
        return
    end

    for i, server_cfg in ipairs(servers) do
        local host = server_cfg.host or "127.0.0.1"
        local port = server_cfg.port or 9100
        local name = string.format("game_%d", i)

        local connector = game_conn.GameConnector.new(name)
        local ok, err = connector:connect(host, port)

        if ok then
            game_connections[i] = connector
            log("info", "GAME", string.format(
                "Gateway connected to Game at %s:%d (index=%d)",
                host, port, i
            ))
            print(string.format(
                "Gateway connected to Game at %s:%d",
                host, port
            ))
        else
            log("warn", "GAME", string.format(
                "Failed to connect to Game at %s:%d: %s",
                host, port, tostring(err)
            ))
        end
    end

    log("info", "GAME", string.format(
        "Game connections: %d/%d established",
        table_count(game_connections), #servers
    ))
end

--- Send data to a specific game server by index.
--- @param server_index  number  1-based index into game_servers config
--- @param data          string  Data to send
--- @return boolean  true on success
--- @return string|nil  Error message on failure
_G.gateway = _G.gateway or {}
_G.gateway.send_to_game = function(server_index, data)
    local conn = game_connections[server_index]
    if not conn then
        return false, "no game connection at index " .. tostring(server_index)
    end
    return conn:send(data)
end

--- Broadcast data to all connected game servers.
--- @param data  string  Data to send
--- @return number  Count of successful sends
_G.gateway.broadcast_to_games = function(data)
    local count = 0
    for i, conn in pairs(game_connections) do
        local ok, err = conn:send(data)
        if ok then
            count = count + 1
        else
            log("warn", "GAME", string.format(
                "Broadcast to game_%d failed: %s", i, tostring(err)
            ))
        end
    end
    return count
end

--- Disconnect all game server connections.
local function disconnect_game_servers()
    for i, conn in pairs(game_connections) do
        conn:disconnect()
        log("info", "GAME", string.format("Disconnected from game_%d", i))
    end
    game_connections = {}
end

--- Helper: count entries in a table.
local function table_count(t)
    local n = 0
    for _ in pairs(t) do
        n = n + 1
    end
    return n
end

-- ============================================================
-- Stats Reporter Coroutine
-- ============================================================

local function stats_reporter_loop()
    while true do
        sleep(30)  -- Every 30 seconds

        local conn_stats = conn_mgr.stats()
        local hb_stats = heartbeat.stats()

        log("info", "STATS", string.format(
            "Connections: %d/%d active | Heartbeat: %d clients, %d timeouts",
            conn_stats.active,
            conn_stats.max,
            hb_stats.clients_total,
            hb_stats.clients_timeout
        ))
    end
end

-- ============================================================
-- Main Event Loop
-- ============================================================

local function main_loop()
    log("info", "MAIN", "Gateway event loop started")

    while true do
        -- Spawn handlers for newly accepted connections
        spawn_pending_handlers()

        -- Collect all watched sockets
        local sockets = {}
        for sock, _ in pairs(sockets_to_watch) do
            sockets[#sockets + 1] = sock
        end

        -- Always watch the server socket
        local srv_sock = server.get_socket()
        if srv_sock then
            sockets[#sockets + 1] = srv_sock
        end

        -- Check timers
        local now = socket.gettime()
        local has_ready_timer = false
        if _timers then
            for _, timer in pairs(_timers) do
                if now >= timer.deadline then
                    has_ready_timer = true
                    break
                end
            end
        end

        -- If no sockets and no timers, do a short sleep
        if #sockets == 0 and not has_ready_timer then
            socket.sleep(0.01)  -- 10ms idle sleep
        elseif #sockets > 0 then
            -- Use select to wait for readable sockets
            local readable, _, err = socket.select(sockets, nil, 0.05)
            if readable then
                for _, sock in ipairs(readable) do
                    -- Check if this is the server socket
                    if sock == srv_sock then
                        server.accept()
                    else
                        -- Mark the coroutine waiting on this socket as ready
                        local co = sockets_to_watch[sock]
                        if co and coroutine.status(co) == "suspended" then
                            -- Will be resumed below
                        end
                    end
                end
            end
        end

        -- Collect coroutines whose timers expired
        local ready_cos = {}
        if _timers then
            for id, timer in pairs(_timers) do
                if socket.gettime() >= timer.deadline then
                    _timers[id] = nil
                    ready_cos[timer.co] = true
                end
            end
        end

        -- Resume coroutines
        for i = #coroutines, 1, -1 do
            local entry = coroutines[i]
            local status = coroutine.status(entry.co)
            if status == "suspended" then
                -- Check if this coroutine is ready
                local is_ready = ready_cos[entry.co]
                if not is_ready then
                    -- Check if any of its watched sockets became readable
                    for sock, co in pairs(sockets_to_watch) do
                        if co == entry.co then
                            is_ready = true
                            break
                        end
                    end
                end

                if is_ready or #sockets == 0 then
                    local ok, err = coroutine.resume(entry.co)
                    if not ok then
                        log("error", "COROUTINE", string.format("[%s] Error: %s", entry.name, tostring(err)))
                    end
                    if coroutine.status(entry.co) == "dead" then
                        table.remove(coroutines, i)
                    end
                end
            elseif status == "dead" then
                table.remove(coroutines, i)
            end
        end
    end
end

-- ============================================================
-- Signal Handling
-- ============================================================

local function setup_signal_handlers()
    -- Lua 5.4 doesn't have built-in signal handling; we rely on the shell script
    -- to send SIGTERM and the process will exit cleanly.
    -- For graceful shutdown, we check a flag file.
    spawn(function()
        while true do
            local f = io.open("/tmp/chaos_gateway_stop", "r")
            if f then
                f:close()
                os.remove("/tmp/chaos_gateway_stop")
                graceful_shutdown()
            end
            sleep(1)
        end
    end, "shutdown_watcher")
end

local function graceful_shutdown()
    log("info", "MAIN", "Shutting down gracefully...")
    heartbeat.stop()
    server.stop()
    router.close_backend_pool()
    disconnect_game_servers()
    log("info", "MAIN", "Shutdown complete")
    os.exit(0)
end

-- ============================================================
-- Entry Point
-- ============================================================

-- Parse command-line arguments
parse_args()

-- Print banner
io.stderr:write(string.format([[
╔══════════════════════════════════════════════════════╗
║         ChaosEngine Gateway v%s                     ║
║         TCP Port: %-5d    KCP Port: %-5d          ║
║         WS Port:  %-5d                              ║
║         Backend:  %-34s ║
╚══════════════════════════════════════════════════════╝
]], config.version, config.tcp_port, config.kcp_port, config.ws_port,
   config.backends[1] and (config.backends[1].host .. ":" .. config.backends[1].port) or "none"))

-- Setup signal handling
setup_signal_handlers()

-- Start heartbeat checker coroutine
spawn(heartbeat_loop, "heartbeat")

-- Start stats reporter
spawn(stats_reporter_loop, "stats_reporter")

-- Start TCP server
local srv, err = server.start()
if not srv then
    log("error", "MAIN", "Failed to start TCP server: " .. tostring(err))
    os.exit(1)
end

log("info", "MAIN", "Gateway initialization complete")

-- Connect to game servers
connect_to_game_servers()

-- Run main event loop (blocks forever)
main_loop()
