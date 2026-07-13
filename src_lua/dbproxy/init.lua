--[[
    init.lua — Lua DBProxy Entry Point

    Standalone Lua process that provides:
    - Port 9001: State sync server (receives Game → DBProxy sync frames)
    - Port 9003: Database proxy server (receives DB operation requests)

    Architecture:
    - Uses LuaSocket for TCP networking
    - Coroutine-based concurrency: each connection runs in its own coroutine
    - Non-blocking I/O with select()-based event loop
    - Heartbeat monitoring via periodic timer coroutine

    Usage:
        lua init.lua [--role primary|backup] [--peer-host HOST] [--peer-port PORT]
]]

-- ============================================================
-- Bootstrap: Setup package path
-- ============================================================

-- Add parent directory to package.path so 'require("dbproxy.xxx")' works
local script_dir = debug.getinfo(1, "S").source:match("@(.*/)") or "."
-- Remove trailing slash
script_dir = script_dir:gsub("/$", "")
local parent_dir = script_dir:gsub("/[^/]+$", "")
package.path = parent_dir .. "/?.lua;" .. parent_dir .. "/?/init.lua;" .. package.path

-- ============================================================
-- Imports
-- ============================================================

local socket   = require("socket")
local config   = require("dbproxy.config")
local protocol = require("dbproxy.protocol")
local mirror   = require("dbproxy.state_mirror")
local handler  = require("dbproxy.handler")
local heartbeat = require("dbproxy.heartbeat")

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
        if arg == "--role" and args[i + 1] then
            config.role = args[i + 1]
            i = i + 1
        elseif arg == "--peer-host" and args[i + 1] then
            config.peer_host = args[i + 1]
            i = i + 1
        elseif arg == "--peer-port" and args[i + 1] then
            config.peer_port = tonumber(args[i + 1]) or config.peer_port
            i = i + 1
        elseif arg == "--mongo-uri" and args[i + 1] then
            config.mongo_uri = args[i + 1]
            i = i + 1
        elseif arg == "--mongo-db" and args[i + 1] then
            config.mongo_db = args[i + 1]
            i = i + 1
        elseif arg == "--archive-dir" and args[i + 1] then
            config.archive_dir = args[i + 1]
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
  --role ROLE           Set DBProxy role: "primary" or "backup" (default: primary)
  --peer-host HOST      Peer DBProxy host (default: 127.0.0.1)
  --peer-port PORT      Peer DBProxy port (default: 9101)
  --mongo-uri URI       MongoDB connection URI
  --mongo-db DB         MongoDB database name
  --archive-dir DIR     State mirror archive directory
  --help, -h            Show this help

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

local coroutines = {}       -- List of active coroutines
local sockets_to_watch = {} -- socket → coroutine mapping

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
    -- We use a simple approach: yield and check time in the main loop
    -- Store deadline in a registry
    local timer_id = tostring(co):match("0x(%x+)") or tostring(co)
    _timers = _timers or {}
    _timers[timer_id] = { co = co, deadline = deadline }
    coroutine.yield()
end

-- ============================================================
-- TCP Server
-- ============================================================

--- Create a TCP server listening on the given port.
--- @param port       number  Port to listen on
--- @param label      string  Label for logging
--- @param on_accept  function(client_sock, client_addr)  Connection handler
--- @return table|nil  Server socket, or nil on failure
local function create_server(port, label, on_accept)
    local server, err = socket.tcp()
    if not server then
        log("error", label, "Failed to create socket: " .. tostring(err))
        return nil
    end

    server:setoption("reuseaddr", true)

    local ok, bind_err = server:bind("0.0.0.0", port)
    if not ok then
        log("error", label, string.format("Failed to bind port %d: %s", port, tostring(bind_err)))
        server:close()
        return nil
    end

    local ok2, listen_err = server:listen(config.max_sync_connections)
    if not ok2 then
        log("error", label, string.format("Failed to listen on port %d: %s", port, tostring(listen_err)))
        server:close()
        return nil
    end

    server:settimeout(0)  -- Non-blocking

    log("info", label, string.format("Listening on 0.0.0.0:%d", port))

    -- Spawn accept coroutine
    spawn(function()
        while true do
            local client, err = server:accept()
            if client then
                local addr, port = client:getpeername()
                local client_id = string.format("%s:%d", addr, port)
                log("info", label, string.format("Accepted connection from %s", client_id))
                client:settimeout(0)  -- Non-blocking
                spawn(function()
                    on_accept(client, client_id)
                end, label .. "_" .. client_id)
            elseif err == "timeout" then
                -- No pending connections; yield and retry
                wait_readable(server)
            elseif err ~= "timeout" then
                log("error", label, "Accept error: " .. tostring(err))
                wait_readable(server)
            end
        end
    end, label .. "_acceptor")

    return server
end

-- ============================================================
-- Sync Connection Handler (Port 9001)
-- ============================================================

--- Handle a Game sync connection.
--- Receives binary frames, applies to state mirror, handles heartbeats.
local function handle_sync_connection(client, client_id)
    local recv_buf = ""
    local label = "sync_" .. client_id

    -- Register for heartbeat monitoring
    heartbeat.register_game(client_id, "Game-" .. client_id)

    log("info", "SYNC", string.format("[%s] Connection established", client_id))

    while true do
        -- Try to receive data
        local data, err, partial = client:receive("*a")
        if data then
            recv_buf = recv_buf .. data
        elseif partial and #partial > 0 then
            recv_buf = recv_buf .. partial
        elseif err == "timeout" or err == "wantread" then
            -- No data available; process what we have, then yield
        elseif err == "closed" then
            log("info", "SYNC", string.format("[%s] Connection closed", client_id))
            break
        else
            log("error", "SYNC", string.format("[%s] Receive error: %s", client_id, tostring(err)))
            break
        end

        -- Process complete frames from buffer
        while #recv_buf >= 4 do
            local frame_len = protocol.unpack_u32(recv_buf, 1)
            if frame_len > config.max_frame_size then
                log("error", "SYNC", string.format("[%s] Frame too large: %d bytes", client_id, frame_len))
                recv_buf = ""
                break
            end
            if #recv_buf < frame_len then
                break  -- Incomplete frame, wait for more data
            end

            -- Extract and unpack frame
            local frame_data = recv_buf:sub(1, frame_len)
            recv_buf = recv_buf:sub(frame_len + 1)

            local frame, unpack_err = protocol.unpack_frame(frame_data)
            if not frame then
                log("error", "SYNC", string.format("[%s] Frame unpack error: %s", client_id, tostring(unpack_err)))
            else
                -- Check for heartbeat
                if protocol.is_heartbeat(frame.frame_seq) then
                    heartbeat.game_heartbeat(client_id)
                    log("trace", "SYNC", string.format("[%s] Heartbeat received", client_id))
                    -- Send heartbeat response
                    local resp = protocol.make_heartbeat_frame(protocol.now_us())
                    client:send(resp)
                else
                    -- Apply frame to state mirror
                    local updated = mirror.apply_frame(frame)
                    log("debug", "SYNC", string.format(
                        "[%s] Frame seq=%d, entities=%d, updated=%d",
                        client_id, frame.frame_seq, #(frame.entities or {}), updated
                    ))
                end
            end
        end

        -- Yield to let other coroutines run
        wait_readable(client)
    end

    -- Cleanup
    heartbeat.unregister_game(client_id)
    client:close()
    log("info", "SYNC", string.format("[%s] Connection closed", client_id))
end

-- ============================================================
-- DB Connection Handler (Port 9003)
-- ============================================================

--- Handle a DB proxy connection.
--- Receives DB operation messages, dispatches to handler, sends response.
local function handle_db_connection(client, client_id)
    local recv_buf = ""
    local label = "db_" .. client_id

    log("info", "DB", string.format("[%s] Connection established", client_id))

    while true do
        -- Try to receive data
        local data, err, partial = client:receive("*a")
        if data then
            recv_buf = recv_buf .. data
        elseif partial and #partial > 0 then
            recv_buf = recv_buf .. partial
        elseif err == "timeout" or err == "wantread" then
            -- No data; process buffer then yield
        elseif err == "closed" then
            log("info", "DB", string.format("[%s] Connection closed", client_id))
            break
        else
            log("error", "DB", string.format("[%s] Receive error: %s", client_id, tostring(err)))
            break
        end

        -- Process complete messages from buffer
        while #recv_buf >= 5 do
            local msg_len = protocol.unpack_u32(recv_buf, 1)
            if msg_len > config.max_frame_size then
                log("error", "DB", string.format("[%s] Message too large: %d bytes", client_id, msg_len))
                recv_buf = ""
                break
            end
            if #recv_buf < msg_len then
                break  -- Incomplete message
            end

            -- Extract and unpack message
            local msg_data = recv_buf:sub(1, msg_len)
            recv_buf = recv_buf:sub(msg_len + 1)

            local msg, unpack_err = protocol.unpack_message(msg_data)
            if not msg then
                log("error", "DB", string.format("[%s] Message unpack error: %s", client_id, tostring(unpack_err)))
                -- Send error response
                local err_resp = protocol.pack_response(protocol.RESP_ERROR, "malformed message")
                client:send(err_resp)
            else
                log("debug", "DB", string.format("[%s] Message type=0x%02X", client_id, msg.msg_type))

                -- Dispatch to handler
                local response = handler.dispatch(msg)

                -- Send response
                if response then
                    local sent, send_err = client:send(response)
                    if not sent then
                        log("error", "DB", string.format("[%s] Send error: %s", client_id, tostring(send_err)))
                        break
                    end
                end
            end
        end

        -- Yield
        wait_readable(client)
    end

    client:close()
    log("info", "DB", string.format("[%s] Connection closed", client_id))
end

-- ============================================================
-- Peer Heartbeat Connection (Primary ↔ Backup)
-- ============================================================

--- Connect to peer DBProxy and maintain heartbeat.
local function peer_heartbeat_loop()
    heartbeat.init_peer()

    while true do
        -- Try to connect/send heartbeat to peer
        local peer, err = socket.tcp()
        if peer then
            peer:settimeout(1)  -- 1 second connect timeout
            local ok, conn_err = peer:connect(config.peer_host, config.peer_port)
            if ok then
                -- Send heartbeat message
                local hb_msg = heartbeat.make_peer_heartbeat_message()
                peer:send(hb_msg)

                -- Try to receive response
                peer:settimeout(0.5)
                local resp, recv_err = peer:receive(1024)
                if resp then
                    heartbeat.peer_heartbeat()
                    log("trace", "PEER", "Peer heartbeat OK")
                end
                peer:close()
            else
                log("debug", "PEER", string.format("Cannot reach peer %s:%d: %s",
                    config.peer_host, config.peer_port, tostring(conn_err)))
            end
        end

        -- Wait for next heartbeat interval
        sleep(config.heartbeat_interval)
    end
end

-- ============================================================
-- Heartbeat Checker Coroutine
-- ============================================================

--- Periodic coroutine that checks all heartbeat trackers for timeouts.
local function heartbeat_checker_loop()
    heartbeat.start()

    while true do
        local events = heartbeat.tick()
        for _, ev in ipairs(events) do
            if ev.type == "game_timeout" then
                log("warn", "HEARTBEAT", string.format(
                    "Game timeout: %s (%s), count=%d",
                    ev.conn_id, ev.label, ev.count
                ))
            elseif ev.type == "peer_timeout" then
                log("warn", "HEARTBEAT", string.format(
                    "Peer timeout: %s, count=%d",
                    ev.label, ev.count
                ))
            end
        end

        sleep(config.heartbeat_interval)
    end
end

-- ============================================================
-- Stats Reporter Coroutine
-- ============================================================

-- Helper: count table entries (定义在使用前)
local function table_count(tbl)
    local n = 0
    for _ in pairs(tbl) do
        n = n + 1
    end
    return n
end

local function stats_reporter_loop()
    while true do
        sleep(10)  -- Every 10 seconds

        local mirror_stats = mirror.stats()
        local hb_stats = heartbeat.stats()

        log("info", "STATS", string.format(
            "Mirror: %d entities, %d frames | Heartbeat: %d games, peer=%s",
            mirror_stats.entity_count,
            mirror_stats.total_frames_received,
            hb_stats.games and table_count(hb_stats.games) or 0,
            hb_stats.peer and (hb_stats.peer.alive and "alive" or "dead") or "none"
        ))
    end
end

-- ============================================================
-- Main Event Loop
-- ============================================================

local function main_loop()
    log("info", "MAIN", "DBProxy event loop started")

    while true do
        -- Collect all watched sockets
        local sockets = {}
        for sock, _ in pairs(sockets_to_watch) do
            sockets[#sockets + 1] = sock
        end

        -- Check timers
        local now = socket.gettime()
        if _timers then
            for id, timer in pairs(_timers) do
                if now >= timer.deadline then
                    _timers[id] = nil
                    local co = timer.co
                    if coroutine.status(co) == "suspended" then
                        -- Resume will happen in the coroutine loop below
                        -- We just remove the timer; the coroutine will be picked up
                    end
                end
            end
        end

        -- If no sockets to watch and no timers, do a short sleep
        if #sockets == 0 then
            -- Check if any timer is ready
            local has_ready_timer = false
            if _timers then
                for _, timer in pairs(_timers) do
                    if now >= timer.deadline then
                        has_ready_timer = true
                        break
                    end
                end
            end
            if not has_ready_timer then
                socket.sleep(0.01)  -- 10ms idle sleep
            end
        else
            -- Use select to wait for readable sockets (with short timeout)
            local readable, _, err = socket.select(sockets, nil, 0.05)
            if readable then
                for _, sock in ipairs(readable) do
                    local co = sockets_to_watch[sock]
                    if co and coroutine.status(co) == "suspended" then
                        sockets_to_watch[sock] = nil
                        -- Resume will be handled below
                    end
                end
            end
        end

        -- Resume all ready coroutines
        -- (timers that expired, and sockets that became readable)
        local ready_cos = {}

        -- Collect coroutines whose timers expired
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
                -- Check if this coroutine is ready (timer expired or socket readable)
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
                    -- Resume the coroutine
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
    -- SIGINT / SIGTERM handler
    local function on_shutdown()
        log("info", "MAIN", "Shutting down...")
        heartbeat.stop()

        -- Archive state mirror before exit
        local archive_path, err = mirror.archive()
        if archive_path then
            log("info", "MAIN", "Final state archive: " .. archive_path)
        end

        os.exit(0)
    end

    -- Lua 5.4 doesn't have built-in signal handling; we rely on the shell script
    -- to send SIGTERM and the process will exit cleanly.
    -- For graceful shutdown, we check a flag file.
    spawn(function()
        while true do
            local f = io.open("/tmp/chaos_dbproxy_stop", "r")
            if f then
                f:close()
                os.remove("/tmp/chaos_dbproxy_stop")
                on_shutdown()
            end
            sleep(1)
        end
    end, "shutdown_watcher")
end

-- ============================================================
-- Entry Point
-- ============================================================

-- Parse command-line arguments
parse_args()

-- Print banner
io.stderr:write(string.format([[
╔══════════════════════════════════════════════════════╗
║         ChaosEngine DBProxy v%s                     ║
║         Role: %-36s ║
║         Sync Port: %-5d    DB Port: %-5d          ║
║         MongoDB: %-34s ║
╚══════════════════════════════════════════════════════╝
]], config.version, config.role:upper(), config.sync_port, config.db_port, config.mongo_uri))

-- Initialize state mirror
mirror.init(config)

-- Setup signal handling
setup_signal_handlers()

-- Start heartbeat checker coroutine
spawn(heartbeat_checker_loop, "heartbeat_checker")

-- Start stats reporter
spawn(stats_reporter_loop, "stats_reporter")

-- Start peer heartbeat (if peer is configured)
if config.peer_host and config.peer_host ~= "" then
    spawn(peer_heartbeat_loop, "peer_heartbeat")
end

-- Create sync server (port 9001)
local sync_server = create_server(config.sync_port, "SYNC", handle_sync_connection)
if not sync_server then
    log("error", "MAIN", "Failed to start sync server, exiting")
    os.exit(1)
end

-- Create DB server (port 9003)
local db_server = create_server(config.db_port, "DB", handle_db_connection)
if not db_server then
    log("error", "MAIN", "Failed to start DB server, exiting")
    os.exit(1)
end

log("info", "MAIN", "DBProxy initialization complete")

-- Run main event loop (blocks forever)
main_loop()
