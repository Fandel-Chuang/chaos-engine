--[[
    protocol.lua — Router Binary Message Protocol

    Implements the Router wire protocol used between:
    - Game ↔ Router (port 9100)
    - Router ↔ Router (port 9101, cluster sync)

    Message format (from ce_net_base.h):
        [4B total_len][2B msg_type][N payload]
        total_len = 6 + payload_len（大端序，包含头部的 6 字节）

    msg_type values:
        -- System messages (0x0000-0x0FFF)
        0x0001 — MSG_PING          (heartbeat request)
        0x0002 — MSG_PONG          (heartbeat response)
        0x0010 — MSG_LOGIN         (login request)
        0x0011 — MSG_LOGIN_RESP    (login response)
        0x0100 — MSG_GAME_DATA     (game data, routed between Game processes)
        0xFFFF — MSG_DISCONNECT    (graceful disconnect)

        -- Cross-region messages (0x1000-0x1FFF)
        0x1000 — MSG_CROSS_REGION  (cross-region forward)
        0x1001 — MSG_REGION_SYNC   (region sync)
        0x1002 — MSG_ROUTER_HELLO  (Router discovery)
        0x1003 — MSG_ROUTER_BYE    (Router departure)

        -- Router cluster messages (0x2000-0x2FFF)
        0x2000 — MSG_CLUSTER_SYNC  (cluster state sync)
        0x2001 — MSG_CLUSTER_JOIN  (node join cluster)
        0x2002 — MSG_CLUSTER_LEAVE (node leave cluster)
        0x2003 — MSG_CLUSTER_ELECT (leader election)

        -- Service registry messages (0x3000-0x3FFF)
        0x3000 — MSG_SERVICE_REGISTER   (Game registers with Router)
        0x3001 — MSG_SERVICE_UNREGISTER (Game unregisters)
        0x3002 — MSG_SERVICE_QUERY      (query service location)
        0x3003 — MSG_SERVICE_QUERY_RESP (query response)
        0x3004 — MSG_SERVICE_HEARTBEAT  (service heartbeat)

        -- User-defined messages from 0x8000
        0x8000 — MSG_USER_BASE
]]

local M = {}

-- ============================================================
-- Constants
-- ============================================================

-- Header sizes
M.HEADER_SIZE = 6   -- 4B total_len + 2B msg_type

-- Message types — System (0x0000-0x0FFF)
M.MSG_PING         = 0x0001
M.MSG_PONG         = 0x0002
M.MSG_LOGIN        = 0x0010
M.MSG_LOGIN_RESP   = 0x0011
M.MSG_GAME_DATA    = 0x0100
M.MSG_DISCONNECT   = 0xFFFF

-- Message types — Cross-region (0x1000-0x1FFF)
M.MSG_CROSS_REGION = 0x1000
M.MSG_REGION_SYNC  = 0x1001
M.MSG_ROUTER_HELLO = 0x1002
M.MSG_ROUTER_BYE   = 0x1003

-- Message types — Router cluster (0x2000-0x2FFF)
M.MSG_CLUSTER_SYNC  = 0x2000
M.MSG_CLUSTER_JOIN  = 0x2001
M.MSG_CLUSTER_LEAVE = 0x2002
M.MSG_CLUSTER_ELECT = 0x2003

-- Message types — Service registry (0x3000-0x3FFF)
M.MSG_SERVICE_REGISTER    = 0x3000
M.MSG_SERVICE_UNREGISTER  = 0x3001
M.MSG_SERVICE_QUERY       = 0x3002
M.MSG_SERVICE_QUERY_RESP  = 0x3003
M.MSG_SERVICE_HEARTBEAT   = 0x3004

-- Message types — User-defined
M.MSG_USER_BASE    = 0x8000

-- Max message size
M.MAX_MSG_SIZE = 256 * 1024  -- 256 KiB

-- ============================================================
-- Internal: Big-Endian Integer I/O
-- ============================================================

local string_char  = string.char
local string_byte  = string.byte
local string_sub   = string.sub
local table_concat = table.concat

--- Pack uint16 big-endian → 2-byte string
local function pack_u16(val)
    return string_char(math.floor(val / 256) % 256, val % 256)
end

--- Pack uint32 big-endian → 4-byte string
local function pack_u32(val)
    return string_char(
        math.floor(val / 16777216) % 256,
        math.floor(val / 65536) % 256,
        math.floor(val / 256) % 256,
        val % 256
    )
end

--- Unpack uint16 big-endian from string at position pos
local function unpack_u16(data, pos)
    local b1, b2 = string_byte(data, pos, pos + 1)
    return (b1 * 256) + b2
end

--- Unpack uint32 big-endian from string at position pos
local function unpack_u32(data, pos)
    local b1, b2, b3, b4 = string_byte(data, pos, pos + 3)
    return (b1 * 16777216) + (b2 * 65536) + (b3 * 256) + b4
end

-- ============================================================
-- Message Pack/Unpack
-- ============================================================

--- Pack a message into binary string.
--- Format: [4B total_len][2B msg_type][N body]
--- @param msg_type  number  Message type
--- @param body      string  Raw body data
--- @return string   Binary message
function M.pack(msg_type, body)
    body = body or ""
    local body_len = #body
    local total_len = M.HEADER_SIZE + body_len
    return pack_u32(total_len) .. pack_u16(msg_type) .. body
end

--- Unpack a message from binary string.
--- @param data   string  Raw binary message data
--- @return table  {msg_type, body, _raw_len} or nil, error
function M.unpack(data)
    if not data or #data < M.HEADER_SIZE then
        return nil, "message too short"
    end

    local total_len = unpack_u32(data, 1)
    local msg_type  = unpack_u16(data, 5)

    if total_len > #data then
        return nil, string.format(
            "message incomplete: expected %d bytes, got %d",
            total_len, #data
        )
    end

    if total_len < M.HEADER_SIZE then
        return nil, "invalid total_len in header"
    end

    local body_len = total_len - M.HEADER_SIZE
    local body = ""
    if body_len > 0 then
        body = string_sub(data, M.HEADER_SIZE + 1, total_len)
    end

    return {
        msg_type = msg_type,
        body     = body,
        _raw_len = total_len,
    }
end

--- Peek the total message length from the header without unpacking.
--- @param data   string  Raw binary data buffer
--- @return number|nil  Total message length (header + body)
function M.peek_len(data)
    if not data or #data < M.HEADER_SIZE then
        return nil
    end
    return unpack_u32(data, 1)
end

--- Peek the message type from the header without unpacking.
--- @param data   string  Raw binary data buffer
--- @return number|nil  Message type
function M.peek_type(data)
    if not data or #data < M.HEADER_SIZE then
        return nil
    end
    return unpack_u16(data, 5)
end

-- ============================================================
-- Convenience: Heartbeat Messages
-- ============================================================

--- Create a PING heartbeat message.
--- @return string  Binary PING message
function M.make_ping()
    return M.pack(M.MSG_PING, "")
end

--- Create a PONG heartbeat response.
--- @return string  Binary PONG message
function M.make_pong()
    return M.pack(M.MSG_PONG, "")
end

--- Check if a message type is a heartbeat type.
--- @param msg_type  number
--- @return boolean
function M.is_heartbeat(msg_type)
    return msg_type == M.MSG_PING or msg_type == M.MSG_PONG
end

--- Check if a message type is a service registry type.
--- @param msg_type  number
--- @return boolean
function M.is_service_msg(msg_type)
    return msg_type >= 0x3000 and msg_type <= 0x3FFF
end

--- Check if a message type is a cluster message.
--- @param msg_type  number
--- @return boolean
function M.is_cluster_msg(msg_type)
    return msg_type >= 0x2000 and msg_type <= 0x2FFF
end

--- Check if a message type is a cross-region message.
--- @param msg_type  number
--- @return boolean
function M.is_cross_region_msg(msg_type)
    return msg_type >= 0x1000 and msg_type <= 0x1FFF
end

-- ============================================================
-- Convenience: Service Messages
-- ============================================================

--- Create a service register message.
--- @param service_id   string  Service identifier
--- @param service_type string  Service type (e.g., "game", "dbproxy")
--- @param host         string  Service host
--- @param port         number  Service port
--- @param metadata     string  Optional JSON metadata
--- @return string  Binary message
function M.make_service_register(service_id, service_type, host, port, metadata)
    metadata = metadata or "{}"
    local body = string.format("%s|%s|%s|%d|%s",
        service_id, service_type, host, port, metadata)
    return M.pack(M.MSG_SERVICE_REGISTER, body)
end

--- Create a service query message.
--- @param service_type string  Service type to query
--- @return string  Binary message
function M.make_service_query(service_type)
    return M.pack(M.MSG_SERVICE_QUERY, service_type or "")
end

--- Create a service heartbeat message.
--- @param service_id string  Service identifier
--- @return string  Binary message
function M.make_service_heartbeat(service_id)
    return M.pack(M.MSG_SERVICE_HEARTBEAT, service_id)
end

-- ============================================================
-- Convenience: Cluster Messages
-- ============================================================

--- Create a cluster join message.
--- @param node_id   string  Node identifier
--- @param host      string  Node host
--- @param port      number  Node port
--- @param region    string  Region name
--- @return string  Binary message
function M.make_cluster_join(node_id, host, port, region)
    local body = string.format("%s|%s|%d|%s", node_id, host, port, region or "")
    return M.pack(M.MSG_CLUSTER_JOIN, body)
end

--- Create a cluster sync message.
--- @param payload string  Sync payload
--- @return string  Binary message
function M.make_cluster_sync(payload)
    return M.pack(M.MSG_CLUSTER_SYNC, payload or "")
end

-- ============================================================
-- Utility
-- ============================================================

--- Get current time in seconds (with fractional part via socket).
--- @return number
function M.now()
    return os.time()
end

--- Get current time in milliseconds.
--- @return number
function M.now_ms()
    return math.floor(os.time() * 1000)
end

-- Export internal unpack helpers for use by other modules
M.unpack_u32 = unpack_u32
M.unpack_u16 = unpack_u16

-- Export type-name lookup
M.type_names = {
    [M.MSG_PING]              = "PING",
    [M.MSG_PONG]              = "PONG",
    [M.MSG_LOGIN]             = "LOGIN",
    [M.MSG_LOGIN_RESP]        = "LOGIN_RESP",
    [M.MSG_GAME_DATA]         = "GAME_DATA",
    [M.MSG_DISCONNECT]        = "DISCONNECT",
    [M.MSG_CROSS_REGION]      = "CROSS_REGION",
    [M.MSG_REGION_SYNC]       = "REGION_SYNC",
    [M.MSG_ROUTER_HELLO]      = "ROUTER_HELLO",
    [M.MSG_ROUTER_BYE]        = "ROUTER_BYE",
    [M.MSG_CLUSTER_SYNC]      = "CLUSTER_SYNC",
    [M.MSG_CLUSTER_JOIN]      = "CLUSTER_JOIN",
    [M.MSG_CLUSTER_LEAVE]     = "CLUSTER_LEAVE",
    [M.MSG_CLUSTER_ELECT]     = "CLUSTER_ELECT",
    [M.MSG_SERVICE_REGISTER]  = "SERVICE_REGISTER",
    [M.MSG_SERVICE_UNREGISTER]= "SERVICE_UNREGISTER",
    [M.MSG_SERVICE_QUERY]     = "SERVICE_QUERY",
    [M.MSG_SERVICE_QUERY_RESP]= "SERVICE_QUERY_RESP",
    [M.MSG_SERVICE_HEARTBEAT] = "SERVICE_HEARTBEAT",
}

--- Get human-readable name for a message type.
--- @param msg_type number
--- @return string
function M.type_name(msg_type)
    return M.type_names[msg_type] or string.format("UNKNOWN(0x%04X)", msg_type)
end

return M
