--[[
    registry.lua — Router Service Registry

    Maintains a registry of all Game services connected to this Router.
    Each service registers with its type, address, and optional metadata.

    Features:
    - Register/unregister Game services
    - Query services by type
    - Service heartbeat tracking
    - Automatic cleanup of stale services
    - Integration with hash ring for consistent routing
]]

local config   = require("router.config")
local protocol = require("router.protocol")

local M = {}

-- ============================================================
-- Service Entry
-- ============================================================

--- Create a new service entry.
--- @param service_id   string  Unique service identifier
--- @param service_type string  Service type (e.g., "game", "dbproxy")
--- @param host         string  Service host
--- @param port         number  Service port
--- @param metadata     string  Optional JSON metadata
--- @return table  Service entry
local function new_entry(service_id, service_type, host, port, metadata)
    return {
        service_id   = service_id,
        service_type = service_type,
        host         = host,
        port         = port,
        metadata     = metadata or "{}",
        registered_at = os.time(),
        last_heartbeat = os.time(),
        alive        = true,
    }
end

-- ============================================================
-- Internal State
-- ============================================================

-- All registered services: service_id → entry
local services = {}

-- Services grouped by type: service_type → { service_id → entry }
local services_by_type = {}

-- ============================================================
-- Registration
-- ============================================================

--- Register a service with the Router.
--- @param service_id   string  Unique service identifier
--- @param service_type string  Service type
--- @param host         string  Service host
--- @param port         number  Service port
--- @param metadata     string  Optional JSON metadata
--- @return boolean  true on success, false if already registered
function M.register(service_id, service_type, host, port, metadata)
    if services[service_id] then
        -- Update existing entry
        local entry = services[service_id]
        entry.service_type = service_type
        entry.host = host
        entry.port = port
        entry.metadata = metadata or entry.metadata
        entry.last_heartbeat = os.time()
        entry.alive = true
        return true
    end

    local entry = new_entry(service_id, service_type, host, port, metadata)
    services[service_id] = entry

    -- Index by type
    if not services_by_type[service_type] then
        services_by_type[service_type] = {}
    end
    services_by_type[service_type][service_id] = entry

    return true
end

--- Unregister a service.
--- @param service_id string  Service identifier
--- @return boolean  true if found and removed
function M.unregister(service_id)
    local entry = services[service_id]
    if not entry then
        return false
    end

    -- Remove from type index
    local by_type = services_by_type[entry.service_type]
    if by_type then
        by_type[service_id] = nil
        -- Clean up empty type buckets
        if next(by_type) == nil then
            services_by_type[entry.service_type] = nil
        end
    end

    services[service_id] = nil
    return true
end

-- ============================================================
-- Heartbeat
-- ============================================================

--- Record a heartbeat for a service.
--- @param service_id string  Service identifier
--- @return boolean  true if service found
function M.heartbeat(service_id)
    local entry = services[service_id]
    if not entry then
        return false
    end
    entry.last_heartbeat = os.time()
    entry.alive = true
    return true
end

--- Check and clean up stale services.
--- @param timeout_sec number  Heartbeat timeout in seconds
--- @return number  Count of services marked dead
function M.cleanup_stale(timeout_sec)
    timeout_sec = timeout_sec or config.heartbeat_timeout
    local now = os.time()
    local dead_count = 0

    for service_id, entry in pairs(services) do
        if entry.alive and (now - entry.last_heartbeat) > timeout_sec then
            entry.alive = false
            dead_count = dead_count + 1
        end
    end

    return dead_count
end

--- Remove all dead services.
--- @return number  Count of services removed
function M.purge_dead()
    local removed = 0
    for service_id, entry in pairs(services) do
        if not entry.alive then
            M.unregister(service_id)
            removed = removed + 1
        end
    end
    return removed
end

-- ============================================================
-- Query
-- ============================================================

--- Get a service by ID.
--- @param service_id string
--- @return table|nil  Service entry
function M.get(service_id)
    return services[service_id]
end

--- Query all services of a given type.
--- @param service_type string  Service type, or nil for all
--- @param alive_only   boolean  Only return alive services (default true)
--- @return table  Array of service entries
function M.query(service_type, alive_only)
    if alive_only == nil then alive_only = true end

    local results = {}

    if service_type then
        local by_type = services_by_type[service_type]
        if by_type then
            for _, entry in pairs(by_type) do
                if not alive_only or entry.alive then
                    results[#results + 1] = entry
                end
            end
        end
    else
        for _, entry in pairs(services) do
            if not alive_only or entry.alive then
                results[#results + 1] = entry
            end
        end
    end

    return results
end

--- Get all service IDs of a given type.
--- @param service_type string
--- @param alive_only   boolean
--- @return table  Array of service_id strings
function M.get_ids(service_type, alive_only)
    local entries = M.query(service_type, alive_only)
    local ids = {}
    for _, entry in ipairs(entries) do
        ids[#ids + 1] = entry.service_id
    end
    return ids
end

--- Check if a service exists and is alive.
--- @param service_id string
--- @return boolean
function M.is_alive(service_id)
    local entry = services[service_id]
    return entry ~= nil and entry.alive
end

--- Get the count of registered services.
--- @param service_type string  Optional filter by type
--- @param alive_only   boolean  Only count alive services
--- @return number
function M.count(service_type, alive_only)
    local entries = M.query(service_type, alive_only)
    return #entries
end

-- ============================================================
-- Serialization (for cluster sync)
-- ============================================================

--- Serialize all services to a string for cluster sync.
--- @return string  Serialized registry state
function M.serialize()
    local parts = {}
    for service_id, entry in pairs(services) do
        parts[#parts + 1] = string.format("%s|%s|%s|%d|%s|%d|%s",
            service_id,
            entry.service_type,
            entry.host,
            entry.port,
            entry.metadata,
            entry.last_heartbeat,
            entry.alive and "1" or "0"
        )
    end
    return table.concat(parts, "\n")
end

--- Deserialize and merge registry state from cluster sync.
--- @param data string  Serialized registry state
function M.deserialize_merge(data)
    if not data or data == "" then return end

    for line in data:gmatch("[^\n]+") do
        local parts = {}
        for part in line:gmatch("[^|]+") do
            parts[#parts + 1] = part
        end
        if #parts >= 7 then
            local service_id   = parts[1]
            local service_type = parts[2]
            local host         = parts[3]
            local port         = tonumber(parts[4]) or 0
            local metadata     = parts[5]
            local last_hb      = tonumber(parts[6]) or 0
            local alive        = parts[7] == "1"

            -- Only merge if we don't have this service or remote is newer
            local existing = services[service_id]
            if not existing or last_hb > existing.last_heartbeat then
                M.register(service_id, service_type, host, port, metadata)
                local entry = services[service_id]
                if entry then
                    entry.last_heartbeat = last_hb
                    entry.alive = alive
                end
            end
        end
    end
end

-- ============================================================
-- Stats
-- ============================================================

--- Get registry statistics.
--- @return table
function M.stats()
    local type_counts = {}
    local total_alive = 0
    local total_dead = 0

    for service_type, by_type in pairs(services_by_type) do
        local alive = 0
        local dead = 0
        for _, entry in pairs(by_type) do
            if entry.alive then
                alive = alive + 1
            else
                dead = dead + 1
            end
        end
        type_counts[service_type] = { alive = alive, dead = dead }
        total_alive = total_alive + alive
        total_dead = total_dead + dead
    end

    return {
        total       = total_alive + total_dead,
        alive       = total_alive,
        dead        = total_dead,
        by_type     = type_counts,
    }
end

return M
