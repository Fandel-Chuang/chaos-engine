--[[
    websocket.lua — WebSocket Protocol Implementation (RFC 6455)

    Provides:
    - WebSocket handshake (HTTP Upgrade → 101 Switching Protocols)
    - Frame encode/decode (text, binary, close, ping, pong)
    - Mask/unmask handling for client-to-server frames
    - ws_send(conn, data, opcode) / ws_recv(conn) → {opcode, data}

    Opcodes:
        0x1  Text frame
        0x2  Binary frame
        0x8  Close frame
        0x9  Ping frame
        0xA  Pong frame

    Frame format:
        0                   1                   2                   3
        0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
       +-+-+-+-+-------+-+-------------+-------------------------------+
       |F|R|R|R| opcode|M| Payload len |    Extended payload length    |
       |I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
       |N|V|V|V|       |S|             |   (if payload len==126/127)   |
       | |1|2|3|       |K|             |                               |
       +-+-+-+-+-------+-+-------------+ - - - - - - - - - - - - - - - +
       |     Extended payload length continued, if payload len == 127  |
       + - - - - - - - - - - - - - - - +-------------------------------+
       |                               |Masking-key, if MASK set to 1  |
       +-------------------------------+-------------------------------+
       | Masking-key (continued)       |          Payload Data         |
       +-------------------------------- - - - - - - - - - - - - - - - +
       :                     Payload Data continued ...                :
       + - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - +
       |                     Payload Data continued ...                |
       +---------------------------------------------------------------+
]]

local M = {}

-- ============================================================
-- Constants
-- ============================================================

-- Opcodes
M.OP_TEXT   = 0x1
M.OP_BINARY = 0x2
M.OP_CLOSE  = 0x8
M.OP_PING   = 0x9
M.OP_PONG   = 0xA

-- Frame header flags
M.FIN_BIT   = 0x80

-- WebSocket GUID for handshake (RFC 6455 Section 4.2.2)
local WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

-- Maximum frame payload size (64 KiB, matching DBProxy limit)
M.MAX_PAYLOAD_SIZE = 64 * 1024

-- ============================================================
-- Internal: String helpers
-- ============================================================

local string_char  = string.char
local string_byte  = string.byte
local string_sub   = string.sub
local string_find  = string.find
local string_gsub  = string.gsub
local string_format = string.format
local table_concat = table.concat

-- ============================================================
-- SHA1 Implementation (Pure Lua)
-- ============================================================

-- SHA1 is needed for WebSocket handshake: SHA1(key + GUID) → base64
-- This is a minimal pure-Lua SHA1 implementation.

local function sha1_rotl(n, b)
    return ((n << b) | (n >> (32 - b))) & 0xFFFFFFFF
end

local function sha1_bytes_to_uint32_be(s, pos)
    local b1, b2, b3, b4 = string_byte(s, pos, pos + 3)
    return (b1 << 24) | (b2 << 16) | (b3 << 8) | b4
end

local function sha1_uint32_to_bytes_be(n)
    return string_char(
        (n >> 24) & 0xFF,
        (n >> 16) & 0xFF,
        (n >> 8)  & 0xFF,
        n         & 0xFF
    )
end

--- Compute SHA1 hash of a string. Returns 20-byte binary string.
local function sha1(data)
    -- Initialize variables
    local h0 = 0x67452301
    local h1 = 0xEFCDAB89
    local h2 = 0x98BADCFE
    local h3 = 0x10325476
    local h4 = 0xC3D2E1F0

    -- Pre-processing: padding
    local data_len = #data
    local bit_len = data_len * 8

    -- Append 0x80
    data = data .. "\128"

    -- Append 0x00 until message length ≡ 56 (mod 64)
    while (#data % 64) ~= 56 do
        data = data .. "\0"
    end

    -- Append original length in bits as 64-bit big-endian
    data = data .. string_char(
        (bit_len >> 56) & 0xFF,
        (bit_len >> 48) & 0xFF,
        (bit_len >> 40) & 0xFF,
        (bit_len >> 32) & 0xFF,
        (bit_len >> 24) & 0xFF,
        (bit_len >> 16) & 0xFF,
        (bit_len >> 8)  & 0xFF,
        bit_len         & 0xFF
    )

    -- Process each 512-bit (64-byte) chunk
    local chunks = #data // 64
    for c = 0, chunks - 1 do
        local offset = c * 64 + 1
        local w = {}
        for i = 0, 15 do
            w[i] = sha1_bytes_to_uint32_be(data, offset + i * 4)
        end

        for i = 16, 79 do
            w[i] = sha1_rotl(w[i-3] ~ w[i-8] ~ w[i-14] ~ w[i-16], 1)
        end

        local a = h0
        local b = h1
        local c = h2
        local d = h3
        local e = h4

        for i = 0, 79 do
            local f, k
            if i <= 19 then
                f = (b & c) | (~b & d)
                k = 0x5A827999
            elseif i <= 39 then
                f = b ~ c ~ d
                k = 0x6ED9EBA1
            elseif i <= 59 then
                f = (b & c) | (b & d) | (c & d)
                k = 0x8F1BBCDC
            else
                f = b ~ c ~ d
                k = 0xCA62C1D6
            end

            local temp = (sha1_rotl(a, 5) + f + e + k + w[i]) & 0xFFFFFFFF
            e = d
            d = c
            c = sha1_rotl(b, 30)
            b = a
            a = temp
        end

        h0 = (h0 + a) & 0xFFFFFFFF
        h1 = (h1 + b) & 0xFFFFFFFF
        h2 = (h2 + c) & 0xFFFFFFFF
        h3 = (h3 + d) & 0xFFFFFFFF
        h4 = (h4 + e) & 0xFFFFFFFF
    end

    return sha1_uint32_to_bytes_be(h0)
         .. sha1_uint32_to_bytes_be(h1)
         .. sha1_uint32_to_bytes_be(h2)
         .. sha1_uint32_to_bytes_be(h3)
         .. sha1_uint32_to_bytes_be(h4)
end

-- ============================================================
-- Base64 Encoding
-- ============================================================

local b64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

--- Encode binary string to base64.
local function base64_encode(data)
    local result = {}
    local len = #data
    local i = 1

    while i <= len do
        local a = string_byte(data, i)
        local b = i + 1 <= len and string_byte(data, i + 1) or 0
        local c = i + 2 <= len and string_byte(data, i + 2) or 0

        local n = (a << 16) | (b << 8) | c
        result[#result + 1] = string_sub(b64_chars, ((n >> 18) & 0x3F) + 1, ((n >> 18) & 0x3F) + 1)
        result[#result + 1] = string_sub(b64_chars, ((n >> 12) & 0x3F) + 1, ((n >> 12) & 0x3F) + 1)

        if i + 1 <= len then
            result[#result + 1] = string_sub(b64_chars, ((n >> 6) & 0x3F) + 1, ((n >> 6) & 0x3F) + 1)
        else
            result[#result + 1] = "="
        end

        if i + 2 <= len then
            result[#result + 1] = string_sub(b64_chars, (n & 0x3F) + 1, (n & 0x3F) + 1)
        else
            result[#result + 1] = "="
        end

        i = i + 3
    end

    return table_concat(result)
end

-- ============================================================
-- WebSocket Handshake
-- ============================================================

--- Parse HTTP Upgrade request and extract headers.
--- @param request  string  Raw HTTP request
--- @return table|nil  Parsed headers, or nil on failure
local function parse_http_upgrade(request)
    local headers = {}
    local lines = {}
    for line in request:gmatch("[^\r\n]+") do
        lines[#lines + 1] = line
    end

    if #lines == 0 then return nil end

    -- Parse request line: GET /path HTTP/1.1
    local method, path, version = lines[1]:match("^(%S+)%s+(%S+)%s+(%S+)$")
    if not method then return nil end
    headers.method  = method
    headers.path    = path
    headers.version = version

    -- Parse header fields
    for i = 2, #lines do
        local key, value = lines[i]:match("^([%w%-]+):%s*(.+)$")
        if key then
            headers[key:lower()] = value
        end
    end

    return headers
end

--- Perform WebSocket handshake: validate Upgrade request and generate 101 response.
--- @param request  string  Raw HTTP upgrade request
--- @return string|nil  101 Switching Protocols response, or nil on failure
--- @return string|nil  Error message on failure
function M.handshake(request)
    local headers = parse_http_upgrade(request)
    if not headers then
        return nil, "failed to parse HTTP request"
    end

    -- Validate Upgrade header
    local upgrade = headers["upgrade"]
    if not upgrade or upgrade:lower() ~= "websocket" then
        return nil, "missing or invalid Upgrade header"
    end

    -- Validate Connection header
    local connection = headers["connection"] or ""
    if not connection:lower():match("upgrade") then
        return nil, "missing Upgrade in Connection header"
    end

    -- Validate WebSocket version
    local ws_version = headers["sec-websocket-version"]
    if not ws_version or ws_version ~= "13" then
        return nil, "unsupported WebSocket version: " .. (ws_version or "none")
    end

    -- Validate Sec-WebSocket-Key
    local ws_key = headers["sec-websocket-key"]
    if not ws_key then
        return nil, "missing Sec-WebSocket-Key header"
    end

    -- Compute accept key: base64(sha1(key + GUID))
    local accept_key = base64_encode(sha1(ws_key .. WS_GUID))

    -- Build 101 Switching Protocols response
    local response = string_format(
        "HTTP/1.1 101 Switching Protocols\r\n" ..
        "Upgrade: websocket\r\n" ..
        "Connection: Upgrade\r\n" ..
        "Sec-WebSocket-Accept: %s\r\n" ..
        "\r\n",
        accept_key
    )

    return response
end

--- Generate a WebSocket handshake request (client side, for testing).
--- @param host   string  Target host
--- @param port   number  Target port
--- @param path   string  Request path (default "/")
--- @return string  HTTP upgrade request
function M.client_handshake_request(host, port, path)
    path = path or "/"
    -- Generate a random 16-byte key, base64 encoded
    local key_bytes = {}
    for i = 1, 16 do
        key_bytes[i] = string_char(math.random(0, 255))
    end
    local ws_key = base64_encode(table_concat(key_bytes))

    return string_format(
        "GET %s HTTP/1.1\r\n" ..
        "Host: %s:%d\r\n" ..
        "Upgrade: websocket\r\n" ..
        "Connection: Upgrade\r\n" ..
        "Sec-WebSocket-Key: %s\r\n" ..
        "Sec-WebSocket-Version: 13\r\n" ..
        "\r\n",
        path, host, port, ws_key
    )
end

-- ============================================================
-- Frame Encoding (Server → Client, unmasked)
-- ============================================================

--- Encode a WebSocket frame (server-to-client, no mask).
--- @param data    string  Payload data
--- @param opcode  number  Frame opcode (default OP_TEXT)
--- @param fin     boolean  FIN bit (default true)
--- @return string  Encoded frame
function M.encode_frame(data, opcode, fin)
    opcode = opcode or M.OP_TEXT
    if fin == nil then fin = true end

    local first_byte = opcode
    if fin then
        first_byte = first_byte | M.FIN_BIT
    end

    local payload_len = #data
    local header = string_char(first_byte)

    if payload_len <= 125 then
        header = header .. string_char(payload_len)
    elseif payload_len <= 65535 then
        header = header .. string_char(126)
            .. string_char((payload_len >> 8) & 0xFF, payload_len & 0xFF)
    else
        -- 64-bit length (8 bytes)
        header = header .. string_char(127)
            .. string_char(
                (payload_len >> 56) & 0xFF,
                (payload_len >> 48) & 0xFF,
                (payload_len >> 40) & 0xFF,
                (payload_len >> 32) & 0xFF,
                (payload_len >> 24) & 0xFF,
                (payload_len >> 16) & 0xFF,
                (payload_len >> 8)  & 0xFF,
                payload_len         & 0xFF
            )
    end

    return header .. data
end

--- Encode a close frame.
--- @param code    number  Close status code (default 1000)
--- @param reason  string  Close reason (optional)
--- @return string  Encoded close frame
function M.encode_close_frame(code, reason)
    code = code or 1000
    reason = reason or ""
    local payload = string_char((code >> 8) & 0xFF, code & 0xFF) .. reason
    return M.encode_frame(payload, M.OP_CLOSE, true)
end

--- Encode a ping frame.
--- @param data  string  Optional ping payload
--- @return string  Encoded ping frame
function M.encode_ping_frame(data)
    return M.encode_frame(data or "", M.OP_PING, true)
end

--- Encode a pong frame.
--- @param data  string  Optional pong payload
--- @return string  Encoded pong frame
function M.encode_pong_frame(data)
    return M.encode_frame(data or "", M.OP_PONG, true)
end

-- ============================================================
-- Frame Decoding (Client → Server, masked)
-- ============================================================

--- Apply XOR mask to data using 4-byte masking key.
--- @param data        string  Masked data
--- @param mask_key    string  4-byte masking key
--- @return string  Unmasked data
local function apply_mask(data, mask_key)
    local result = {}
    for i = 1, #data do
        local b = string_byte(data, i)
        local m = string_byte(mask_key, ((i - 1) % 4) + 1)
        result[i] = string_char(b ~ m)
    end
    return table_concat(result)
end

--- Decode a WebSocket frame from raw bytes.
--- Returns nil, error if frame is incomplete or malformed.
--- @param data  string  Raw frame data (may contain trailing data)
--- @return table|nil  {fin, opcode, data, masked, close_code, close_reason, consumed}
--- @return string|nil  Error message
function M.decode_frame(data)
    if not data or #data < 2 then
        return nil, "frame too short (need at least 2 bytes)"
    end

    local pos = 1
    local b1 = string_byte(data, pos); pos = pos + 1
    local b2 = string_byte(data, pos); pos = pos + 1

    local fin    = (b1 & 0x80) ~= 0
    local opcode = b1 & 0x0F
    local masked = (b2 & 0x80) ~= 0
    local payload_len = b2 & 0x7F

    -- Extended payload length
    if payload_len == 126 then
        if #data < pos + 1 then
            return nil, "frame too short (need 2-byte extended length)"
        end
        payload_len = (string_byte(data, pos) << 8) | string_byte(data, pos + 1)
        pos = pos + 2
    elseif payload_len == 127 then
        if #data < pos + 7 then
            return nil, "frame too short (need 8-byte extended length)"
        end
        -- Read 64-bit big-endian; Lua 5.4 supports 64-bit integers
        local hi = (string_byte(data, pos)     << 24)
                 | (string_byte(data, pos + 1) << 16)
                 | (string_byte(data, pos + 2) << 8)
                 |  string_byte(data, pos + 3)
        local lo = (string_byte(data, pos + 4) << 24)
                 | (string_byte(data, pos + 5) << 16)
                 | (string_byte(data, pos + 6) << 8)
                 |  string_byte(data, pos + 7)
        payload_len = hi * 4294967296 + lo  -- hi << 32 + lo
        pos = pos + 8
    end

    -- Validate payload size
    if payload_len > M.MAX_PAYLOAD_SIZE then
        return nil, string_format("payload too large: %d > %d", payload_len, M.MAX_PAYLOAD_SIZE)
    end

    -- Masking key
    local mask_key = nil
    if masked then
        if #data < pos + 3 then
            return nil, "frame too short (need 4-byte mask key)"
        end
        mask_key = string_sub(data, pos, pos + 3)
        pos = pos + 4
    end

    -- Payload data
    if #data < pos + payload_len - 1 then
        return nil, string_format("frame incomplete: need %d bytes, have %d",
            payload_len, #data - pos + 1)
    end

    local payload = string_sub(data, pos, pos + payload_len - 1)
    local consumed = pos + payload_len - 1

    -- Unmask if needed
    if masked and mask_key then
        payload = apply_mask(payload, mask_key)
    end

    -- Parse close frame
    local close_code = nil
    local close_reason = nil
    if opcode == M.OP_CLOSE and #payload >= 2 then
        close_code = (string_byte(payload, 1) << 8) | string_byte(payload, 2)
        close_reason = #payload > 2 and string_sub(payload, 3) or nil
    end

    return {
        fin          = fin,
        opcode       = opcode,
        data         = payload,
        masked       = masked,
        close_code   = close_code,
        close_reason = close_reason,
        consumed     = consumed,
    }
end

-- ============================================================
-- High-Level Send/Receive
-- ============================================================

--- Send a WebSocket message over a connection.
--- @param conn    table   Connection object with :send(data) method
--- @param data    string  Message payload
--- @param opcode  number  Opcode (default OP_TEXT)
--- @return boolean  Success
--- @return string|nil  Error message
function M.ws_send(conn, data, opcode)
    opcode = opcode or M.OP_TEXT
    local frame = M.encode_frame(data, opcode, true)
    local ok, err = conn:send(frame)
    if not ok then
        return false, err
    end
    return true
end

--- Send a ping frame over a connection.
--- @param conn  table  Connection object
--- @param data  string  Optional ping payload
--- @return boolean
function M.ws_ping(conn, data)
    local frame = M.encode_ping_frame(data)
    local ok, err = conn:send(frame)
    return ok, err
end

--- Send a pong frame over a connection.
--- @param conn  table  Connection object
--- @param data  string  Optional pong payload
--- @return boolean
function M.ws_pong(conn, data)
    local frame = M.encode_pong_frame(data)
    local ok, err = conn:send(frame)
    return ok, err
end

--- Send a close frame over a connection.
--- @param conn    table  Connection object
--- @param code    number  Close status code
--- @param reason  string  Close reason
--- @return boolean
function M.ws_close(conn, code, reason)
    local frame = M.encode_close_frame(code, reason)
    local ok, err = conn:send(frame)
    return ok, err
end

--- Receive and decode a WebSocket frame from a connection's buffer.
--- Returns nil if no complete frame is available.
--- @param conn  table  Connection object with recv_buf field
--- @return table|nil  {opcode, data, fin, close_code, close_reason}
--- @return string|nil  Error message
function M.ws_recv(conn)
    local buf = conn.recv_buf or ""
    if #buf < 2 then
        return nil  -- Not enough data
    end

    local frame, err = M.decode_frame(buf)
    if not frame then
        if err and err:match("incomplete") then
            return nil  -- Wait for more data
        end
        return nil, err
    end

    -- Consume the frame from the buffer
    conn.recv_buf = string_sub(buf, frame.consumed + 1)

    return {
        opcode       = frame.opcode,
        data         = frame.data,
        fin          = frame.fin,
        close_code   = frame.close_code,
        close_reason = frame.close_reason,
    }
end

-- ============================================================
-- Utility
-- ============================================================

--- Get opcode name for logging.
--- @param opcode  number
--- @return string
function M.opcode_name(opcode)
    local names = {
        [0x0] = "CONTINUATION",
        [0x1] = "TEXT",
        [0x2] = "BINARY",
        [0x8] = "CLOSE",
        [0x9] = "PING",
        [0xA] = "PONG",
    }
    return names[opcode] or string_format("UNKNOWN(0x%X)", opcode)
end

return M
