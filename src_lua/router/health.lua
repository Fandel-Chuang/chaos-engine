--[[
    health.lua — Router Health Check (1s PING/PONG Heartbeat)

    Manages bidirectional heartbeat monitoring for all Router connections:
    - Game connections (port 9100): 1-second PING/PONG
    - Cluster peer connections (port 9101): 1-second PING/PONG

    Features:
    - Per-connection heartbeat tracking
    - 1-second interval PING sending
    - Timeout detection (configurable threshold, default 3s)
    - Automatic marking of dead connections
    - Callback-based timeout notification
]]

local config   = require("router.config")
local protocol = require("router.protocol")

local M = {}

-- ============================================================
-- Constants
-- ============================================================

M.DEFAULT_INTERVAL = 1.0   -- 1 second between pings
M.DEFAULT_TIMEOUT  = 3.0   -- 3 seconds before declaring dead

-- ============================================================
-- Heartbeat Tracker
-- ============================================================

--- Create a new heartbeat tracker.
--- @param conn_id   string  Connection identifier
--- @param label     string  Human-readable label
--- @param on_timeout function(conn_id)  Callback on timeout
--- @return table  Tracker object
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

--- Mark a tracker as having received a heartbeat response.
--- @param tracker table
local function tracker_pong(tracker)
    tracker.last_seen = os.time()
    tracker.missed_count = 0
    if not tracker.alive then
        tracker.alive = true
        M._log("info", string.format("[%s] %s is back alive", tracker.conn_id, tracker.label))
    end
end

--- Check if a tracker has timed out.
--- @param tracker table
--- @param timeout number  Timeout in seconds
--- @return boolean
local function tracker_is_timeout(tracker, timeout)
    return (os.time() - tracker.last_seen) > timeout
end

-- ============================================================
-- Internal State
-- ============================================================

-- All tracked connections: conn_id → tracker
local trackers = {}

-- Heartbeat configuration
local hb_config = {
    interval = M.DEFAULT_INTERVAL,
    timeout  = M.DEFAULT_TIMEOUT,
}

-- Whether the heartbeat checker is running
local running = false

-- ============================================================
-- Configuration
-- ============================================================

--- Configure heartbeat parameters.
--- @param opts table  { interval, timeout }
function M.configure(opts)
    if opts.interval then hb_config.interval = opts.interval end
    if opts.timeout  then hb_config.timeout  = opts.timeout end
end

--- Get current heartbeat configuration.
--- @return table  { interval, timeout }
function M.get_config()
    return { interval = hb_config.interval, timeout = hb_config.timeout }
end

-- ============================================================
-- Connection Registration
-- ============================================================

--- Register a connection for heartbeat monitoring.
--- @param conn_id    string  Unique connection identifier
--- @param label      string  Human-readable label
--- @param on_timeout function(conn_id)  Called when heartbeat times out
--- @return table  Tracker object
function M.register(conn_id, label, on_timeout)
    local tracker = new_tracker(conn_id, label, on_timeout)
    trackers[conn_id] = tracker
    M._log("debug", string.format("[%s] Registered %s for heartbeat", conn_id, label))
    return tracker
end

--- Record a heartbeat received from a connection.
--- @param conn_id string  Connection identifier
--- @return boolean  true if connection was found
function M.heartbeat_received(conn_id)
    local tracker = trackers[conn_id]
    if not tracker then
        return false
    end
    tracker_pong(tracker)
    return true
end

--- Unregister a connection from heartbeat monitoring.
--- @param conn_id string  Connection identifier
function M.unregister(conn_id)
    trackers[conn_id] = nil
    M._log("debug", string.format("[%s] Unregistered from heartbeat", conn_id))
end

--- Check if a connection is alive.
--- @param conn_id string
--- @return boolean
function M.is_alive(conn_id)
    local tracker = trackers[conn_id]
    if not tracker then return false end
    return not tracker_is_timeout(tracker, hb_config.timeout)
end

-- ============================================================
-- Heartbeat Sending
-- ============================================================

--- Send a PING to a connection.
--- The caller must provide the actual send function.
--- @param conn_id string  Connection identifier
--- @param send_fn function(conn_id, data)  Function to send raw data
--- @return boolean  true if ping was sent
function M.send_ping(conn_id, send_fn)
    local tracker = trackers[conn_id]
    if not tracker then return false end

    tracker.last_ping = os.time()

    if send_fn then
        local ping_data = protocol.make_ping()
        local ok, err = send_fn(conn_id, ping_data)
        if not ok then
            M._log("warn", string.format("[%s] Failed to send PING: %s",
                conn_id, tostring(err or "unknown error")))
            return false
        end
    end

    return true
end

--- Send a PONG response to a connection.
--- @param conn_id string  Connection identifier
--- @param send_fn function(conn_id, data)  Function to send raw data
--- @return boolean  true if pong was sent
function M.send_pong(conn_id, send_fn)
    if send_fn then
        local pong_data = protocol.make_pong()
        local ok, err = send_fn(conn_id, pong_data)
        if not ok then
            M._log("warn", string.format("[%s] Failed to send PONG: %s",
                conn_id, tostring(err or "unknown error")))
            return false
        end
    end
    return true
end

-- ============================================================
-- Heartbeat Check Loop
-- ============================================================

--- Start the heartbeat checker.
function M.start()
    if running then return end
    running = true
    M._log("info", string.format(
        "Heartbeat checker started (interval=%.1fs, timeout=%.1fs)",
        hb_config.interval, hb_config.timeout
    ))
end

--- Stop the heartbeat checker.
function M.stop()
    running = false
    M._log("info", "Heartbeat checker stopped")
end

--- Check if the heartbeat checker is running.
--- @return boolean
function M.is_running()
    return running
end

--- Perform one heartbeat tick.
--- Checks all tracked connections for timeouts and sends PINGs.
--- @param send_fn function(conn_id, data)  Function to send raw data
--- @return table  Array of timeout events { conn_id, label, count }
function M.tick(send_fn)
    if not running then return {} end

    local now = os.time()
    local events = {}

    for conn_id, tracker in pairs(trackers) do
        -- Check for timeout
        if tracker.alive and tracker_is_timeout(tracker, hb_config.timeout) then
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

        -- Send PING if interval has elapsed
        if (now - tracker.last_ping) >= hb_config.interval then
            M.send_ping(conn_id, send_fn)
        end
    end

    return events
end

-- ============================================================
-- Stats
-- ============================================================

--- Get heartbeat statistics.
--- @return table
function M.stats()
    local alive_count = 0
    local dead_count = 0
    local details = {}

    for conn_id, tracker in pairs(trackers) do
        if tracker.alive then
            alive_count = alive_count + 1
        else
            dead_count = dead_count + 1
        end
        details[conn_id] = {
            label       = tracker.label,
            alive       = tracker.alive,
            last_seen   = tracker.last_seen,
            last_ping   = tracker.last_ping,
            missed      = tracker.missed_count,
        }
    end

    return {
        running      = running,
        interval     = hb_config.interval,
        timeout      = hb_config.timeout,
        total        = alive_count + dead_count,
        alive        = alive_count,
        dead         = dead_count,
        connections  = details,
    }
end

-- ============================================================
-- Internal: Logging
-- ============================================================

--- Internal logging function.
--- @param level string  Log level
--- @param msg   string  Log message
function M._log(level, msg)
    local timestamp = os.date("%Y-%m-%d %H:%M:%S")
    local prefix = string.format("[%s] [ROUTER-HEALTH] [%s]", timestamp, level:upper())
    io.stderr:write(string.format("%s %s\n", prefix, msg))
    io.stderr:flush()
end

return M
