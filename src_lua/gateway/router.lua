--[[
    router.lua — Message Router & Backend Connection Pool

    Routes client messages to backend Game services and
    forwards responses back to clients.

    Features:
    - Routing table: msg_type → backend address
    - Backend connection pool (persistent TCP connections to Game processes)
    - Forward: send to backend, receive response, relay to client
]]

local config   = require("gateway.config")
local protocol = require("gateway.protocol")

local socket = require("socket")

local M = {}

-- ============================================================
-- Routing Table
-- ============================================================

-- Default routing: all game data messages go to the first backend.
-- msg_type → { host, port }
local routes = {}

--- Set a route for a specific message type.
--- @param msg_type  number  Message type
--- @param host      string  Backend host
--- @param port      number  Backend port
function M.set_route(msg_type, host, port)
    routes[msg_type] = { host = host, port = port }
end

--- Remove a route.
--- @param msg_type  number
function M.remove_route(msg_type)
    routes[msg_type] = nil
end

--- Resolve the backend address for a message type.
--- Falls back to the first configured backend.
--- @param msg_type  number
--- @return table|nil  {host, port} or nil if no backend available
function M.route(msg_type)
    -- Check explicit route first
    local backend = routes[msg_type]
    if backend then
        return backend
    end

    -- Fallback to first configured backend
    if config.backends and #config.backends > 0 then
        return config.backends[1]
    end

    return nil
end

--- Get all configured backends.
--- @return table  Array of {host, port}
function M.get_backends()
    return config.backends
end

-- ============================================================
-- Backend Connection Pool
-- ============================================================

-- Pool of persistent TCP connections to backend Game services.
-- Key: "host:port" → array of {socket, in_use}
local backend_pool = {}

--- Get or create a backend connection.
--- @param host  string  Backend host
--- @param port  number  Backend port
--- @return userdata|nil  Socket, or nil on failure
--- @return string|nil    Error message
local function get_backend_conn(host, port)
    local key = string.format("%s:%d", host, port)

    if not backend_pool[key] then
        backend_pool[key] = {}
    end

    local pool = backend_pool[key]

    -- Try to find a free connection
    for _, entry in ipairs(pool) do
        if not entry.in_use then
            entry.in_use = true
            return entry.sock
        end
    end

    -- Create a new connection
    if #pool < config.backend_pool_size then
        local sock, err = socket.tcp()
        if not sock then
            return nil, "failed to create socket: " .. tostring(err)
        end

        sock:settimeout(5)  -- 5 second connect timeout
        local ok, conn_err = sock:connect(host, port)
        if not ok then
            sock:close()
            return nil, "failed to connect to " .. key .. ": " .. tostring(conn_err)
        end

        sock:settimeout(0)  -- Non-blocking for event loop

        local entry = { sock = sock, in_use = true }
        table.insert(pool, entry)
        return sock
    end

    return nil, "backend pool exhausted for " .. key
end

--- Release a backend connection back to the pool.
--- @param host  string
--- @param port  number
--- @param sock  userdata
local function release_backend_conn(host, port, sock)
    local key = string.format("%s:%d", host, port)
    local pool = backend_pool[key]
    if not pool then return end

    for _, entry in ipairs(pool) do
        if entry.sock == sock then
            entry.in_use = false
            return
        end
    end
end

--- Close all backend connections.
function M.close_backend_pool()
    for key, pool in pairs(backend_pool) do
        for _, entry in ipairs(pool) do
            entry.sock:close()
        end
        backend_pool[key] = nil
    end
end

-- ============================================================
-- Forward: Client → Backend → Client
-- ============================================================

--- Forward a client message to the backend and return the response.
--- This is a blocking operation within a coroutine context.
---
--- @param conn       table    Client Connection object
--- @param msg        table    Parsed message {msg_type, body}
--- @param timeout    number   Backend response timeout in seconds (default 10)
--- @return boolean   true on success
--- @return string|nil  Error message on failure
function M.forward(conn, msg, timeout)
    timeout = timeout or 10

    -- Resolve backend
    local backend = M.route(msg.msg_type)
    if not backend then
        return false, "no backend available for msg_type 0x" .. string.format("%04X", msg.msg_type)
    end

    -- Get backend connection
    local backend_sock, err = get_backend_conn(backend.host, backend.port)
    if not backend_sock then
        return false, err
    end

    -- Forward message to backend
    -- Re-pack with the same format for the backend
    local fwd_data = protocol.pack(msg.msg_type, msg.body)
    local sent, send_err = backend_sock:send(fwd_data)
    if not sent then
        release_backend_conn(backend.host, backend.port, backend_sock)
        return false, "backend send failed: " .. tostring(send_err)
    end

    -- Receive response from backend
    backend_sock:settimeout(timeout)
    local resp_data, recv_err = backend_sock:receive("*a")
    backend_sock:settimeout(0)  -- Restore non-blocking

    release_backend_conn(backend.host, backend.port, backend_sock)

    if not resp_data then
        return false, "backend recv failed: " .. tostring(recv_err)
    end

    -- Unpack response
    local resp, unpack_err = protocol.unpack(resp_data)
    if not resp then
        return false, "backend response unpack failed: " .. tostring(unpack_err)
    end

    -- Send response back to client
    local resp_packed = protocol.pack(resp.msg_type, resp.body)
    conn:send(resp_packed)

    return true
end

-- ============================================================
-- Backend Health Check
-- ============================================================

--- Ping a backend to check if it's alive.
--- @param host  string
--- @param port  number
--- @param timeout  number  Connect timeout in seconds
--- @return boolean  true if backend is reachable
function M.ping_backend(host, port, timeout)
    timeout = timeout or 2

    local sock, err = socket.tcp()
    if not sock then
        return false
    end

    sock:settimeout(timeout)
    local ok, conn_err = sock:connect(host, port)
    if ok then
        -- Send a PING and expect PONG
        local ping = protocol.make_ping()
        sock:send(ping)

        local resp, recv_err = sock:receive(1024)
        sock:close()

        if resp then
            local msg = protocol.unpack(resp)
            if msg and msg.msg_type == protocol.MSG_PONG then
                return true
            end
        end
        -- Backend may not support PING/PONG; TCP connect is enough
        return true
    else
        sock:close()
        return false
    end
end

--- Check all configured backends.
--- @return table  Array of {host, port, alive}
function M.check_all_backends()
    local results = {}
    for _, backend in ipairs(config.backends) do
        local alive = M.ping_backend(backend.host, backend.port)
        results[#results + 1] = {
            host  = backend.host,
            port  = backend.port,
            alive = alive,
        }
    end
    return results
end

-- ============================================================
-- Stats
-- ============================================================

--- Get router statistics.
--- @return table
function M.stats()
    local pool_stats = {}
    for key, pool in pairs(backend_pool) do
        local total = #pool
        local in_use = 0
        for _, entry in ipairs(pool) do
            if entry.in_use then
                in_use = in_use + 1
            end
        end
        pool_stats[key] = { total = total, in_use = in_use }
    end

    return {
        routes      = routes,
        backend_pool = pool_stats,
    }
end

return M
