--[[
    hash_ring.lua — Consistent Hash Ring

    Implements a consistent hash ring with virtual nodes for distributing
    Game messages across multiple Game service instances.

    Features:
    - 150 virtual nodes per physical node (configurable)
    - MD5-based hashing (via LuaSocket's digest or pure-Lua fallback)
    - Add/remove nodes with minimal key redistribution
    - Lookup: given a key, find the responsible node
    - Support for weighted nodes (more vnodes = more load)
]]

local config = require("router.config")

local M = {}

-- ============================================================
-- Hash Function
-- ============================================================

-- Try to use LuaSocket's MD5 if available, otherwise use a simple
-- DJB2-based hash that works well for consistent hashing.

local has_luasocket_md5 = false
local function try_load_md5()
    local ok, socket = pcall(require, "socket")
    if ok and socket then
        return true
    end
    return false
end

has_luasocket_md5 = try_load_md5()

--- Compute a 32-bit hash of a string.
--- Uses DJB2 algorithm (good distribution, fast).
--- @param str string  Input string
--- @return number  32-bit unsigned integer hash
local function hash_djb2(str)
    local hash = 5381
    for i = 1, #str do
        hash = ((hash * 33) + string.byte(str, i)) % 4294967296  -- 2^32
    end
    return hash
end

--- Compute a 32-bit hash of a string using MD5 (if available).
--- Falls back to DJB2.
--- @param str string  Input string
--- @return number  32-bit unsigned integer hash
local function hash_str(str)
    if has_luasocket_md5 then
        local socket = require("socket")
        -- Use first 8 hex chars of MD5 as 32-bit hash
        local md5 = socket.digest and socket.digest.md5 and socket.digest.md5(str)
        if md5 then
            return tonumber(md5:sub(1, 8), 16) or hash_djb2(str)
        end
    end
    return hash_djb2(str)
end

-- ============================================================
-- Ring Data Structure
-- ============================================================

-- Sorted list of { hash, node_id } entries
-- The ring is kept sorted by hash value for binary search lookups.
local ring = {}

-- Physical nodes: node_id → { vnode_count, weight }
local nodes = {}

-- Total number of virtual nodes
local total_vnodes = 0

-- ============================================================
-- Binary Search Helper
-- ============================================================

--- Find the first ring entry with hash >= target_hash.
--- Wraps around to the first entry if none found.
--- @param target_hash number  32-bit hash value
--- @return table  { hash, node_id }
local function find_node(target_hash)
    if #ring == 0 then
        return nil
    end

    -- Binary search for first hash >= target_hash
    local lo, hi = 1, #ring
    while lo < hi do
        local mid = math.floor((lo + hi) / 2)
        if ring[mid].hash < target_hash then
            lo = mid + 1
        else
            hi = mid
        end
    end

    -- If all hashes are smaller, wrap around to the first
    if ring[lo].hash < target_hash then
        return ring[1]
    end

    return ring[lo]
end

-- ============================================================
-- Node Management
-- ============================================================

--- Add a node to the hash ring.
--- @param node_id     string  Unique node identifier
--- @param vnode_count number  Number of virtual nodes (default from config)
--- @param weight      number  Weight multiplier for vnode count (default 1.0)
function M.add_node(node_id, vnode_count, weight)
    if nodes[node_id] then
        -- Node already exists, remove first then re-add
        M.remove_node(node_id)
    end

    vnode_count = vnode_count or config.hash_ring_vnodes
    weight = weight or 1.0
    local actual_vnodes = math.max(1, math.floor(vnode_count * weight))

    nodes[node_id] = {
        vnode_count = actual_vnodes,
        weight = weight,
    }

    -- Add virtual nodes to the ring
    for i = 0, actual_vnodes - 1 do
        local vnode_key = string.format("%s#vnode%d", node_id, i)
        local h = hash_str(vnode_key)
        table.insert(ring, { hash = h, node_id = node_id })
    end

    total_vnodes = total_vnodes + actual_vnodes

    -- Re-sort the ring by hash
    table.sort(ring, function(a, b) return a.hash < b.hash end)
end

--- Remove a node from the hash ring.
--- @param node_id string  Node identifier
--- @return boolean  true if node was found and removed
function M.remove_node(node_id)
    local node = nodes[node_id]
    if not node then
        return false
    end

    -- Remove all virtual nodes for this node
    local new_ring = {}
    for _, entry in ipairs(ring) do
        if entry.node_id ~= node_id then
            new_ring[#new_ring + 1] = entry
        end
    end
    ring = new_ring

    total_vnodes = total_vnodes - node.vnode_count
    nodes[node_id] = nil

    return true
end

--- Update a node's weight (re-adds with new vnode count).
--- @param node_id string
--- @param weight  number  New weight multiplier
function M.update_weight(node_id, weight)
    local node = nodes[node_id]
    if not node then return end

    local vnode_count = config.hash_ring_vnodes
    M.remove_node(node_id)
    M.add_node(node_id, vnode_count, weight)
end

-- ============================================================
-- Lookup
-- ============================================================

--- Find the node responsible for a given key.
--- @param key string  Key to hash (e.g., player_id, entity_id)
--- @return string|nil  node_id of the responsible node, or nil if ring is empty
function M.lookup(key)
    if #ring == 0 then
        return nil
    end

    local h = hash_str(key)
    local entry = find_node(h)
    return entry and entry.node_id or nil
end

--- Find N distinct nodes responsible for a key (for replication).
--- Returns up to N distinct nodes, starting from the primary.
--- @param key        string  Key to hash
--- @param n          number  Number of replicas desired
--- @return table  Array of node_id strings
function M.lookup_n(key, n)
    if #ring == 0 then
        return {}
    end

    local h = hash_str(key)
    local results = {}
    local seen = {}

    -- Find starting position
    local start_idx = 1
    for i, entry in ipairs(ring) do
        if entry.hash >= h then
            start_idx = i
            break
        end
    end

    -- Walk the ring collecting distinct nodes
    local idx = start_idx
    for _ = 1, #ring do
        local node_id = ring[idx].node_id
        if not seen[node_id] then
            seen[node_id] = true
            results[#results + 1] = node_id
            if #results >= n then
                break
            end
        end
        idx = idx + 1
        if idx > #ring then
            idx = 1
        end
    end

    return results
end

-- ============================================================
-- Info & Stats
-- ============================================================

--- Get all nodes in the ring.
--- @return table  Array of node_id strings
function M.get_nodes()
    local result = {}
    for node_id, _ in pairs(nodes) do
        result[#result + 1] = node_id
    end
    return result
end

--- Check if a node exists in the ring.
--- @param node_id string
--- @return boolean
function M.has_node(node_id)
    return nodes[node_id] ~= nil
end

--- Get the number of physical nodes.
--- @return number
function M.node_count()
    local n = 0
    for _ in pairs(nodes) do
        n = n + 1
    end
    return n
end

--- Get the total number of virtual nodes.
--- @return number
function M.vnode_count()
    return total_vnodes
end

--- Get ring statistics.
--- @return table
function M.stats()
    local node_stats = {}
    for node_id, node in pairs(nodes) do
        node_stats[node_id] = {
            vnodes = node.vnode_count,
            weight = node.weight,
            -- Calculate what fraction of the ring this node owns
            fraction = total_vnodes > 0 and (node.vnode_count / total_vnodes) or 0,
        }
    end

    return {
        physical_nodes = M.node_count(),
        virtual_nodes  = total_vnodes,
        nodes          = node_stats,
    }
end

--- Clear the entire ring.
function M.clear()
    ring = {}
    nodes = {}
    total_vnodes = 0
end

return M
