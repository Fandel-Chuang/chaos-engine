--[[
    heartbeat.lua — Heartbeat Detection & Timeout Handling

    Manages bidirectional heartbeat monitoring:
    1. Game → DBProxy heartbeat (sync port 9001)
       - Expects heartbeat frames (frame_seq=0xFFFF) every 500ms
       - Declares Game A dead after 3s without heartbeat
       - On timeout: triggers state_mirror.archive()

    2. DBProxy primary ↔ backup heartbeat (peer port)
       - Sends periodic heartbeat pings to peer DBProxy
       - Monitors peer liveness
       - On peer timeout: primary may take over backup duties

    Design:
    - Each connection has a HeartbeatTracker recording last_seen time
    - A periodic coroutine checks all trackers and fires timeout callbacks
]]

local config   = require("dbproxy.config")
local protocol = require("dbproxy.protocol")
local mirror   = require("dbproxy.state_mirror")

local M = {}

-- ============================================================
-- Heartbeat Tracker
-- ============================================================

--- Create a new heartbeat tracker for a connection.
--- @param conn_id     string  Unique connection identifier
--- @param label       string  Human-readable label (e.g., "Game A", "peer")
--- @param on_timeout  function  Callback when timeout fires
--- @return table  Tracker object
local function new_tracker(conn_id, label, on_timeout)
    return {
        conn_id    = conn_id,
        label      = label,
        last_seen  = os.time(),
        alive      = true,
        timeout_count = 0,
        on_timeout = on_timeout,
    }
end

--- Update tracker: record that we received a heartbeat.
local function tracker_ping(tracker)
    tracker.last_seen = os.time()
    if not tracker.alive then
        tracker.alive = true
        M._log("info", string.format("[%s] %s is back alive", tracker.conn_id, tracker.label))
    end
end

--- Check if tracker has timed out.
local function tracker_is_timeout(tracker)
    local elapsed = os.time() - tracker.last_seen
    return elapsed > config.heartbeat_timeout
end

-- ============================================================
-- Internal State
-- ============================================================

-- Trackers for Game connections (sync port)
local game_trackers = {}    -- conn_id → tracker

-- Tracker for peer DBProxy
local peer_tracker = nil

-- Running flag
local running = false

-- Timeout callbacks registry
local timeout_callbacks = {}

-- ============================================================
-- Game Heartbeat
-- ============================================================

--- Register a Game connection for heartbeat monitoring.
--- @param conn_id  string  Connection identifier
--- @param label    string  Label (e.g., "Game A")
function M.register_game(conn_id, label)
    local tracker = new_tracker(conn_id, label, function()
        M._log("warn", string.format("[%s] %s heartbeat timeout! Archiving state mirror...", conn_id, label))
        -- Trigger state mirror archive on Game A timeout
        local archive_path, err = mirror.archive()
        if archive_path then
            M._log("info", string.format("[%s] State mirror archived to: %s", conn_id, archive_path))
        else
            M._log("error", string.format("[%s] Failed to archive state mirror: %s", conn_id, tostring(err)))
        end
    end)
    game_trackers[conn_id] = tracker
    M._log("info", string.format("[%s] Registered %s for heartbeat monitoring", conn_id, label))
    return tracker
end

--- Record a heartbeat received from a Game connection.
--- @param conn_id  string  Connection identifier
function M.game_heartbeat(conn_id)
    local tracker = game_trackers[conn_id]
    if tracker then
        tracker_ping(tracker)
    end
end

--- Unregister a Game connection (on disconnect).
--- @param conn_id  string  Connection identifier
function M.unregister_game(conn_id)
    game_trackers[conn_id] = nil
    M._log("info", string.format("[%s] Unregistered from heartbeat monitoring", conn_id))
end

--- Check if a specific Game connection is alive.
--- @param conn_id  string  Connection identifier
--- @return boolean
function M.is_game_alive(conn_id)
    local tracker = game_trackers[conn_id]
    if not tracker then return false end
    return not tracker_is_timeout(tracker)
end

-- ============================================================
-- Peer DBProxy Heartbeat
-- ============================================================

--- Initialize peer heartbeat monitoring.
--- Called for both primary and backup roles.
function M.init_peer()
    local label = (config.role == "primary") and "backup" or "primary"
    peer_tracker = new_tracker("peer", label, function()
        M._log("warn", string.format("[peer] %s DBProxy heartbeat timeout!", label))
        -- In production: trigger failover logic
        -- For now: log and continue
    end)
    M._log("info", string.format("[peer] Monitoring %s DBProxy at %s:%d",
        label, config.peer_host, config.peer_port))
end

--- Record a heartbeat received from peer DBProxy.
function M.peer_heartbeat()
    if peer_tracker then
        tracker_ping(peer_tracker)
    end
end

--- Check if peer DBProxy is alive.
--- @return boolean
function M.is_peer_alive()
    if not peer_tracker then return false end
    return not tracker_is_timeout(peer_tracker)
end

--- Get peer tracker (for sending heartbeats).
--- @return table|nil
function M.get_peer_tracker()
    return peer_tracker
end

-- ============================================================
-- Heartbeat Check Loop
-- ============================================================

--- Start the heartbeat checker coroutine.
--- Runs periodically, checking all trackers for timeouts.
function M.start()
    if running then return end
    running = true

    M._log("info", string.format(
        "Heartbeat checker started (interval=%.1fs, timeout=%.1fs)",
        config.heartbeat_interval, config.heartbeat_timeout
    ))
end

--- Stop the heartbeat checker.
function M.stop()
    running = false
    M._log("info", "Heartbeat checker stopped")
end

--- Run one tick of the heartbeat checker.
--- Should be called from the main event loop at heartbeat_interval.
--- @return table  List of timeout events that occurred
function M.tick()
    if not running then return {} end

    local now = os.time()
    local events = {}

    -- Check Game trackers
    for conn_id, tracker in pairs(game_trackers) do
        if tracker.alive and tracker_is_timeout(tracker) then
            tracker.alive = false
            tracker.timeout_count = tracker.timeout_count + 1
            M._log("warn", string.format(
                "[%s] %s heartbeat timeout #%d (last seen: %ds ago)",
                conn_id, tracker.label, tracker.timeout_count,
                now - tracker.last_seen
            ))
            events[#events + 1] = {
                type    = "game_timeout",
                conn_id = conn_id,
                label   = tracker.label,
                count   = tracker.timeout_count,
            }
            -- Fire timeout callback
            if tracker.on_timeout then
                tracker.on_timeout()
            end
        end
    end

    -- Check peer tracker
    if peer_tracker and peer_tracker.alive and tracker_is_timeout(peer_tracker) then
        peer_tracker.alive = false
        peer_tracker.timeout_count = peer_tracker.timeout_count + 1
        M._log("warn", string.format(
            "[peer] %s heartbeat timeout #%d (last seen: %ds ago)",
            peer_tracker.label, peer_tracker.timeout_count,
            now - peer_tracker.last_seen
        ))
        events[#events + 1] = {
            type  = "peer_timeout",
            label = peer_tracker.label,
            count = peer_tracker.timeout_count,
        }
        if peer_tracker.on_timeout then
            peer_tracker.on_timeout()
        end
    end

    return events
end

-- ============================================================
-- Heartbeat Frame Helpers
-- ============================================================

--- Build a heartbeat frame to send to a Game connection.
--- @return string  Binary heartbeat frame
function M.make_game_heartbeat_frame()
    return protocol.make_heartbeat_frame(protocol.now_us())
end

--- Build a heartbeat message to send to peer DBProxy.
--- @return string  Binary heartbeat message
function M.make_peer_heartbeat_message()
    return protocol.make_heartbeat_message()
end

-- ============================================================
-- Stats
-- ============================================================

--- Get heartbeat monitoring statistics.
--- @return table
function M.stats()
    local game_status = {}
    for conn_id, tracker in pairs(game_trackers) do
        game_status[conn_id] = {
            label      = tracker.label,
            alive      = tracker.alive,
            last_seen  = tracker.last_seen,
            timeouts   = tracker.timeout_count,
        }
    end

    local peer_status = nil
    if peer_tracker then
        peer_status = {
            label      = peer_tracker.label,
            alive      = peer_tracker.alive,
            last_seen  = peer_tracker.last_seen,
            timeouts   = peer_tracker.timeout_count,
        }
    end

    return {
        running      = running,
        interval     = config.heartbeat_interval,
        timeout      = config.heartbeat_timeout,
        games        = game_status,
        peer         = peer_status,
    }
end

-- ============================================================
-- Internal: Logging
-- ============================================================

function M._log(level, msg)
    local timestamp = os.date("%Y-%m-%d %H:%M:%S")
    local prefix = string.format("[%s] [HEARTBEAT] [%s]", timestamp, level:upper())
    io.stderr:write(string.format("%s %s\n", prefix, msg))
    io.stderr:flush()
end

return M
