--[[
    ws_handler.lua — Phase 6.1-6.3: Lapis WebSocket 路由 + 定时推送循环 + 日志实时推送

    WebSocket endpoint at /ws that pushes real-time updates for all subsystems.
    Uses Lapis's built-in WebSocket support via nginx's lua-resty-websocket.

    Protocol:
      Server → Client: JSON messages with { type, data, timestamp }
      Types: stats, aoi, cell, network, memory, cpu, render, log, system

    Push intervals (configurable via config.PUSH_INTERVALS):
      stats:   0.5s
      aoi:     0.5s
      cell:    1.0s
      network: 1.0s
      memory:  2.0s
      render:  0.5s
      log:     1.0s (incremental tail)
      system:  5.0s (infrequent)
]]

local json = require("cjson")
local ipc = require("admin.ipc_client")
local config = require("admin.config")

local M = {}

-- Last log timestamp for incremental log push
local last_log_timestamp_us = 0

--- Send a JSON message over WebSocket.
--- Returns true on success, false + error on failure.
local function ws_send(ws, msg)
    if not ws then return false, "ws is nil" end
    local ok, err = pcall(function()
        local data = json.encode(msg)
        ws:send_text(data)
    end)
    if not ok then
        return false, tostring(err)
    end
    return true
end

--- Make an IPC call and return the result data.
local function ipc_fetch(method, params)
    local sock, err = ipc.connect()
    if not sock then
        return nil, err
    end

    local result, call_err = ipc.call(sock, method, params)
    ipc.close(sock)

    if not result then
        return nil, call_err
    end

    return result
end

--- Push a single subsystem update over WebSocket.
local function push_subsystem(ws, subsystem_type, method, params)
    local data, err = ipc_fetch(method, params)
    if not data then
        -- Silently skip on error (connection may be down)
        return
    end

    local msg = {
        type = subsystem_type,
        timestamp = os.time(),
        data = data
    }
    ws_send(ws, msg)
end

--- Push log entries incrementally.
local function push_logs(ws)
    local params = {
        lines = config.LOG_MAX_LINES or 200,
        since_us = last_log_timestamp_us
    }

    local data, err = ipc_fetch("log", params)
    if not data then
        return
    end

    -- Update the last timestamp for next incremental fetch
    if data.max_timestamp_us and data.max_timestamp_us > 0 then
        last_log_timestamp_us = data.max_timestamp_us
    end

    -- Only push if there are new entries
    if data.entries and #data.entries > 0 then
        local msg = {
            type = "log",
            timestamp = os.time(),
            data = data.entries
        }
        ws_send(ws, msg)
    end
end

--- Main WebSocket handler loop.
--- This runs in a coroutine managed by Lapis/nginx.
--- @param ws table The WebSocket object from Lapis
function M.handle(ws)
    if not ws then
        return
    end

    -- Track last push times for each subsystem
    local last_push = {
        stats   = 0,
        aoi     = 0,
        cell    = 0,
        network = 0,
        memory  = 0,
        cpu     = 0,
        render  = 0,
        log     = 0,
        system  = 0,
    }

    -- Reset log timestamp on new connection
    last_log_timestamp_us = 0

    -- Get push intervals from config (with defaults)
    local intervals = {
        stats   = config.PUSH_INTERVALS and config.PUSH_INTERVALS.stats   or 0.5,
        aoi     = config.PUSH_INTERVALS and config.PUSH_INTERVALS.aoi     or 0.5,
        cell    = config.PUSH_INTERVALS and config.PUSH_INTERVALS.cell    or 1.0,
        network = config.PUSH_INTERVALS and config.PUSH_INTERVALS.network or 1.0,
        memory  = config.PUSH_INTERVALS and config.PUSH_INTERVALS.memory  or 2.0,
        render  = config.PUSH_INTERVALS and config.PUSH_INTERVALS.render  or 0.5,
        log     = 1.0,  -- Log push interval
        system  = 5.0,  -- System info push interval (infrequent)
    }

    -- Push system info immediately on connect
    push_subsystem(ws, "system", "system")
    last_push.system = os.time()

    -- Main push loop
    while true do
        -- Check if WebSocket is still alive
        -- In Lapis, we can check by trying to receive with a short timeout
        -- If the client disconnected, this will return nil

        -- Read any incoming messages (with 100ms timeout)
        -- This also serves as a keep-alive check
        local data, typ, err = ws:recv(0.1) -- 100ms timeout

        if typ == "close" or (not data and err == "closed") then
            -- Client disconnected
            break
        end

        -- Handle incoming messages (e.g., client requesting specific data)
        if data and typ == "text" then
            local ok, msg = pcall(json.decode, data)
            if ok and msg then
                -- Client can request specific subsystem data
                if msg.type == "subscribe" and msg.subsystems then
                    -- Could implement selective subscription
                    -- For now, we push everything
                elseif msg.type == "ping" then
                    ws_send(ws, { type = "pong", timestamp = os.time() })
                end
            end
        end

        local now = os.time()

        -- Push each subsystem on its interval
        -- Stats (0.5s)
        if now - last_push.stats >= intervals.stats then
            push_subsystem(ws, "stats", "stats")
            last_push.stats = now
        end

        -- AOI (0.5s)
        if now - last_push.aoi >= intervals.aoi then
            push_subsystem(ws, "aoi", "aoi")
            last_push.aoi = now
        end

        -- Cell (1.0s)
        if now - last_push.cell >= intervals.cell then
            push_subsystem(ws, "cell", "cell")
            last_push.cell = now
        end

        -- Network (1.0s)
        if now - last_push.network >= intervals.network then
            push_subsystem(ws, "network", "network")
            last_push.network = now
        end

        -- Memory (2.0s)
        if now - last_push.memory >= intervals.memory then
            push_subsystem(ws, "memory", "memory")
            last_push.memory = now
        end

        -- CPU (1.0s)
        if now - last_push.cpu >= 1.0 then
            push_subsystem(ws, "cpu", "cpu")
            last_push.cpu = now
        end

        -- Render (0.5s)
        if now - last_push.render >= intervals.render then
            push_subsystem(ws, "render", "render")
            last_push.render = now
        end

        -- Log (1.0s, incremental)
        if now - last_push.log >= intervals.log then
            push_logs(ws)
            last_push.log = now
        end

        -- System (5.0s)
        if now - last_push.system >= intervals.system then
            push_subsystem(ws, "system", "system")
            last_push.system = now
        end
    end
end

return M
