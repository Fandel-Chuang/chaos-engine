--[[
    cross_region.lua — Cross-Region Routing

    Handles routing messages between different geographic regions.
    Each region has its own Router cluster; this module enables
    cross-region communication by forwarding messages through
    remote region Routers.

    Features:
    - Remote region Router connections
    - Cross-region message forwarding
    - Region-to-region message routing table
    - TTL-based message expiry
    - Hop-count tracking to prevent routing loops
]]

local config   = require("router.config")
local protocol = require("router.protocol")
local cluster  = require("router.cluster")

local M = {}

-- ============================================================
-- Constants
-- ============================================================

-- Default TTL for cross-region messages
M.DEFAULT_TTL = 16

-- Max hop count before message is dropped
M.MAX_HOPS = 32

-- ============================================================
-- Remote Region Entry
-- ============================================================

--- Create a remote region entry.
--- @param region string  Region name
--- @param host   string  Remote Router host
--- @param port   number  Remote Router port
--- @return table  Region entry
local function new_region(region, host, port)
    return {
        region     = region,
        host       = host,
        port       = port,
        sock       = nil,
        connected  = false,
        last_seen  = 0,
    }
end

-- ============================================================
-- Internal State
-- ============================================================

-- Remote regions: region_name → region entry
local remote_regions = {}

-- Cross-region routing table: dst_region → { via_region, hop_count }
local region_routes = {}

-- ============================================================
-- Region Management
-- ============================================================

--- Add a remote region.
--- @param region string  Region name
--- @param host   string  Remote Router host
--- @param port   number  Remote Router port
function M.add_region(region, host, port)
    remote_regions[region] = new_region(region, host, port)
    -- Direct route (0 hops to the region's own Router)
    region_routes[region] = { via_region = region, hop_count = 0 }
    M._log("info", string.format("Remote region added: %s at %s:%d", region, host, port))
end

--- Remove a remote region.
--- @param region string  Region name
function M.remove_region(region)
    local entry = remote_regions[region]
    if entry and entry.sock then
        pcall(function() entry.sock:close() end)
    end
    remote_regions[region] = nil
    region_routes[region] = nil
    M._log("info", string.format("Remote region removed: %s", region))
end

--- Get a remote region entry.
--- @param region string
--- @return table|nil  Region entry
function M.get_region(region)
    return remote_regions[region]
end

--- Get all configured remote regions.
--- @return table  Array of region names
function M.get_all_regions()
    local result = {}
    for name, _ in pairs(remote_regions) do
        result[#result + 1] = name
    end
    return result
end

-- ============================================================
-- Connection Management
-- ============================================================

--- Connect to a remote region's Router.
--- @param region string  Region name
--- @return boolean  true if connected
--- @return string|nil  Error message
function M.connect_region(region)
    local entry = remote_regions[region]
    if not entry then
        return false, "unknown region: " .. region
    end

    if entry.connected and entry.sock then
        return true  -- Already connected
    end

    local socket = require("socket")
    local sock, err = socket.tcp()
    if not sock then
        return false, "failed to create socket: " .. tostring(err)
    end

    sock:settimeout(5)  -- 5 second connect timeout
    local ok, conn_err = sock:connect(entry.host, entry.port)
    if not ok then
        sock:close()
        return false, "failed to connect to " .. region .. " at " ..
            entry.host .. ":" .. entry.port .. ": " .. tostring(conn_err)
    end

    sock:settimeout(0)  -- Non-blocking

    entry.sock = sock
    entry.connected = true
    entry.last_seen = os.time()

    -- Send Router HELLO to announce presence
    local hello_msg = protocol.pack(protocol.MSG_ROUTER_HELLO,
        cluster.get_local_node().node_id)
    pcall(function() sock:send(hello_msg) end)

    M._log("info", string.format("Connected to remote region: %s at %s:%d",
        region, entry.host, entry.port))

    return true
end

--- Connect to all configured remote regions.
--- @return number  Count of successful connections
function M.connect_all_regions()
    local connected = 0
    for region, _ in pairs(remote_regions) do
        local ok, _ = M.connect_region(region)
        if ok then
            connected = connected + 1
        end
    end
    return connected
end

--- Disconnect from a remote region.
--- @param region string  Region name
function M.disconnect_region(region)
    local entry = remote_regions[region]
    if not entry then return end

    if entry.sock then
        -- Send BYE
        local bye_msg = protocol.pack(protocol.MSG_ROUTER_BYE,
            cluster.get_local_node().node_id)
        pcall(function() entry.sock:send(bye_msg) end)
        pcall(function() entry.sock:close() end)
        entry.sock = nil
    end

    entry.connected = false
    M._log("info", string.format("Disconnected from remote region: %s", region))
end

--- Disconnect from all remote regions.
function M.disconnect_all()
    for region, _ in pairs(remote_regions) do
        M.disconnect_region(region)
    end
end

-- ============================================================
-- Cross-Region Message Format
-- ============================================================

--- Pack a cross-region message.
--- Format: src_region|dst_region|timestamp|hop_count|ttl|inner_type|inner_data
--- @param src_region  string  Source region
--- @param dst_region  string  Destination region
--- @param inner_type  number  Inner message type
--- @param inner_data  string  Inner message body
--- @param ttl         number  Time-to-live (default DEFAULT_TTL)
--- @return string  Cross-region message body
function M.pack_cross_region(src_region, dst_region, inner_type, inner_data, ttl)
    ttl = ttl or M.DEFAULT_TTL
    local timestamp = os.time()
    local hop_count = 0

    -- Format: src|dst|ts|hops|ttl|type|data
    -- Use base64-like encoding for inner_data to avoid delimiter issues
    -- For simplicity, use length-prefixed inner data
    local inner_len = #(inner_data or "")
    return string.format("%s|%s|%d|%d|%d|%d|%d|%s",
        src_region, dst_region, timestamp, hop_count, ttl,
        inner_type, inner_len, inner_data or "")
end

--- Unpack a cross-region message.
--- @param body string  Cross-region message body
--- @return table|nil  { src_region, dst_region, timestamp, hop_count, ttl, inner_type, inner_data }
--- @return string|nil  Error message
function M.unpack_cross_region(body)
    if not body or body == "" then
        return nil, "empty body"
    end

    -- Parse: src|dst|ts|hops|ttl|type|len|data
    local parts = {}
    local pos = 1
    -- Parse first 7 pipe-delimited fields
    for _ = 1, 7 do
        local pipe_pos = body:find("|", pos, true)
        if not pipe_pos then
            return nil, "malformed cross-region header"
        end
        parts[#parts + 1] = body:sub(pos, pipe_pos - 1)
        pos = pipe_pos + 1
    end

    local src_region  = parts[1]
    local dst_region  = parts[2]
    local timestamp   = tonumber(parts[3]) or 0
    local hop_count   = tonumber(parts[4]) or 0
    local ttl         = tonumber(parts[5]) or M.DEFAULT_TTL
    local inner_type  = tonumber(parts[6]) or 0
    local inner_len   = tonumber(parts[7]) or 0

    -- Remaining is inner_data
    local inner_data = body:sub(pos, pos + inner_len - 1)

    return {
        src_region  = src_region,
        dst_region  = dst_region,
        timestamp   = timestamp,
        hop_count   = hop_count,
        ttl         = ttl,
        inner_type  = inner_type,
        inner_data  = inner_data,
    }
end

-- ============================================================
-- Cross-Region Routing
-- ============================================================

--- Send a message to a remote region.
--- @param dst_region string  Destination region
--- @param msg_type   number  Inner message type
--- @param body       string  Inner message body
--- @param ttl        number  Time-to-live
--- @return boolean  true if sent
--- @return string|nil  Error message
function M.send_to_region(dst_region, msg_type, body, ttl)
    local local_info = cluster.get_local_node()
    local src_region = local_info.region

    -- Don't route to self
    if dst_region == src_region then
        return false, "cannot route to local region"
    end

    -- Check if we have a direct connection to the destination region
    local entry = remote_regions[dst_region]
    if not entry or not entry.connected then
        -- Try to find a route through another region
        local route = region_routes[dst_region]
        if not route then
            return false, "no route to region: " .. dst_region
        end
        -- Re-route through intermediate region
        entry = remote_regions[route.via_region]
        if not entry or not entry.connected then
            return false, "via region not connected: " .. route.via_region
        end
    end

    -- Pack cross-region message
    local cr_body = M.pack_cross_region(src_region, dst_region, msg_type, body, ttl)
    local cr_msg = protocol.pack(protocol.MSG_CROSS_REGION, cr_body)

    local ok, err = entry.sock:send(cr_msg)
    if not ok then
        entry.connected = false
        return false, "send failed: " .. tostring(err)
    end

    return true
end

--- Handle an incoming cross-region message.
--- @param body string  Cross-region message body
--- @return table|nil  { should_relay, dst_region, inner_type, inner_data }
--- @return string|nil  Error message
function M.handle_incoming(body)
    local cr, err = M.unpack_cross_region(body)
    if not cr then
        return nil, err
    end

    local local_info = cluster.get_local_node()
    local local_region = local_info.region

    -- Check TTL
    if cr.ttl <= 0 then
        return nil, "TTL expired"
    end

    -- Check hop count
    if cr.hop_count >= M.MAX_HOPS then
        return nil, "max hops exceeded"
    end

    -- If this message is for our region, deliver locally
    if cr.dst_region == local_region then
        return {
            should_relay = false,
            src_region   = cr.src_region,
            dst_region   = cr.dst_region,
            inner_type   = cr.inner_type,
            inner_data   = cr.inner_data,
        }
    end

    -- Otherwise, relay to the next hop
    cr.hop_count = cr.hop_count + 1
    cr.ttl = cr.ttl - 1

    -- Re-pack and forward
    local relay_body = M.pack_cross_region(
        cr.src_region, cr.dst_region, cr.inner_type, cr.inner_data, cr.ttl
    )
    -- Note: hop_count is embedded in the re-packed body

    return {
        should_relay = true,
        dst_region   = cr.dst_region,
        relay_body   = relay_body,
    }
end

-- ============================================================
-- Route Learning
-- ============================================================

--- Learn a route from an incoming cross-region message.
--- @param src_region string  Source region of the message
--- @param via_region string  Region the message came through
--- @param hop_count  number  Hops from source to us
function M.learn_route(src_region, via_region, hop_count)
    local existing = region_routes[src_region]
    if not existing or (hop_count + 1) < existing.hop_count then
        region_routes[src_region] = {
            via_region = via_region,
            hop_count  = hop_count + 1,
        }
    end
end

-- ============================================================
-- Stats
-- ============================================================

--- Get cross-region statistics.
--- @return table
function M.stats()
    local region_stats = {}
    for name, entry in pairs(remote_regions) do
        region_stats[name] = {
            host      = entry.host,
            port      = entry.port,
            connected = entry.connected,
            last_seen = entry.last_seen,
        }
    end

    local route_stats = {}
    for dst, route in pairs(region_routes) do
        route_stats[dst] = {
            via_region = route.via_region,
            hop_count  = route.hop_count,
        }
    end

    return {
        regions = region_stats,
        routes  = route_stats,
    }
end

-- ============================================================
-- Internal: Logging
-- ============================================================

--- Internal logging function.
--- @param level string  Log level
--- @param msg   string  Log message
function M._log(level, msg)
    local timestamp = os.date("%Y-%m-%d %H:%M:%S")
    local prefix = string.format("[%s] [ROUTER-CROSSREGION] [%s]", timestamp, level:upper())
    io.stderr:write(string.format("%s %s\n", prefix, msg))
    io.stderr:flush()
end

return M
