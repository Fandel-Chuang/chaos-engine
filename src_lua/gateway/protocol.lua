--[[
    protocol.lua — Gateway Binary Message Protocol

    Implements the Gateway wire protocol used between
    clients and the Gateway process.

    Message format:
        [2B msg_type][4B body_len][N body]
        All integers are big-endian.

    msg_type values:
        0x0001 — MSG_PING        (heartbeat request)
        0x0002 — MSG_PONG        (heartbeat response)
        0x0010 — MSG_LOGIN       (login request)
        0x0011 — MSG_LOGIN_RESP  (login response)
        0x0100 — MSG_GAME_DATA   (game data, routed to backend)
        0xFFFF — MSG_DISCONNECT  (graceful disconnect)
]]

local M = {}

-- ============================================================
-- Constants
-- ============================================================

-- Header sizes
M.HEADER_SIZE = 6   -- 2B type + 4B len

-- Message types
M.MSG_PING         = 0x0001
M.MSG_PONG         = 0x0002
M.MSG_LOGIN        = 0x0010
M.MSG_LOGIN_RESP   = 0x0011
M.MSG_GAME_DATA    = 0x0100
M.MSG_DISCONNECT   = 0xFFFF

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
    return string_char((val >> 8) & 0xFF, val & 0xFF)
end

--- Pack uint32 big-endian → 4-byte string
local function pack_u32(val)
    return string_char(
        (val >> 24) & 0xFF,
        (val >> 16) & 0xFF,
        (val >> 8)  & 0xFF,
        val         & 0xFF
    )
end

--- Unpack uint16 big-endian from string at position pos
local function unpack_u16(data, pos)
    local b1, b2 = string_byte(data, pos, pos + 1)
    return (b1 << 8) | b2
end

--- Unpack uint32 big-endian from string at position pos
local function unpack_u32(data, pos)
    local b1, b2, b3, b4 = string_byte(data, pos, pos + 3)
    return (b1 << 24) | (b2 << 16) | (b3 << 8) | b4
end

-- ============================================================
-- Message Pack/Unpack
-- ============================================================

--- Pack a message into binary string.
--- Format: [2B msg_type][4B body_len][N body]
--- @param msg_type  number  Message type (MSG_PING, MSG_GAME_DATA, etc.)
--- @param body      string  Raw body data
--- @return string   Binary message
function M.pack(msg_type, body)
    body = body or ""
    local body_len = #body
    return pack_u16(msg_type) .. pack_u32(body_len) .. body
end

--- Unpack a message from binary string.
--- @param data   string  Raw binary message data
--- @return table  {msg_type, body} or nil, error
function M.unpack(data)
    if not data or #data < M.HEADER_SIZE then
        return nil, "message too short"
    end

    local msg_type = unpack_u16(data, 1)
    local body_len = unpack_u32(data, 3)

    if M.HEADER_SIZE + body_len > #data then
        return nil, string.format(
            "message incomplete: expected %d bytes, got %d",
            M.HEADER_SIZE + body_len, #data
        )
    end

    local body = ""
    if body_len > 0 then
        body = string_sub(data, M.HEADER_SIZE + 1, M.HEADER_SIZE + body_len)
    end

    return {
        msg_type = msg_type,
        body     = body,
        _raw_len = M.HEADER_SIZE + body_len,
    }
end

--- Peek the total message length from the header without unpacking.
--- Returns nil if header is incomplete.
--- @param data   string  Raw binary data buffer
--- @return number|nil  Total message length (header + body)
function M.peek_len(data)
    if not data or #data < M.HEADER_SIZE then
        return nil
    end
    local body_len = unpack_u32(data, 3)
    return M.HEADER_SIZE + body_len
end

--- Peek the message type from the header without unpacking.
--- @param data   string  Raw binary data buffer
--- @return number|nil  Message type
function M.peek_type(data)
    if not data or #data < M.HEADER_SIZE then
        return nil
    end
    return unpack_u16(data, 1)
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

-- ============================================================
-- Utility
-- ============================================================

--- Get current time in seconds (with fractional part).
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

return M
