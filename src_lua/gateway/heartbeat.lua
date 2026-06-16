--[[
    heartbeat.lua — Gateway Heartbeat Management

    Manages bidirectional heartbeat monitoring for Gateway connections.
    Supports both TCP (application-level heartbeat messages) and
    WebSocket (PING/PONG frames per RFC 6455).

    Features:
    - Per-connection heartbeat tracking
    - Periodic PING sending (WebSocket) or heartbeat message (TCP)
    - Timeout detection (configurable threshold)
    - Automatic cleanup of dead connections
    - WebSocket PING/PONG frame handling

    API used by server.lua:
        heartbeat.register(conn, on_timeout_callback)
        heartbeat.unregister(conn)
        heartbeat.heartbeat_received(conn)
        heartbeat.configure({interval, timeout})
        heartbeat.start()
        heartbeat.tick(connections) → events
        heartbeat.send_heartbeat(conn) → boolean
]]

local M = {}

-- ============================================================
-- Constants
-- ============================================================

M.DEFAULT_INTERVAL = 1.0
M.DEFAULT_TIMEOUT  = 3.0

-- ============================================================
-- Heartbeat Tracker
-- ============================================================

local function new_tracker(conn_id, label, on_timeout)
    return {
        conn_id       = conn_id,
        label         = label,
        last_seen     = os.time(),
        last_ping     = os.time(),
        alive         = true,
        missed_count  = 0,
        on_timeout    = on_timeout,
    }
end

local function tracker_ping(tracker)
    tracker.last_seen = os.time()
    tracker.missed_count = 0
    if not tracker.alive then
        tracker.alive = true
        M._log("info", string.format("[%s] %s is back alive", tracker.conn_id, tracker.label))
    end
end

local function tracker_is_timeout(tracker, timeout)
    return (os.time() - tracker.last_seen) > timeout
end

-- ============================================================
-- Internal State
-- ============================================================

local trackers = {}
local config = {
    interval   = M.DEFAULT_INTERVAL,
    timeout    = M.DEFAULT_TIMEOUT,
}
local running = false
local ws_module = nil

-- ============================================================
-- Configuration
-- ============================================================

function M.configure(opts)
    if opts.interval then config.interval = opts.interval end
    if opts.timeout then config.timeout = opts.timeout end
end

function M.get_config()
    return { interval = config.interval, timeout = config.timeout }
end

-- ============================================================
-- Connection Registration
-- ============================================================

function M.register(conn, on_timeout)
    local conn_id = conn.session_id or conn.client_addr or tostring(conn.id)
    local label = string.format("%s/%s", conn.proto or "tcp", conn.client_addr or "unknown")

    local tracker = new_tracker(conn_id, label, on_timeout)
    trackers[conn_id] = tracker

    M._log("info", string.format("[%s] Registered %s for heartbeat monitoring", conn_id, label))
    return tracker
end

function M.heartbeat_received(conn)
    local conn_id = conn.session_id or conn.client_addr or tostring(conn.id)
    local tracker = trackers[conn_id]
    if tracker then
        tracker_ping(tracker)
    end
end

function M.unregister(conn)
    local conn_id = conn.session_id or conn.client_addr or tostring(conn.id)
    trackers[conn_id] = nil
    M._log("info", string.format("[%s] Unregistered from heartbeat monitoring", conn_id))
end

function M.is_alive(conn)
    local conn_id = conn.session_id or conn.client_addr or tostring(conn.id)
    local tracker = trackers[conn_id]
    if not tracker then return false end
    return not tracker_is_timeout(tracker, config.timeout)
end

-- ============================================================
-- Heartbeat Sending
-- ============================================================

function M.send_heartbeat(conn)
    local conn_id = conn.session_id or conn.client_addr or tostring(conn.id)
    local tracker = trackers[conn_id]
    if not tracker then return false end

    tracker.last_ping = os.time()

    if conn.proto == "websocket" then
        if not ws_module then
            ws_module = require("gateway.websocket")
        end
        local ok, err = ws_module.ws_ping(conn, "ping")
        if not ok then
            M._log("warn", string.format("[%s] Failed to send WS ping: %s", conn_id, tostring(err)))
            return false
        end
        return true
    else
        local ok, err = conn.sock:send("HB\n")
        if not ok then
            M._log("warn", string.format("[%s] Failed to send TCP heartbeat: %s", conn_id, tostring(err)))
            return false
        end
        return true
    end
end

-- ============================================================
-- Heartbeat Check Loop
-- ============================================================

function M.start()
    if running then return end
    running = true
    M._log("info", string.format(
        "Heartbeat checker started (interval=%.1fs, timeout=%.1fs)",
        config.interval, config.timeout
    ))
end

function M.stop()
    running = false
    M._log("info", "Heartbeat checker stopped")
end

function M.is_running()
    return running
end

function M.tick(connections)
    if not running then return {} end

    local now = os.time()
    local events = {}

    for conn_id, tracker in pairs(trackers) do
        if tracker.alive and tracker_is_timeout(tracker, config.timeout) then
            tracker.alive = false
            tracker.missed_count = tracker.missed_count + 1
            M._log("warn", string.format(
                "[%s] %s heartbeat timeout #%d (last seen: %ds ago)",
                conn_id, tracker.label, tracker.missed_count,
                now - tracker.last_seen
            ))
            events[#events + 1] = {
                type    = "heartbeat_timeout",
                conn_id = conn_id,
                label   = tracker.label,
                count   = tracker.missed_count,
            }
            if tracker.on_timeout then
                tracker.on_timeout(conn_id)
            end
        end

        if connections and connections[conn_id] then
            if (now - tracker.last_ping) >= config.interval then
                M.send_heartbeat(connections[conn_id])
            end
        end
    end

    return events
end

-- ============================================================
-- Stats
-- ============================================================

function M.stats()
    local status = {}
    for conn_id, tracker in pairs(trackers) do
        status[conn_id] = {
            label       = tracker.label,
            alive       = tracker.alive,
            last_seen   = tracker.last_seen,
            last_ping   = tracker.last_ping,
            missed      = tracker.missed_count,
        }
    end

    local n = 0
    for _ in pairs(trackers) do n = n + 1 end

    return {
        running       = running,
        interval      = config.interval,
        timeout       = config.timeout,
        total_tracked = n,
        connections   = status,
    }
end

-- ============================================================
-- Internal: Logging
-- ============================================================

function M._log(level, msg)
    local timestamp = os.date("%Y-%m-%d %H:%M:%S")
    local prefix = string.format("[%s] [GATEWAY-HEARTBEAT] [%s]", timestamp, level:upper())
    io.stderr:write(string.format("%s %s\n", prefix, msg))
    io.stderr:flush()
end

return M
