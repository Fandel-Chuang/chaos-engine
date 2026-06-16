--[[
    init.lua — Friend Service Entry Point

    Standalone Lua process for the Friend service.
    Connects to Router cluster for message routing and DBProxy
    for friend data persistence.

    Usage:
        lua init.lua [--router-host HOST] [--router-port PORT]
                     [--dbproxy-host HOST] [--dbproxy-port PORT]
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

local socket        = require("socket")
local config        = require("services.config_friend")
local service_types = require("services.service_types")
local friend        = require("services.friend")

-- ============================================================
-- Argument Parsing
-- ============================================================

local cli_args = {...}

local function parse_args()
    local args = cli_args
    local i = 1
    while i <= #args do
        local arg = args[i]
        if arg == "--router-host" and args[i + 1] then
            config.router_host = args[i + 1]
            i = i + 1
        elseif arg == "--router-port" and args[i + 1] then
            config.router_port = tonumber(args[i + 1]) or config.router_port
            i = i + 1
        elseif arg == "--dbproxy-host" and args[i + 1] then
            config.dbproxy_host = args[i + 1]
            i = i + 1
        elseif arg == "--dbproxy-port" and args[i + 1] then
            config.dbproxy_port = tonumber(args[i + 1]) or config.dbproxy_port
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
  --router-host HOST    Router host (default: 127.0.0.1)
  --router-port PORT    Router port (default: 9000)
  --dbproxy-host HOST   DBProxy host (default: 127.0.0.1)
  --dbproxy-port PORT   DBProxy port (default: 9013)
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
-- DBProxy Client (port 9013)
-- ============================================================

local dbproxy_sock = nil

--- Connect to DBProxy.
local function dbproxy_connect()
    local sock, err = socket.tcp()
    if not sock then
        log("error", "DBPROXY", "Failed to create socket: " .. tostring(err))
        return false
    end

    sock:settimeout(config.heartbeat_timeout)
    local ok, conn_err = sock:connect(config.dbproxy_host, config.dbproxy_port)
    if not ok then
        log("warn", "DBPROXY", string.format("Cannot connect to %s:%d: %s",
            config.dbproxy_host, config.dbproxy_port, tostring(conn_err)))
        sock:close()
        return false
    end

    sock:settimeout(0)  -- Non-blocking
    dbproxy_sock = sock
    log("info", "DBPROXY", string.format("Connected to DBProxy at %s:%d",
        config.dbproxy_host, config.dbproxy_port))
    return true
end

--- Send a message to DBProxy using the binary protocol.
--- Format: [4B msg_len][1B msg_type][N payload]
local function dbproxy_send(msg_type, payload)
    if not dbproxy_sock then
        if not dbproxy_connect() then
            return false
        end
    end

    payload = payload or ""
    local msg_len = 5 + #payload

    -- Pack: 4B big-endian length + 1B type + payload
    local packed = string.char(
        (msg_len >> 24) & 0xFF,
        (msg_len >> 16) & 0xFF,
        (msg_len >> 8)  & 0xFF,
        msg_len         & 0xFF,
        msg_type
    ) .. payload

    local ok, err = dbproxy_sock:send(packed)
    if not ok then
        log("error", "DBPROXY", "Send failed: " .. tostring(err))
        dbproxy_sock:close()
        dbproxy_sock = nil
        return false
    end

    return true
end

-- ============================================================
-- Router Connection (via ce_rpc_call C binding or TCP)
-- ============================================================

local router_sock = nil

--- Connect to Router.
local function router_connect()
    local sock, err = socket.tcp()
    if not sock then
        log("error", "ROUTER", "Failed to create socket: " .. tostring(err))
        return false
    end

    sock:settimeout(config.heartbeat_timeout)
    local ok, conn_err = sock:connect(config.router_host, config.router_port)
    if not ok then
        log("warn", "ROUTER", string.format("Cannot connect to %s:%d: %s",
            config.router_host, config.router_port, tostring(conn_err)))
        sock:close()
        return false
    end

    sock:settimeout(0)  -- Non-blocking
    router_sock = sock
    log("info", "ROUTER", string.format("Connected to Router at %s:%d",
        config.router_host, config.router_port))
    return true
end

--- Register this service with the Router.
local function router_register()
    if not router_sock then
        if not router_connect() then
            return false
        end
    end

    -- Build registration message: [4B len][2B type=0x2000][1B service_type][1B name_len][N name]
    local name = config.service_name
    local payload = string.char(config.service_type, #name) .. name
    local total_len = 6 + #payload  -- 4B len + 2B type + payload

    local packed = string.char(
        (total_len >> 24) & 0xFF,
        (total_len >> 16) & 0xFF,
        (total_len >> 8)  & 0xFF,
        total_len         & 0xFF,
        0x20, 0x00  -- CE_RPC_MSG_REGISTER = 0x2000
    ) .. payload

    local ok, err = router_sock:send(packed)
    if not ok then
        log("error", "ROUTER", "Registration send failed: " .. tostring(err))
        return false
    end

    log("info", "ROUTER", "Registered service " .. name .. " (type=0x" ..
        string.format("%02X", config.service_type) .. ")")
    return true
end

-- ============================================================
-- Protocol: Message Packing/Unpacking
-- ============================================================

--- Pack a uint32 big-endian.
local function pack_u32(val)
    return string.char(
        (val >> 24) & 0xFF,
        (val >> 16) & 0xFF,
        (val >> 8)  & 0xFF,
        val         & 0xFF
    )
end

--- Unpack a uint32 big-endian from string at position.
local function unpack_u32(data, pos)
    local b1, b2, b3, b4 = string.byte(data, pos, pos + 3)
    return (b1 << 24) | (b2 << 16) | (b3 << 8) | b4
end

--- Unpack a uint16 big-endian from string at position.
local function unpack_u16(data, pos)
    local b1, b2 = string.byte(data, pos, pos + 1)
    return (b1 << 8) | b2
end

-- ============================================================
-- Request Handler
-- ============================================================

--- Handle an incoming RPC request.
--- @param msg_type  number  Message type
--- @param payload   string  Raw payload
--- @return string|nil  Response payload (nil if no response needed)
local function handle_request(msg_type, payload)
    -- Parse the RPC call payload:
    -- [1B target_service][1B method_len][N method][4B params_len][N params]
    if #payload < 2 then
        return nil
    end

    local target_service = string.byte(payload, 1)
    local method_len = string.byte(payload, 2)

    if #payload < 2 + method_len + 4 then
        return nil
    end

    local method = payload:sub(3, 2 + method_len)
    local params_len = unpack_u32(payload, 3 + method_len)
    local params = ""
    if params_len > 0 and #payload >= 3 + method_len + 4 + params_len then
        params = payload:sub(3 + method_len + 4, 3 + method_len + 4 + params_len - 1)
    end

    log("debug", "HANDLER", string.format("RPC: method=%s, params_len=%d", method, params_len))

    -- Parse params as simple key=value pairs (for testing)
    local function parse_params(str)
        local result = {}
        if str and #str > 0 then
            for key, val in str:gmatch("([%w_]+)=([%w_]+)") do
                result[key] = val
            end
        end
        return result
    end

    local args = parse_params(params)

    -- Dispatch to friend module
    if method == "add_friend" then
        local ok, err = friend.add_friend(args.player_id, args.friend_id)
        if ok then
            return "ok"
        else
            return "error:" .. (err or "unknown")
        end

    elseif method == "remove_friend" then
        local ok, err = friend.remove_friend(args.player_id, args.friend_id)
        if ok then
            return "ok"
        else
            return "error:" .. (err or "unknown")
        end

    elseif method == "get_friend_list" then
        local list = friend.get_friend_list(args.player_id)
        local parts = {}
        for _, f in ipairs(list) do
            parts[#parts + 1] = f.friend_id
        end
        return table.concat(parts, ",")

    elseif method == "get_online_friends" then
        local online = friend.get_online_friends(args.player_id)
        return table.concat(online, ",")

    elseif method == "is_friend" then
        local result = friend.is_friend(args.player_id, args.friend_id)
        return result and "true" or "false"

    elseif method == "set_online_status" then
        local status = args.status
        if status == "true" or status == "1" then
            friend.set_online_status(args.player_id, true)
        else
            friend.set_online_status(args.player_id, false)
        end
        return "ok"

    elseif method == "add_blacklist" then
        local ok, err = friend.add_blacklist(args.player_id, args.blocked_id)
        if ok then
            return "ok"
        else
            return "error:" .. (err or "unknown")
        end

    elseif method == "remove_blacklist" then
        local ok, err = friend.remove_blacklist(args.player_id, args.blocked_id)
        if ok then
            return "ok"
        else
            return "error:" .. (err or "unknown")
        end

    elseif method == "get_stats" then
        local stats = friend.get_stats()
        return string.format("players=%d,friends=%d,online=%d",
            stats.total_players, stats.total_friendships, stats.total_online)

    else
        log("warn", "HANDLER", "Unknown method: " .. method)
        return "error:unknown_method"
    end
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
-- Router Message Receiver Coroutine
-- ============================================================

local function router_receiver()
    log("info", "ROUTER", "Receiver coroutine started")

    local recv_buf = ""

    while true do
        if not router_sock then
            sleep(1)
            router_connect()
        else
            -- Try to receive data
            local data, err, partial = router_sock:receive("*a")
            if data then
                recv_buf = recv_buf .. data
            elseif partial and #partial > 0 then
                recv_buf = recv_buf .. partial
            elseif err == "closed" then
                log("warn", "ROUTER", "Connection closed by Router")
                router_sock:close()
                router_sock = nil
                recv_buf = ""
                sleep(1)
            end

            -- Process complete messages from buffer
            while #recv_buf >= 6 do
                local msg_len = unpack_u32(recv_buf, 1)
                if msg_len > 256 * 1024 then
                    log("error", "ROUTER", "Message too large: " .. msg_len)
                    recv_buf = ""
                    break
                end
                if #recv_buf < msg_len then
                    break  -- Incomplete message
                end

                local msg_data = recv_buf:sub(1, msg_len)
                recv_buf = recv_buf:sub(msg_len + 1)

                local msg_type = unpack_u16(msg_data, 5)
                local payload = msg_data:sub(7, msg_len)

                -- Handle different message types
                if msg_type == 0x2001 then  -- REGISTER_RESP
                    log("info", "ROUTER", "Registration confirmed")
                elseif msg_type == 0x2003 then  -- HEARTBEAT_RESP
                    log("trace", "ROUTER", "Heartbeat response")
                elseif msg_type == 0x2100 then  -- RPC_CALL
                    local response = handle_request(msg_type, payload)
                    if response then
                        -- Send RPC response back
                        -- [4B len][2B type=0x2101][N response]
                        local resp_payload = response
                        local resp_len = 6 + #resp_payload
                        local resp_packed = pack_u32(resp_len) ..
                            string.char(0x21, 0x01) .. resp_payload
                        if router_sock then
                            router_sock:send(resp_packed)
                        end
                    end
                end
            end

            if router_sock then
                wait_readable(router_sock)
            end
        end
    end
end

-- ============================================================
-- Heartbeat Coroutine
-- ============================================================

local function heartbeat_loop()
    log("info", "HEARTBEAT", "Heartbeat coroutine started")

    while true do
        sleep(config.heartbeat_interval)

        if router_sock then
            -- Send heartbeat: [4B len][2B type=0x2002][1B service_type]
            local payload = string.char(config.service_type)
            local total_len = 6 + #payload
            local packed = pack_u32(total_len) .. string.char(0x20, 0x02) .. payload
            local ok, err = router_sock:send(packed)
            if not ok then
                log("warn", "HEARTBEAT", "Heartbeat send failed: " .. tostring(err))
            end
        end
    end
end

-- ============================================================
-- Stats Reporter Coroutine
-- ============================================================

local function stats_reporter_loop()
    while true do
        sleep(10)  -- Every 10 seconds

        local stats = friend.get_stats()
        log("info", "STATS", string.format(
            "Players: %d, Friendships: %d, Online: %d",
            stats.total_players, stats.total_friendships, stats.total_online
        ))
    end
end

-- ============================================================
-- Main Event Loop
-- ============================================================

local function main_loop()
    log("info", "MAIN", "Friend service event loop started")

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
                end
            end
        end

        -- If no sockets to watch, do a short sleep
        if #sockets == 0 then
            socket.sleep(0.01)
        else
            -- Use select to wait for readable sockets
            socket.select(sockets, nil, 0.1)
        end

        -- Resume ready coroutines
        local to_resume = {}
        for _, entry in ipairs(coroutines) do
            if coroutine.status(entry.co) == "suspended" then
                table.insert(to_resume, entry)
            end
        end

        for _, entry in ipairs(to_resume) do
            local ok, err = coroutine.resume(entry.co)
            if not ok then
                log("error", "COROUTINE", string.format("[%s] resume error: %s",
                    entry.name, tostring(err)))
            end
        end

        -- Remove dead coroutines
        local alive = {}
        for _, entry in ipairs(coroutines) do
            if coroutine.status(entry.co) ~= "dead" then
                alive[#alive + 1] = entry
            end
        end
        coroutines = alive
    end
end

-- ============================================================
-- Startup
-- ============================================================

parse_args()

log("info", "MAIN", string.format("Friend Service v%s starting", config.version))
log("info", "MAIN", string.format("Router: %s:%d, DBProxy: %s:%d",
    config.router_host, config.router_port,
    config.dbproxy_host, config.dbproxy_port))

-- Connect to DBProxy
dbproxy_connect()

-- Connect to Router and register
router_connect()
router_register()

-- Spawn coroutines
spawn(router_receiver, "router_receiver")
spawn(heartbeat_loop, "heartbeat")
spawn(stats_reporter_loop, "stats_reporter")

-- Run main loop
main_loop()
