--[[
    init.lua — Lua Router Entry Point

    Standalone Lua process that provides:
    - Port 9100: Game ↔ Router (Game processes connect here)
    - Port 9101: Router ↔ Router (cluster sync between Routers)

    Architecture:
    - Uses LuaSocket for TCP networking
    - Coroutine-based concurrency: each connection runs in its own coroutine
    - Non-blocking I/O with select()-based event loop
    - 1-second PING/PONG heartbeat on all connections
    - Consistent hash ring for Game message routing
    - Service registry for Game discovery
    - Cluster sync between Router peers
    - Cross-region routing

    Binary protocol format (from ce_net_base):
        [4B total_len][2B msg_type][N payload]
        total_len = 6 + payload_len (big-endian)

    Usage:
        lua init.lua [--node-id ID] [--region NAME] [--game-port PORT] [--cluster-port PORT]
]]

-- ============================================================
-- Bootstrap: Setup package path
-- ============================================================

-- Add parent directory to package.path so 'require("router.xxx")' works
local script_dir = debug.getinfo(1, "S").source:match("@(.*/)") or "."
-- Remove trailing slash
script_dir = script_dir:gsub("/$", "")
local parent_dir = script_dir:gsub("/[^/]+$", "")
package.path = parent_dir .. "/?.lua;" .. parent_dir .. "/?/init.lua;" .. package.path

-- ============================================================
-- Imports
-- ============================================================

local socket       = require("socket")
local config       = require("router.config")
local protocol     = require("router.protocol")
local registry     = require("router.registry")
local hash_ring    = require("router.hash_ring")
local router       = require("router.router")
local health       = require("router.health")
local cluster      = require("router.cluster")
local cross_region = require("router.cross_region")

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
        if arg == "--node-id" and args[i + 1] then
            config.node_id = args[i + 1]
            i = i + 1
        elseif arg == "--region" and args[i + 1] then
            config.region = args[i + 1]
            i = i + 1
        elseif arg == "--game-port" and args[i + 1] then
            config.game_port = tonumber(args[i + 1]) or config.game_port
            i = i + 1
        elseif arg == "--cluster-port" and args[i + 1] then
            config.cluster_port = tonumber(args[i + 1]) or config.cluster_port
            i = i + 1
        elseif arg == "--peer" and args[i + 1] then
            -- Format: node_id:host:port
            local node_id, host, port_str = args[i + 1]:match("^([^:]+):([^:]+):(%d+)$")
            if node_id and host and port_str then
                config.peers[#config.peers + 1] = {
                    node_id = node_id,
                    host    = host,
                    port    = tonumber(port_str),
                }
            end
            i = i + 1
        elseif arg == "--remote-region" and args[i + 1] then
            -- Format: region:host:port
            local region, host, port_str = args[i + 1]:match("^([^:]+):([^:]+):(%d+)$")
            if region and host and port_str then
                config.remote_regions[#config.remote_regions + 1] = {
                    region = region,
                    host   = host,
                    port   = tonumber(port_str),
                }
            end
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
  --node-id ID             Router node ID (default: router-1)
  --region NAME            Region name (default: default)
  --game-port PORT         Game connection port (default: 9100)
  --cluster-port PORT      Cluster sync port (default: 9101)
  --peer NODE:HOST:PORT    Add a cluster peer
  --remote-region REGION:HOST:PORT  Add a remote region Router
  --log-level LEVEL        Log level: trace, debug, info, warn, error (default: info)
  --help, -h               Show this help

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
-- TCP Server Helper
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

    local max_conn = (label == "GAME") and config.max_game_connections or config.max_cluster_connections
    local ok2, listen_err = server:listen(max_conn)
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
-- Game Connection Handler (Port 9100)
-- ============================================================

--- Handle a Game process connection.
--- Game processes connect here to register services and exchange messages.
local function handle_game_connection(client, client_id)
    local recv_buf = ""

    -- Register for heartbeat
    health.register(client_id, "Game-" .. client_id, function(cid)
        log("warn", "GAME", string.format("[%s] Heartbeat timeout, closing", cid))
        router.unregister_connection(cid)
        pcall(function() client:close() end)
    end)

    log("info", "GAME", string.format("[%s] Game connection established", client_id))

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
            log("info", "GAME", string.format("[%s] Connection closed", client_id))
            break
        else
            log("error", "GAME", string.format("[%s] Receive error: %s", client_id, tostring(err)))
            break
        end

        -- Process complete messages from buffer
        while #recv_buf >= protocol.HEADER_SIZE do
            local total_len = protocol.peek_len(recv_buf)
            if not total_len or total_len < protocol.HEADER_SIZE then
                recv_buf = ""
                break
            end
            if total_len > protocol.MAX_MSG_SIZE then
                log("error", "GAME", string.format("[%s] Message too large: %d bytes", client_id, total_len))
                recv_buf = ""
                break
            end
            if #recv_buf < total_len then
                break  -- Incomplete message, wait for more data
            end

            -- Extract and unpack message
            local msg_data = recv_buf:sub(1, total_len)
            recv_buf = recv_buf:sub(total_len + 1)

            local msg, unpack_err = protocol.unpack(msg_data)
            if not msg then
                log("error", "GAME", string.format("[%s] Unpack error: %s", client_id, tostring(unpack_err)))
            else
                -- Handle message based on type
                handle_game_message(client, client_id, msg)
            end
        end

        -- Yield to let other coroutines run
        wait_readable(client)
    end

    -- Cleanup
    health.unregister(client_id)
    router.unregister_connection(client_id)
    pcall(function() client:close() end)
    log("info", "GAME", string.format("[%s] Connection closed", client_id))
end

--- Handle a message from a Game connection.
local function handle_game_message(client, client_id, msg)
    local msg_type = msg.msg_type
    local body = msg.body

    -- Heartbeat messages
    if msg_type == protocol.MSG_PING then
        health.heartbeat_received(client_id)
        -- Send PONG back
        local pong = protocol.make_pong()
        pcall(function() client:send(pong) end)
        return
    end

    if msg_type == protocol.MSG_PONG then
        health.heartbeat_received(client_id)
        return
    end

    -- Service registration
    if msg_type == protocol.MSG_SERVICE_REGISTER then
        -- Parse: service_id|service_type|host|port|metadata
        local parts = {}
        for part in (body or ""):gmatch("[^|]+") do
            parts[#parts + 1] = part
        end
        if #parts >= 4 then
            local service_id   = parts[1]
            local service_type = parts[2]
            local host         = parts[3]
            local port         = tonumber(parts[4]) or 0
            local metadata     = parts[5] or "{}"

            router.on_service_registered(service_id, service_type, host, port, metadata)
            router.register_connection(client_id, client, service_id, host, port)

            log("info", "GAME", string.format("[%s] Service registered: %s (%s) at %s:%d",
                client_id, service_id, service_type, host, port))
        end
        return
    end

    -- Service unregistration
    if msg_type == protocol.MSG_SERVICE_UNREGISTER then
        local service_id = body
        router.on_service_unregistered(service_id)
        log("info", "GAME", string.format("[%s] Service unregistered: %s", client_id, service_id))
        return
    end

    -- Service heartbeat
    if msg_type == protocol.MSG_SERVICE_HEARTBEAT then
        local service_id = body
        registry.heartbeat(service_id)
        return
    end

    -- Service query
    if msg_type == protocol.MSG_SERVICE_QUERY then
        local service_type = body
        local services = registry.query(service_type, true)
        -- Build response: service_id|host|port;service_id|host|port;...
        local resp_parts = {}
        for _, svc in ipairs(services) do
            resp_parts[#resp_parts + 1] = string.format("%s|%s|%d",
                svc.service_id, svc.host, svc.port)
        end
        local resp_body = table.concat(resp_parts, ";")
        local resp = protocol.pack(protocol.MSG_SERVICE_QUERY_RESP, resp_body)
        pcall(function() client:send(resp) end)
        return
    end

    -- Game data routing
    if msg_type == protocol.MSG_GAME_DATA then
        -- Route to the appropriate Game instance using hash ring
        -- The body format is: key|inner_data
        -- where key is used for consistent hash routing
        local pipe_pos = body:find("|", 1, true)
        if pipe_pos then
            local key = body:sub(1, pipe_pos - 1)
            local inner_data = body:sub(pipe_pos + 1)
            local ok, route_err = router.route_by_hash(key, msg_type, inner_data)
            if not ok then
                log("warn", "GAME", string.format("[%s] Route failed: %s", client_id, tostring(route_err)))
            end
        else
            -- No key, broadcast to all game services
            local sent = router.broadcast("game", msg_type, body)
            log("debug", "GAME", string.format("[%s] Broadcast to %d game services", client_id, sent))
        end
        return
    end

    -- Cross-region messages: forward to cross_region module
    if protocol.is_cross_region_msg(msg_type) then
        local result, cr_err = cross_region.handle_incoming(body)
        if result then
            if result.should_relay then
                cross_region.send_to_region(result.dst_region, protocol.MSG_CROSS_REGION, result.relay_body)
            else
                -- Deliver locally: re-route the inner message
                router.route_by_hash(result.src_region .. "_" .. tostring(result.inner_type),
                    result.inner_type, result.inner_data)
            end
        else
            log("warn", "GAME", string.format("[%s] Cross-region error: %s", client_id, tostring(cr_err)))
        end
        return
    end

    -- Default: log unknown message types
    log("debug", "GAME", string.format("[%s] Message: %s (0x%04X) len=%d",
        client_id, protocol.type_name(msg_type), msg_type, #body))
end

-- ============================================================
-- Cluster Connection Handler (Port 9101)
-- ============================================================

--- Handle a cluster peer connection.
--- Other Router instances connect here for cluster sync.
local function handle_cluster_connection(client, client_id)
    local recv_buf = ""

    -- The peer's node_id will be set when we receive a CLUSTER_JOIN message
    local peer_node_id = client_id  -- Temporary ID

    -- Register for heartbeat
    health.register(client_id, "Cluster-" .. client_id, function(cid)
        log("warn", "CLUSTER", string.format("[%s] Heartbeat timeout", cid))
        cluster.handle_peer_disconnect(peer_node_id)
        pcall(function() client:close() end)
    end)

    log("info", "CLUSTER", string.format("[%s] Cluster connection established", client_id))

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
            log("info", "CLUSTER", string.format("[%s] Connection closed", client_id))
            break
        else
            log("error", "CLUSTER", string.format("[%s] Receive error: %s", client_id, tostring(err)))
            break
        end

        -- Process complete messages from buffer
        while #recv_buf >= protocol.HEADER_SIZE do
            local total_len = protocol.peek_len(recv_buf)
            if not total_len or total_len < protocol.HEADER_SIZE then
                recv_buf = ""
                break
            end
            if total_len > protocol.MAX_MSG_SIZE then
                log("error", "CLUSTER", string.format("[%s] Message too large: %d bytes", client_id, total_len))
                recv_buf = ""
                break
            end
            if #recv_buf < total_len then
                break  -- Incomplete message
            end

            local msg_data = recv_buf:sub(1, total_len)
            recv_buf = recv_buf:sub(total_len + 1)

            local msg, unpack_err = protocol.unpack(msg_data)
            if not msg then
                log("error", "CLUSTER", string.format("[%s] Unpack error: %s", client_id, tostring(unpack_err)))
            else
                -- Handle cluster message
                handle_cluster_message(client, client_id, peer_node_id, msg)
            end
        end

        wait_readable(client)
    end

    -- Cleanup
    health.unregister(client_id)
    cluster.handle_peer_disconnect(peer_node_id)
    pcall(function() client:close() end)
    log("info", "CLUSTER", string.format("[%s] Connection closed", client_id))
end

--- Handle a message from a cluster peer connection.
local function handle_cluster_message(client, client_id, peer_node_id, msg)
    local msg_type = msg.msg_type
    local body = msg.body

    -- Heartbeat
    if msg_type == protocol.MSG_PING then
        health.heartbeat_received(client_id)
        local pong = protocol.make_pong()
        pcall(function() client:send(pong) end)
        return
    end

    if msg_type == protocol.MSG_PONG then
        health.heartbeat_received(client_id)
        return
    end

    -- Cluster join: parse node_id from body
    if msg_type == protocol.MSG_CLUSTER_JOIN then
        local parts = {}
        for part in (body or ""):gmatch("[^|]+") do
            parts[#parts + 1] = part
        end
        if #parts >= 3 then
            peer_node_id = parts[1]
            local host = parts[2]
            local port = tonumber(parts[3]) or 0
            local region = parts[4] or "unknown"
            cluster.add_peer(peer_node_id, host, port, region)
            cluster.register_peer_connection(peer_node_id, client)
            log("info", "CLUSTER", string.format("[%s] Peer identified as: %s", client_id, peer_node_id))
        end
        return
    end

    -- Delegate other cluster messages to cluster module
    if protocol.is_cluster_msg(msg_type) then
        cluster.handle_message(peer_node_id, msg_type, body)
        return
    end

    -- Cross-region messages
    if protocol.is_cross_region_msg(msg_type) then
        local result, cr_err = cross_region.handle_incoming(body)
        if result then
            if result.should_relay then
                cross_region.send_to_region(result.dst_region, protocol.MSG_CROSS_REGION, result.relay_body)
            else
                -- Deliver locally
                router.route_by_hash(result.src_region .. "_" .. tostring(result.inner_type),
                    result.inner_type, result.inner_data)
            end
        end
        return
    end

    log("debug", "CLUSTER", string.format("[%s] Message: %s (0x%04X)", client_id, protocol.type_name(msg_type), msg_type))
end

-- ============================================================
-- Heartbeat Checker Coroutine
-- ============================================================

--- Periodic coroutine that checks heartbeats and sends PINGs.
local function heartbeat_loop()
    health.configure({
        interval = config.heartbeat_interval,
        timeout  = config.heartbeat_timeout,
    })
    health.start()

    -- Send function: looks up the socket by conn_id and sends data
    -- This is a simplified approach; in production, maintain a conn_id→socket map
    local function send_fn(conn_id, data)
        -- The health module's send_ping is called, but actual sending
        -- is handled by the connection coroutines themselves.
        -- For now, we rely on the connection handlers to send PINGs.
        -- This is a placeholder for future direct-send capability.
        return true
    end

    while true do
        local events = health.tick(send_fn)

        -- Log timeout events
        for _, ev in ipairs(events) do
            log("warn", "HEARTBEAT", string.format(
                "%s heartbeat timeout #%d", ev.label, ev.count
            ))
        end

        sleep(config.heartbeat_interval)
    end
end

-- ============================================================
-- Cluster Sync Coroutine
-- ============================================================

--- Periodic coroutine that syncs registry state to peers.
local function cluster_sync_loop()
    while true do
        sleep(5)  -- Sync every 5 seconds

        if cluster.is_running() then
            cluster.sync_registry_to_peers()
        end
    end
end

-- ============================================================
-- Service Cleanup Coroutine
-- ============================================================

--- Periodic coroutine that cleans up stale services.
local function service_cleanup_loop()
    while true do
        sleep(10)  -- Every 10 seconds

        local dead = registry.cleanup_stale(config.heartbeat_timeout)
        if dead > 0 then
            log("info", "REGISTRY", string.format("Marked %d services as dead", dead))
            -- Remove dead services from hash ring
            router.sync_hash_ring()
        end

        local purged = registry.purge_dead()
        if purged > 0 then
            log("info", "REGISTRY", string.format("Purged %d dead services", purged))
        end
    end
end

-- ============================================================
-- Stats Reporter Coroutine
-- ============================================================

local function stats_reporter_loop()
    while true do
        sleep(30)  -- Every 30 seconds

        local reg_stats = registry.stats()
        local ring_stats = hash_ring.stats()
        local cluster_stats = cluster.stats()
        local hb_stats = health.stats()

        log("info", "STATS", string.format(
            "Services: %d alive/%d dead | Ring: %d nodes/%d vnodes | " ..
            "Cluster: %d/%d peers alive | Health: %d/%d alive",
            reg_stats.alive, reg_stats.dead,
            ring_stats.physical_nodes, ring_stats.virtual_nodes,
            cluster_stats.alive_peers, cluster_stats.total_peers,
            hb_stats.alive, hb_stats.total
        ))
    end
end

-- ============================================================
-- Main Event Loop
-- ============================================================

local function main_loop()
    log("info", "MAIN", "Router event loop started")

    while true do
        -- Collect all watched sockets
        local sockets = {}
        for sock, _ in pairs(sockets_to_watch) do
            sockets[#sockets + 1] = sock
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
                    -- Mark the coroutine waiting on this socket as ready
                    -- (actual resumption happens below)
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
-- Signal Handling / Graceful Shutdown
-- ============================================================

local function setup_shutdown_watcher()
    spawn(function()
        while true do
            local f = io.open("/tmp/chaos_router_stop", "r")
            if f then
                f:close()
                os.remove("/tmp/chaos_router_stop")
                graceful_shutdown()
            end
            sleep(1)
        end
    end, "shutdown_watcher")
end

local function graceful_shutdown()
    log("info", "MAIN", "Shutting down gracefully...")
    health.stop()
    cluster.stop()
    cross_region.disconnect_all()
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
║         ChaosEngine Router v%s                      ║
║         Node:    %-34s ║
║         Region:  %-34s ║
║         Game Port:    %-5d                          ║
║         Cluster Port: %-5d                          ║
╚══════════════════════════════════════════════════════╝
]], config.version, config.node_id, config.region,
   config.game_port, config.cluster_port))

-- Setup shutdown watcher
setup_shutdown_watcher()

-- Initialize cluster module
cluster.set_local_host("127.0.0.1")

-- Add configured peers
for _, peer in ipairs(config.peers) do
    cluster.add_peer(peer.node_id, peer.host, peer.port, config.region)
end

-- Add configured remote regions
for _, rr in ipairs(config.remote_regions) do
    cross_region.add_region(rr.region, rr.host, rr.port)
end

-- Start cluster
cluster.start()

-- Connect to peers
local peer_count = cluster.connect_all_peers()
if peer_count > 0 then
    log("info", "MAIN", string.format("Connected to %d cluster peers", peer_count))
end

-- Connect to remote regions
local region_count = cross_region.connect_all_regions()
if region_count > 0 then
    log("info", "MAIN", string.format("Connected to %d remote regions", region_count))
end

-- Start heartbeat checker coroutine
spawn(heartbeat_loop, "heartbeat")

-- Start cluster sync coroutine
spawn(cluster_sync_loop, "cluster_sync")

-- Start service cleanup coroutine
spawn(service_cleanup_loop, "service_cleanup")

-- Start stats reporter
spawn(stats_reporter_loop, "stats_reporter")

-- Start Game server (port 9100)
local game_srv = create_server(config.game_port, "GAME", handle_game_connection)
if not game_srv then
    log("error", "MAIN", "Failed to start Game server on port " .. config.game_port)
    os.exit(1)
end

-- Start Cluster server (port 9101)
local cluster_srv = create_server(config.cluster_port, "CLUSTER", handle_cluster_connection)
if not cluster_srv then
    log("error", "MAIN", "Failed to start Cluster server on port " .. config.cluster_port)
    os.exit(1)
end

log("info", "MAIN", "Router initialization complete")

-- Run main event loop (blocks forever)
main_loop()
