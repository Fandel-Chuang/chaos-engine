--[[
    router.lua — Game↔Game Message Router

    Routes messages between Game processes connected to this Router.
    Uses the consistent hash ring for load-balanced routing and the
    service registry for service discovery.

    Features:
    - Route game data messages to the correct Game instance
    - Consistent hash-based routing via hash_ring
    - Service registry integration for Game discovery
    - Connection tracking for active Game connections
    - Broadcast to all Game instances of a type
]]

local config   = require("router.config")
local protocol = require("router.protocol")
local registry = require("router.registry")
local hash_ring = require("router.hash_ring")

local M = {}

-- ============================================================
-- Connection Tracking
-- ============================================================

-- Active Game connections: conn_id → { sock, service_id, host, port }
local game_connections = {}

--- Register a Game connection with the Router.
--- @param conn_id    string  Connection identifier (e.g., "ip:port")
--- @param sock       userdata  LuaSocket socket
--- @param service_id string  Service identifier
--- @param host       string  Service host
--- @param port       number  Service port
function M.register_connection(conn_id, sock, service_id, host, port)
    game_connections[conn_id] = {
        sock       = sock,
        service_id = service_id,
        host       = host,
        port       = port,
        connected_at = os.time(),
    }
end

--- Unregister a Game connection.
--- @param conn_id string  Connection identifier
function M.unregister_connection(conn_id)
    game_connections[conn_id] = nil
end

--- Get a Game connection by ID.
--- @param conn_id string
--- @return table|nil  Connection info
function M.get_connection(conn_id)
    return game_connections[conn_id]
end

--- Find a connection by service_id.
--- @param service_id string
--- @return string|nil  conn_id, or nil if not found
function M.find_connection_by_service(service_id)
    for conn_id, conn in pairs(game_connections) do
        if conn.service_id == service_id then
            return conn_id
        end
    end
    return nil
end

-- ============================================================
-- Message Routing
-- ============================================================

--- Route a message to a specific Game instance by service_id.
--- @param service_id string  Target service identifier
--- @param msg_type   number  Message type
--- @param body       string  Message body
--- @return boolean  true if message was sent
--- @return string|nil  Error message on failure
function M.route_to_service(service_id, msg_type, body)
    local conn_id = M.find_connection_by_service(service_id)
    if not conn_id then
        return false, "service not connected: " .. service_id
    end

    return M.route_to_connection(conn_id, msg_type, body)
end

--- Route a message to a specific connection.
--- @param conn_id  string  Connection identifier
--- @param msg_type number  Message type
--- @param body     string  Message body
--- @return boolean  true if message was sent
--- @return string|nil  Error message on failure
function M.route_to_connection(conn_id, msg_type, body)
    local conn = game_connections[conn_id]
    if not conn then
        return false, "connection not found: " .. conn_id
    end

    local data = protocol.pack(msg_type, body)
    local ok, err = conn.sock:send(data)
    if not ok then
        return false, "send failed: " .. tostring(err)
    end

    return true
end

--- Route a message using consistent hash ring.
--- The key is hashed to find the responsible Game instance.
--- @param key      string  Hash key (e.g., player_id, entity_id)
--- @param msg_type number  Message type
--- @param body     string  Message body
--- @return boolean  true if message was sent
--- @return string|nil  Error message on failure
function M.route_by_hash(key, msg_type, body)
    local service_id = hash_ring.lookup(key)
    if not service_id then
        return false, "no node in hash ring for key: " .. key
    end

    return M.route_to_service(service_id, msg_type, body)
end

--- Route a message to N replicas using consistent hash ring.
--- @param key      string  Hash key
--- @param n        number  Number of replicas
--- @param msg_type number  Message type
--- @param body     string  Message body
--- @return number  Count of successful sends
function M.route_by_hash_n(key, n, msg_type, body)
    local service_ids = hash_ring.lookup_n(key, n)
    local sent = 0

    for _, service_id in ipairs(service_ids) do
        local ok, _ = M.route_to_service(service_id, msg_type, body)
        if ok then
            sent = sent + 1
        end
    end

    return sent
end

--- Broadcast a message to all connected Game instances of a given type.
--- @param service_type string  Service type filter (nil = all)
--- @param msg_type     number  Message type
--- @param body         string  Message body
--- @return number  Count of successful sends
function M.broadcast(service_type, msg_type, body)
    local sent = 0

    for conn_id, conn in pairs(game_connections) do
        -- Filter by service type if specified
        local skip = false
        if service_type then
            local svc = registry.get(conn.service_id)
            if not svc or svc.service_type ~= service_type then
                skip = true
            end
        end

        if not skip then
            local data = protocol.pack(msg_type, body)
            local ok, _ = conn.sock:send(data)
            if ok then
                sent = sent + 1
            end
        end
    end

    return sent
end

--- Forward a message from one Game to another (Game↔Game relay).
--- The Router acts as a transparent relay between Game processes.
--- @param from_conn_id string  Source connection ID
--- @param to_service_id string  Destination service ID
--- @param msg_type     number  Message type
--- @param body         string  Message body
--- @return boolean  true if forwarded successfully
--- @return string|nil  Error message
function M.forward(from_conn_id, to_service_id, msg_type, body)
    -- Optionally: add routing metadata to the body
    -- For now, just relay the message as-is
    return M.route_to_service(to_service_id, msg_type, body)
end

-- ============================================================
-- Service Discovery Integration
-- ============================================================

--- Sync the hash ring with the current service registry.
--- Adds all alive Game services to the hash ring.
function M.sync_hash_ring()
    hash_ring.clear()

    local game_services = registry.query("game", true) -- alive only
    for _, svc in ipairs(game_services) do
        hash_ring.add_node(svc.service_id, config.hash_ring_vnodes)
    end
end

--- Handle a service registration event.
--- Updates both registry and hash ring.
--- @param service_id   string
--- @param service_type string
--- @param host         string
--- @param port         number
--- @param metadata     string
function M.on_service_registered(service_id, service_type, host, port, metadata)
    registry.register(service_id, service_type, host, port, metadata)

    -- Add to hash ring if it's a Game service
    if service_type == "game" then
        hash_ring.add_node(service_id, config.hash_ring_vnodes)
    end
end

--- Handle a service unregistration event.
--- @param service_id string
function M.on_service_unregistered(service_id)
    local svc = registry.get(service_id)
    if svc and svc.service_type == "game" then
        hash_ring.remove_node(service_id)
    end
    registry.unregister(service_id)
end

-- ============================================================
-- Stats
-- ============================================================

--- Get router statistics.
--- @return table
function M.stats()
    local active_conns = 0
    for _ in pairs(game_connections) do
        active_conns = active_conns + 1
    end

    return {
        active_connections = active_conns,
        registry           = registry.stats(),
        hash_ring          = hash_ring.stats(),
    }
end

return M
