--[[
    connection.lua — Gateway Connection Management

    Manages client connections for the Gateway process.
    Supports multiple protocol types: TCP, UDP, KCP, WebSocket.

    Provides two APIs:
    1. Simple API (used by server.lua):
       connection.new(sock, client_addr, proto) → conn
       connection.append_recv(conn, data)
       connection.send_data(conn, data)
       connection.close(conn)

    2. Manager API (used by external code):
       ConnectionManager:create(opts) → conn
       ConnectionManager:get(id) → conn
]]

local M = {}

-- ============================================================
-- Protocol Type Constants (string-based, used by server.lua)
-- ============================================================

M.PROTO_TCP       = "tcp"
M.PROTO_UDP       = "udp"
M.PROTO_KCP       = "kcp"
M.PROTO_WEBSOCKET = "websocket"

-- Numeric protocol types (used by ConnectionManager)
local ProtocolType = {
    TCP       = 1,
    UDP       = 2,
    KCP       = 3,
    WEBSOCKET = 4,
}

-- Map string proto → numeric
local proto_to_num = {
    [M.PROTO_TCP]       = ProtocolType.TCP,
    [M.PROTO_UDP]       = ProtocolType.UDP,
    [M.PROTO_KCP]       = ProtocolType.KCP,
    [M.PROTO_WEBSOCKET] = ProtocolType.WEBSOCKET,
}

-- Map numeric → string proto
local num_to_proto = {
    [ProtocolType.TCP]       = M.PROTO_TCP,
    [ProtocolType.UDP]       = M.PROTO_UDP,
    [ProtocolType.KCP]       = M.PROTO_KCP,
    [ProtocolType.WEBSOCKET] = M.PROTO_WEBSOCKET,
}

-- ============================================================
-- Connection State Constants
-- ============================================================

M.STATE_HANDSHAKING = "handshaking"
M.STATE_ACTIVE      = "active"
M.STATE_CLOSING     = "closing"
M.STATE_CLOSED      = "closed"

local ConnectionState = {
    DISCONNECTED = 0,
    CONNECTING   = 1,
    CONNECTED    = 2,
    CLOSING      = 3,
}

-- ============================================================
-- Connection Object (Simple API)
-- ============================================================

--- Create a new connection object (simple API, used by server.lua).
--- @param sock         table   LuaSocket socket object
--- @param client_addr  string  Client address (ip:port)
--- @param proto        string  Protocol type (PROTO_TCP, PROTO_WEBSOCKET, etc.)
--- @param opts         table   Optional configuration
--- @return table  Connection object
function M.new(sock, client_addr, proto, opts)
    opts = opts or {}
    proto = proto or M.PROTO_TCP

    local conn = {
        -- Socket
        sock        = sock,
        client_addr = client_addr,

        -- Protocol
        proto       = proto,
        state       = M.STATE_ACTIVE,

        -- Buffers
        recv_buf    = "",
        send_buf    = "",

        -- Metadata
        player_id   = opts.player_id,
        session_id  = opts.session_id or string.format("%s-%d", client_addr, os.time()),

        -- Statistics
        bytes_sent     = 0,
        bytes_received = 0,
        msgs_sent      = 0,
        msgs_received  = 0,
        connected_at   = os.time(),
        last_active    = os.time(),

        -- WebSocket-specific
        ws_path        = nil,

        -- Manager-compatible fields
        id           = opts.id or 0,
        created_at   = os.time(),
        last_recv    = 0,
        last_send    = 0,
        packets_sent = 0,
        packets_recv = 0,
        user_data    = opts.user_data or {},
    }

    -- If WebSocket, start in handshaking state
    if proto == M.PROTO_WEBSOCKET then
        conn.state = M.STATE_HANDSHAKING
    end

    return conn
end

-- ============================================================
-- Protocol Helpers
-- ============================================================

--- Set the connection's protocol type.
function M.set_proto(conn, proto)
    conn.proto = proto
end

--- Set the connection's state.
function M.set_state(conn, state)
    conn.state = state
end

--- Check if the connection is in active state.
function M.is_active(conn)
    return conn.state == M.STATE_ACTIVE
end

--- Check if the connection is a WebSocket connection.
function M.is_websocket(conn)
    return conn.proto == M.PROTO_WEBSOCKET
end

--- Check if the connection is a TCP connection.
function M.is_tcp(conn)
    return conn.proto == M.PROTO_TCP
end

--- Check if the connection is a KCP connection.
function M.is_kcp(conn)
    return conn.proto == M.PROTO_KCP
end

--- Get protocol name.
function M.get_protocol_name(conn)
    return conn.proto or "UNKNOWN"
end

-- ============================================================
-- Buffer Operations
-- ============================================================

--- Append data to the receive buffer.
function M.append_recv(conn, data)
    conn.recv_buf = conn.recv_buf .. data
    conn.bytes_received = (conn.bytes_received or 0) + #data
    conn.bytes_recv = (conn.bytes_recv or 0) + #data
    conn.packets_recv = (conn.packets_recv or 0) + 1
    conn.last_active = os.time()
    conn.last_recv = os.time()
end

--- Send data through the connection.
function M.send_data(conn, data)
    local ok, err = conn.sock:send(data)
    if ok then
        conn.bytes_sent = (conn.bytes_sent or 0) + #data
        conn.msgs_sent = (conn.msgs_sent or 0) + 1
        conn.packets_sent = (conn.packets_sent or 0) + 1
        conn.last_active = os.time()
        conn.last_send = os.time()
    end
    return ok, err
end

-- ============================================================
-- Connection Lifecycle
-- ============================================================

--- Close the connection.
function M.close(conn)
    if conn.state == M.STATE_CLOSED then return end
    conn.state = M.STATE_CLOSED
    pcall(function() conn.sock:close() end)
end

--- Mark connection as closing (for graceful WebSocket close).
function M.mark_closing(conn)
    conn.state = M.STATE_CLOSING
end

--- Check if the connection is closed.
function M.is_closed(conn)
    return conn.state == M.STATE_CLOSED
end

--- Check if connection is alive (Manager-compatible).
function M.is_alive(conn)
    return conn.state == M.STATE_ACTIVE or conn.state == M.STATE_HANDSHAKING
end

-- ============================================================
-- Statistics
-- ============================================================

--- Get connection statistics.
function M.stats(conn)
    return {
        client_addr    = conn.client_addr,
        proto          = conn.proto,
        state          = conn.state,
        session_id     = conn.session_id,
        player_id      = conn.player_id,
        bytes_sent     = conn.bytes_sent,
        bytes_received = conn.bytes_received,
        msgs_sent      = conn.msgs_sent,
        msgs_received  = conn.msgs_received,
        connected_at   = conn.connected_at,
        last_active    = conn.last_active,
        idle_seconds   = os.time() - conn.last_active,
        uptime_seconds = os.time() - conn.connected_at,
        ws_path        = conn.ws_path,
    }
end

--- Get a summary string for logging.
function M.summary(conn)
    return string.format(
        "[%s] %s/%s player=%s msgs=%d/%d bytes=%d/%d idle=%ds",
        conn.client_addr,
        conn.proto,
        conn.state,
        conn.player_id or "-",
        conn.msgs_received,
        conn.msgs_sent,
        conn.bytes_received,
        conn.bytes_sent,
        os.time() - conn.last_active
    )
end

-- ============================================================
-- Connection Manager (Manager API)
-- ============================================================

local ConnectionManager = {}
ConnectionManager.__index = ConnectionManager

--- Create a connection manager.
function ConnectionManager.new()
    local self = setmetatable({}, ConnectionManager)
    self.connections = {}
    self.next_id = 1
    self.max_connections = 10000
    return self
end

--- Create a new connection via the manager.
--- @param opts table { protocol, addr, socket, kcp_ctx, user_data }
--- @return table  Connection object
function ConnectionManager:create(opts)
    if self:count() >= self.max_connections then
        return nil, "max connections reached"
    end

    local id = self.next_id
    self.next_id = self.next_id + 1

    opts = opts or {}
    opts.id = id

    -- Convert numeric protocol to string
    local proto_str = num_to_proto[opts.protocol] or M.PROTO_TCP
    local addr_str = "unknown"
    if opts.addr then
        addr_str = string.format("%s:%d", opts.addr.host or "0.0.0.0", opts.addr.port or 0)
    end

    local conn = M.new(opts.socket, addr_str, proto_str, {
        id        = id,
        player_id = opts.player_id,
        user_data = opts.user_data,
    })

    -- Store manager-specific fields
    conn._protocol_num = opts.protocol or ProtocolType.TCP
    conn.kcp_ctx = opts.kcp_ctx

    self.connections[id] = conn
    return conn
end

--- Get connection by ID.
function ConnectionManager:get(id)
    return self.connections[id]
end

--- Remove connection.
function ConnectionManager:remove(id)
    local conn = self.connections[id]
    if conn then
        M.close(conn)
        self.connections[id] = nil
    end
end

--- Get all connections.
function ConnectionManager:get_all()
    local result = {}
    for id, conn in pairs(self.connections) do
        result[#result + 1] = conn
    end
    return result
end

--- Filter by protocol type.
function ConnectionManager:get_by_protocol(protocol)
    local result = {}
    for id, conn in pairs(self.connections) do
        if conn._protocol_num == protocol then
            result[#result + 1] = conn
        end
    end
    return result
end

--- Get all KCP connections.
function ConnectionManager:get_kcp_connections()
    return self:get_by_protocol(ProtocolType.KCP)
end

--- Get all TCP connections.
function ConnectionManager:get_tcp_connections()
    return self:get_by_protocol(ProtocolType.TCP)
end

--- Get all WebSocket connections.
function ConnectionManager:get_ws_connections()
    return self:get_by_protocol(ProtocolType.WEBSOCKET)
end

--- Get connection count.
function ConnectionManager:count()
    local n = 0
    for _ in pairs(self.connections) do
        n = n + 1
    end
    return n
end

--- Get statistics.
function ConnectionManager:get_stats()
    local stats = {
        total     = 0,
        tcp       = 0,
        udp       = 0,
        kcp       = 0,
        websocket = 0,
        connected = 0,
    }

    for id, conn in pairs(self.connections) do
        stats.total = stats.total + 1
        if conn._protocol_num == ProtocolType.TCP then
            stats.tcp = stats.tcp + 1
        elseif conn._protocol_num == ProtocolType.UDP then
            stats.udp = stats.udp + 1
        elseif conn._protocol_num == ProtocolType.KCP then
            stats.kcp = stats.kcp + 1
        elseif conn._protocol_num == ProtocolType.WEBSOCKET then
            stats.websocket = stats.websocket + 1
        end
        if M.is_active(conn) then
            stats.connected = stats.connected + 1
        end
    end

    return stats
end

--- Cleanup disconnected connections.
function ConnectionManager:cleanup()
    local removed = 0
    for id, conn in pairs(self.connections) do
        if M.is_closed(conn) then
            self.connections[id] = nil
            removed = removed + 1
        end
    end
    return removed
end

-- ============================================================
-- Export
-- ============================================================

-- Export protocol/state tables for Manager API compatibility
M.ProtocolType      = ProtocolType
M.ConnectionState   = ConnectionState
M.Connection        = M  -- Self-reference for compatibility
M.ConnectionManager = ConnectionManager

return M
