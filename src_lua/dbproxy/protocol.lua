--[[
    protocol.lua — Binary Protocol Serialization/Deserialization

    Implements the ChaosEngine binary wire protocol used between
    Game processes and DBProxy. Format matches ce_sync.c / ce_dbproxy.

    Frame format (sync port 9001):
        [4B frame_len][2B frame_seq][8B timestamp][2B entity_count][N entities...]
        Each entity: [8B entity_id][2B component_type][4B data_len][N data]
        All integers are big-endian.

    Message format (db port 9003):
        [4B msg_len][1B msg_type][N payload]
        All integers are big-endian.

    Heartbeat frame: frame_seq = 0xFFFF
]]

local M = {}

-- ============================================================
-- Constants
-- ============================================================

-- Frame header sizes
M.FRAME_HEADER_SIZE   = 16   -- 4 + 2 + 8 + 2
M.ENTITY_HEADER_SIZE  = 14   -- 8 + 2 + 4
M.MAX_FRAME_SIZE      = 64 * 1024  -- 64 KiB

-- Heartbeat special sequence
M.HEARTBEAT_SEQ       = 0xFFFF

-- Message types (db port)
M.MSG_SAVE_PLAYER     = 0x01
M.MSG_LOAD_PLAYER     = 0x02
M.MSG_SAVE_WORLD      = 0x03
M.MSG_HEARTBEAT       = 0x04

-- Response types
M.RESP_OK             = 0x00
M.RESP_ERROR          = 0x01
M.RESP_DATA           = 0x02

-- ============================================================
-- Internal: Big-Endian Integer I/O
-- ============================================================

local string_char   = string.char
local string_byte   = string.byte
local string_sub    = string.sub
local table_concat  = table.concat

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

--- Pack uint64 big-endian → 8-byte string
local function pack_u64(val)
    -- Lua 5.3+ has native 64-bit integers; handle both integer and float
    local v = math.floor(val)
    return string_char(
        (v >> 56) & 0xFF,
        (v >> 48) & 0xFF,
        (v >> 40) & 0xFF,
        (v >> 32) & 0xFF,
        (v >> 24) & 0xFF,
        (v >> 16) & 0xFF,
        (v >> 8)  & 0xFF,
        v         & 0xFF
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

--- Unpack uint64 big-endian from string at position pos
local function unpack_u64(data, pos)
    local b1, b2, b3, b4, b5, b6, b7, b8 = string_byte(data, pos, pos + 7)
    return (b1 << 56) | (b2 << 48) | (b3 << 40) | (b4 << 32)
         | (b5 << 24) | (b6 << 16) | (b7 << 8)  | b8
end

-- ============================================================
-- Frame Pack/Unpack (Sync Protocol)
-- ============================================================

--- Pack a sync frame into binary string.
--- @param frame_seq   number  Frame sequence number (0xFFFF = heartbeat)
--- @param timestamp   number  Timestamp in microseconds
--- @param entities    table   Array of {entity_id, component_type, data}
--- @return string     Binary frame data
function M.pack_frame(frame_seq, timestamp, entities)
    local entity_count = entities and #entities or 0
    local parts = {}

    -- Calculate entity data size
    local entity_data_size = 0
    local entity_parts = {}
    if entity_count > 0 then
        for i = 1, entity_count do
            local ent = entities[i]
            local data = ent.data or ""
            local data_len = #data
            local part = pack_u64(ent.entity_id)
                       .. pack_u16(ent.component_type)
                       .. pack_u32(data_len)
                       .. data
            entity_parts[i] = part
            entity_data_size = entity_data_size + M.ENTITY_HEADER_SIZE + data_len
        end
    end

    local frame_len = M.FRAME_HEADER_SIZE + entity_data_size

    -- Assemble frame
    parts[1] = pack_u32(frame_len)
    parts[2] = pack_u16(frame_seq)
    parts[3] = pack_u64(timestamp)
    parts[4] = pack_u16(entity_count)

    local idx = 5
    for i = 1, entity_count do
        parts[idx] = entity_parts[i]
        idx = idx + 1
    end

    return table_concat(parts)
end

--- Unpack a binary sync frame.
--- @param data   string  Raw binary frame data
--- @return table  {frame_seq, timestamp, entities}
---   entities = {{entity_id, component_type, data}, ...}
function M.unpack_frame(data)
    if not data or #data < M.FRAME_HEADER_SIZE then
        return nil, "frame too short"
    end

    local frame_len = unpack_u32(data, 1)
    if frame_len > #data then
        return nil, "frame incomplete: expected " .. frame_len .. " bytes, got " .. #data
    end

    local pos = 5  -- after 4-byte frame_len

    local frame_seq    = unpack_u16(data, pos);  pos = pos + 2
    local timestamp    = unpack_u64(data, pos);  pos = pos + 8
    local entity_count = unpack_u16(data, pos);  pos = pos + 2

    local entities = {}
    for i = 1, entity_count do
        if pos + M.ENTITY_HEADER_SIZE - 1 > #data then
            return nil, "frame truncated at entity " .. i
        end

        local entity_id      = unpack_u64(data, pos);  pos = pos + 8
        local component_type = unpack_u16(data, pos);  pos = pos + 2
        local data_len       = unpack_u32(data, pos);  pos = pos + 4

        local entity_data = ""
        if data_len > 0 then
            if pos + data_len - 1 > #data then
                return nil, "entity " .. i .. " data exceeds frame boundary"
            end
            entity_data = string_sub(data, pos, pos + data_len - 1)
            pos = pos + data_len
        end

        entities[i] = {
            entity_id      = entity_id,
            component_type = component_type,
            data           = entity_data,
        }
    end

    return {
        frame_seq    = frame_seq,
        timestamp    = timestamp,
        entities     = entities,
        _raw_len     = frame_len,
    }
end

-- ============================================================
-- Message Pack/Unpack (DB Protocol)
-- ============================================================

--- Pack a DB message into binary string.
--- Format: [4B msg_len][1B msg_type][N payload]
--- @param msg_type  number  Message type (MSG_SAVE_PLAYER, etc.)
--- @param payload   string  Raw payload data
--- @return string   Binary message
function M.pack_message(msg_type, payload)
    payload = payload or ""
    local msg_len = 5 + #payload  -- 4B len + 1B type + payload
    return pack_u32(msg_len) .. string_char(msg_type) .. payload
end

--- Unpack a DB message from binary string.
--- @param data   string  Raw binary message data
--- @return table  {msg_type, payload}
function M.unpack_message(data)
    if not data or #data < 5 then
        return nil, "message too short"
    end

    local msg_len = unpack_u32(data, 1)
    if msg_len > #data then
        return nil, "message incomplete: expected " .. msg_len .. " bytes, got " .. #data
    end

    local msg_type = string_byte(data, 5)
    local payload = ""
    if msg_len > 5 then
        payload = string_sub(data, 6, msg_len)
    end

    return {
        msg_type = msg_type,
        payload  = payload,
        _raw_len = msg_len,
    }
end

--- Pack a DB response message.
--- @param resp_type  number  Response type (RESP_OK, RESP_ERROR, RESP_DATA)
--- @param payload    string  Optional payload
--- @return string    Binary response
function M.pack_response(resp_type, payload)
    return M.pack_message(resp_type, payload)
end

-- ============================================================
-- Utility
-- ============================================================

--- Check if a frame is a heartbeat frame.
function M.is_heartbeat(frame_seq)
    return frame_seq == M.HEARTBEAT_SEQ
end

--- Create a heartbeat frame.
--- @param timestamp  number  Current timestamp in microseconds
--- @return string    Binary heartbeat frame
function M.make_heartbeat_frame(timestamp)
    return M.pack_frame(M.HEARTBEAT_SEQ, timestamp, {})
end

--- Create a heartbeat message (for db port).
--- @return string  Binary heartbeat message
function M.make_heartbeat_message()
    return M.pack_message(M.MSG_HEARTBEAT, "")
end

--- Get current time in microseconds.
function M.now_us()
    return os.time() * 1000000
end

-- Export internal unpack helpers for use by init.lua
M.unpack_u32 = unpack_u32
M.unpack_u16 = unpack_u16

return M
