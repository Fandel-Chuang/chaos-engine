--[[
    server.lua — Gateway Server Entry Point

    ChaosEngine Gateway — network access layer that handles:
    - TCP client connections (port 9000)
    - WebSocket client connections (port 9002) with HTTP Upgrade handshake
    - KCP client connections (port 9001, placeholder)
    - Message routing to backend Game processes
    - Connection lifecycle management
    - Heartbeat monitoring

    Architecture:
    - Uses LuaSocket for TCP networking
    - Coroutine-based concurrency: each connection runs in its own coroutine
    - Non-blocking I/O with select()-based event loop
    - WebSocket: HTTP Upgrade handshake → frame-based messaging
    - TCP: raw message forwarding to Game processes

    Usage:
        lua server.lua [--tcp-port PORT] [--ws-port PORT] [--game-host HOST] [--game-port PORT]
]]

-- ============================================================
-- Bootstrap: Setup package path
-- ============================================================

local script_dir = debug.getinfo(1, "S").source:match("@(.*/)") or "."
script_dir = script_dir:gsub("/$", "")
local parent_dir = script_dir:gsub("/[^/]+$", "")
package.path = parent_dir .. "/?.lua;" .. parent_dir .. "/?/init.lua;" .. package.path

-- ============================================================
-- Imports
-- ============================================================

local socket     = require("socket")
local connection = require("gateway.connection")
local websocket  = require("gateway.websocket")
local heartbeat  = require("gateway.heartbeat")

-- ============================================================
-- Configuration
-- ============================================================

local config = {
    -- Server ports
    tcp_port = 9000,  -- TCP client connections
    kcp_port = 9001,  -- KCP client connections (placeholder)
    ws_port  = 9002,  -- WebSocket client connections

    -- Backend Game server
    game_host = "127.0.0.1",
    game_port = 7777,

    -- Heartbeat
    heartbeat_interval = 1.0,
    heartbeat_timeout  = 3.0,

    -- Connection limits
    max_connections = 1024,

    -- Buffer sizes
    recv_buf_size = 128 * 1024,

    -- Logging
    log_level = "info",
}

-- ============================================================
-- Argument Parsing
-- ============================================================

local cli_args = {...}

local function parse_args()
    local i = 1
    while i <= #cli_args do
        local arg = cli_args[i]
        if arg == "--tcp-port" and cli_args[i + 1] then
            config.tcp_port = tonumber(cli_args[i + 1]) or config.tcp_port
            i = i + 1
        elseif arg == "--ws-port" and cli_args[i + 1] then
            config.ws_port = tonumber(cli_args[i + 1]) or config.ws_port
            i = i + 1
        elseif arg == "--game-host" and cli_args[i + 1] then
            config.game_host = cli_args[i + 1]
            i = i + 1
        elseif arg == "--game-port" and cli_args[i + 1] then
            config.game_port = tonumber(cli_args[i + 1]) or config.game_port
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
Usage: lua server.lua [OPTIONS]

Options:
  --tcp-port PORT       TCP client port (default: 9000)
  --ws-port PORT        WebSocket client port (default: 9002)
  --game-host HOST      Backend Game server host (default: 127.0.0.1)
  --game-port PORT      Backend Game server port (default: 7777)
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

local coroutines = {}
local sockets_to_watch = {}

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

local function wait_readable(sock)
    local co = coroutine.running()
    sockets_to_watch[sock] = co
    coroutine.yield()
    sockets_to_watch[sock] = nil
end

local function sleep(seconds)
    local deadline = socket.gettime() + seconds
    local co = coroutine.running()
    local timer_id = tostring(co):match("0x(%x+)") or tostring(co)
    _timers = _timers or {}
    _timers[timer_id] = { co = co, deadline = deadline }
    coroutine.yield()
end

-- ============================================================
-- Connection Registry
-- ============================================================

local connections = {}  -- conn_id → connection object

local function register_connection(conn)
    local conn_id = conn.session_id
    connections[conn_id] = conn
    heartbeat.register(conn, function(cid)
        log("warn", "CONN", string.format("[%s] Heartbeat timeout, closing connection", cid))
        local c = connections[cid]
        if c then
            connection.close(c)
            connections[cid] = nil
            heartbeat.unregister(c)
        end
    end)
end

local function unregister_connection(conn)
    local conn_id = conn.session_id
    connections[conn_id] = nil
    heartbeat.unregister(conn)
end

-- ============================================================
-- TCP Server Helper
-- ============================================================

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

    local ok2, listen_err = server:listen(config.max_connections)
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
                local addr, client_port = client:getpeername()
                local client_id = string.format("%s:%d", addr, client_port)
                log("info", label, string.format("Accepted connection from %s", client_id))
                client:settimeout(0)  -- Non-blocking
                spawn(function()
                    on_accept(client, client_id)
                end, label .. "_" .. client_id)
            elseif err == "timeout" then
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
-- TCP Connection Handler (Port 9000)
-- ============================================================

local function handle_tcp_connection(client, client_id)
    local conn = connection.new(client, client_id, connection.PROTO_TCP)
    register_connection(conn)

    log("info", "TCP", string.format("[%s] TCP connection established", client_id))

    while true do
        local data, err, partial = client:receive("*a")
        if data then
            connection.append_recv(conn, data)
        elseif partial and #partial > 0 then
            connection.append_recv(conn, partial)
        elseif err == "timeout" or err == "wantread" then
            -- No data; process buffer then yield
        elseif err == "closed" then
            log("info", "TCP", string.format("[%s] Connection closed by client", client_id))
            break
        else
            log("error", "TCP", string.format("[%s] Receive error: %s", client_id, tostring(err)))
            break
        end

        -- Process complete messages from buffer
        -- TCP messages are newline-delimited for simplicity
        while true do
            local nl_pos = string.find(conn.recv_buf, "\n", 1, true)
            if not nl_pos then break end

            local msg = string.sub(conn.recv_buf, 1, nl_pos - 1)
            conn.recv_buf = string.sub(conn.recv_buf, nl_pos + 1)
            conn.msgs_received = conn.msgs_received + 1

            -- Check for heartbeat
            if msg == "HB" then
                heartbeat.heartbeat_received(conn)
                log("trace", "TCP", string.format("[%s] Heartbeat received", client_id))
            else
                log("debug", "TCP", string.format("[%s] Message: %s", client_id, msg))

                -- Route message to backend Game server
                local game_conn = socket.tcp()
                game_conn:settimeout(1)
                local ok, conn_err = game_conn:connect(config.game_host, config.game_port)
                if ok then
                    game_conn:send(msg .. "\n")
                    local resp, resp_err = game_conn:receive("*l")
                    game_conn:close()

                    if resp then
                        -- Send response back to client
                        connection.send_data(conn, resp .. "\n")
                        log("debug", "TCP", string.format("[%s] Response: %s", client_id, resp))
                    else
                        log("warn", "TCP", string.format("[%s] No response from Game: %s", client_id, tostring(resp_err)))
                        connection.send_data(conn, "ERR: no response from server\n")
                    end
                else
                    log("error", "TCP", string.format("[%s] Cannot connect to Game server: %s", client_id, tostring(conn_err)))
                    connection.send_data(conn, "ERR: backend unavailable\n")
                end
            end
        end

        wait_readable(client)
    end

    -- Cleanup
    unregister_connection(conn)
    connection.close(conn)
    log("info", "TCP", string.format("[%s] Connection closed", client_id))
end

-- ============================================================
-- WebSocket Connection Handler (Port 9002)
-- ============================================================

local function handle_ws_connection(client, client_id)
    -- Create connection in handshaking state
    local conn = connection.new(client, client_id, connection.PROTO_WEBSOCKET)

    log("info", "WS", string.format("[%s] WebSocket connection waiting for handshake", client_id))

    -- Phase 1: Wait for HTTP Upgrade request
    local handshake_done = false
    local handshake_buf = ""

    while not handshake_done do
        local data, err, partial = client:receive("*a")
        if data then
            handshake_buf = handshake_buf .. data
        elseif partial and #partial > 0 then
            handshake_buf = handshake_buf .. partial
        elseif err == "timeout" or err == "wantread" then
            -- No data yet
        elseif err == "closed" then
            log("info", "WS", string.format("[%s] Connection closed during handshake", client_id))
            client:close()
            return
        else
            log("error", "WS", string.format("[%s] Handshake receive error: %s", client_id, tostring(err)))
            client:close()
            return
        end

        -- Check if we have a complete HTTP request (ends with \r\n\r\n)
        local header_end = string.find(handshake_buf, "\r\n\r\n", 1, true)
        if header_end then
            local http_request = string.sub(handshake_buf, 1, header_end + 3)
            -- Save any data after the headers (shouldn't happen, but be safe)
            conn.recv_buf = string.sub(handshake_buf, header_end + 4)

            -- Perform WebSocket handshake
            local response, hs_err = websocket.handshake(http_request)
            if not response then
                log("error", "WS", string.format("[%s] Handshake failed: %s", client_id, tostring(hs_err)))
                -- Send 400 Bad Request
                client:send("HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
                client:close()
                return
            end

            -- Send 101 Switching Protocols
            local ok, send_err = client:send(response)
            if not ok then
                log("error", "WS", string.format("[%s] Failed to send handshake response: %s", client_id, tostring(send_err)))
                client:close()
                return
            end

            -- Upgrade complete
            conn.state = connection.STATE_ACTIVE
            handshake_done = true
            log("info", "WS", string.format("[%s] WebSocket handshake complete, connection upgraded", client_id))
        end

        if not handshake_done then
            wait_readable(client)
        end
    end

    -- Register for heartbeat (after handshake)
    register_connection(conn)

    -- Phase 2: WebSocket frame processing
    while true do
        local data, err, partial = client:receive("*a")
        if data then
            connection.append_recv(conn, data)
        elseif partial and #partial > 0 then
            connection.append_recv(conn, partial)
        elseif err == "timeout" or err == "wantread" then
            -- No data; process buffer
        elseif err == "closed" then
            log("info", "WS", string.format("[%s] WebSocket connection closed", client_id))
            break
        else
            log("error", "WS", string.format("[%s] Receive error: %s", client_id, tostring(err)))
            break
        end

        -- Process complete WebSocket frames
        while #conn.recv_buf >= 2 do
            local frame, frame_err = websocket.decode_frame(conn.recv_buf)
            if not frame then
                if frame_err and frame_err:match("incomplete") then
                    break  -- Wait for more data
                end
                log("error", "WS", string.format("[%s] Frame decode error: %s", client_id, tostring(frame_err)))
                break
            end

            -- Consume the frame from buffer
            conn.recv_buf = string.sub(conn.recv_buf, frame.consumed + 1)
            conn.msgs_received = conn.msgs_received + 1

            -- Handle frame by opcode
            if frame.opcode == websocket.OP_PING then
                -- Respond with PONG
                local pong_frame = websocket.encode_pong_frame(frame.data)
                connection.send_data(conn, pong_frame)
                heartbeat.heartbeat_received(conn)
                log("trace", "WS", string.format("[%s] PING received, sent PONG", client_id))

            elseif frame.opcode == websocket.OP_PONG then
                heartbeat.heartbeat_received(conn)
                log("trace", "WS", string.format("[%s] PONG received", client_id))

            elseif frame.opcode == websocket.OP_CLOSE then
                log("info", "WS", string.format("[%s] CLOSE frame received (code=%s)", client_id, tostring(frame.close_code)))
                -- Send close frame back
                local close_frame = websocket.encode_close_frame(frame.close_code or 1000)
                connection.send_data(conn, close_frame)
                connection.mark_closing(conn)
                break

            elseif frame.opcode == websocket.OP_TEXT or frame.opcode == websocket.OP_BINARY then
                log("debug", "WS", string.format("[%s] %s frame: %s", client_id,
                    websocket.opcode_name(frame.opcode), frame.data))

                -- Route message to backend Game server
                local game_conn = socket.tcp()
                game_conn:settimeout(1)
                local ok, conn_err = game_conn:connect(config.game_host, config.game_port)
                if ok then
                    game_conn:send(frame.data .. "\n")
                    local resp, resp_err = game_conn:receive("*l")
                    game_conn:close()

                    if resp then
                        -- Send response back as WebSocket text frame
                        websocket.ws_send(conn, resp, websocket.OP_TEXT)
                        log("debug", "WS", string.format("[%s] Response: %s", client_id, resp))
                    else
                        log("warn", "WS", string.format("[%s] No response from Game: %s", client_id, tostring(resp_err)))
                        websocket.ws_send(conn, "ERR: no response from server", websocket.OP_TEXT)
                    end
                else
                    log("error", "WS", string.format("[%s] Cannot connect to Game server: %s", client_id, tostring(conn_err)))
                    websocket.ws_send(conn, "ERR: backend unavailable", websocket.OP_TEXT)
                end
            else
                log("debug", "WS", string.format("[%s] Unknown opcode: 0x%X", client_id, frame.opcode))
            end
        end

        -- Check if we should exit (close frame received)
        if conn.state == connection.STATE_CLOSING then
            break
        end

        wait_readable(client)
    end

    -- Cleanup
    unregister_connection(conn)
    connection.close(conn)
    log("info", "WS", string.format("[%s] WebSocket connection closed", client_id))
end

-- ============================================================
-- Heartbeat Checker Coroutine
-- ============================================================

local function heartbeat_checker_loop()
    heartbeat.configure({
        interval = config.heartbeat_interval,
        timeout  = config.heartbeat_timeout,
    })
    heartbeat.start()

    while true do
        local events = heartbeat.tick(connections)
        for _, ev in ipairs(events) do
            if ev.type == "heartbeat_timeout" then
                log("warn", "HEARTBEAT", string.format(
                    "Connection timeout: %s (%s), count=%d",
                    ev.conn_id, ev.label, ev.count
                ))
            end
        end

        sleep(config.heartbeat_interval)
    end
end

-- ============================================================
-- Stats Reporter Coroutine
-- ============================================================

local function stats_reporter_loop()
    while true do
        sleep(10)  -- Every 10 seconds

        local count = 0
        local by_proto = { tcp = 0, websocket = 0, kcp = 0 }
        for _, conn in pairs(connections) do
            if connection.is_active(conn) then
                count = count + 1
                local proto = conn.proto
                by_proto[proto] = (by_proto[proto] or 0) + 1
            end
        end

        log("info", "STATS", string.format(
            "Active connections: %d (TCP=%d, WS=%d, KCP=%d)",
            count, by_proto.tcp or 0, by_proto.websocket or 0, by_proto.kcp or 0
        ))
    end
end

-- ============================================================
-- Main Event Loop
-- ============================================================

local function main_loop()
    log("info", "MAIN", "Gateway event loop started")

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
                        -- Will be picked up in the coroutine loop below
                    end
                end
            end
        end

        -- If no sockets to watch and no timers, do a short sleep
        if #sockets == 0 then
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
                socket.sleep(0.05)
            end
        end

        -- Wait for readable sockets with timeout
        if #sockets > 0 then
            local readable, _, err = socket.select(sockets, nil, 0.05)
            if readable then
                for _, sock in ipairs(readable) do
                    local co = sockets_to_watch[sock]
                    if co and coroutine.status(co) == "suspended" then
                        coroutine.resume(co)
                    end
                end
            end
        end

        -- Resume any timer-expired coroutines
        if _timers then
            for id, timer in pairs(_timers) do
                if now >= timer.deadline then
                    _timers[id] = nil
                    local co = timer.co
                    if coroutine.status(co) == "suspended" then
                        coroutine.resume(co)
                    end
                end
            end
        end
    end
end

-- ============================================================
-- Startup
-- ============================================================

local function main()
    parse_args()

    log("info", "MAIN", "============================================")
    log("info", "MAIN", "ChaosEngine Gateway v0.1.0")
    log("info", "MAIN", "============================================")
    log("info", "MAIN", string.format("TCP port: %d", config.tcp_port))
    log("info", "MAIN", string.format("WebSocket port: %d", config.ws_port))
    log("info", "MAIN", string.format("Game backend: %s:%d", config.game_host, config.game_port))
    log("info", "MAIN", string.format("Heartbeat: interval=%.1fs, timeout=%.1fs",
        config.heartbeat_interval, config.heartbeat_timeout))

    -- Create TCP server (port 9000)
    local tcp_server = create_server(config.tcp_port, "TCP", handle_tcp_connection)
    if not tcp_server then
        log("error", "MAIN", "Failed to create TCP server, exiting")
        os.exit(1)
    end

    -- Create WebSocket server (port 9002)
    local ws_server = create_server(config.ws_port, "WS", handle_ws_connection)
    if not ws_server then
        log("error", "MAIN", "Failed to create WebSocket server, exiting")
        os.exit(1)
    end

    -- Start heartbeat checker
    spawn(heartbeat_checker_loop, "heartbeat_checker")

    -- Start stats reporter
    spawn(stats_reporter_loop, "stats_reporter")

    -- Enter main event loop
    main_loop()
end

-- Run
main()
