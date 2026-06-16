--[[
    cluster.lua — Router Cluster Synchronization

    Manages Router-to-Router communication for cluster state synchronization.
    Each Router in a cluster shares its service registry with peers,
    enabling global service discovery across the cluster.

    Features:
    - Peer connection management (port 9101)
    - Cluster join/leave protocol
    - Periodic registry sync
    - Leader election (simple: lowest node_id wins)
    - Heartbeat monitoring of peer connections
]]

local config   = require("router.config")
local protocol = require("router.protocol")
local registry = require("router.registry")

local M = {}

-- ============================================================
-- Cluster Node
-- ============================================================

--- Create a cluster node entry.
--- @param node_id string  Node identifier
--- @param host    string  Node host
--- @param port    number  Node port
--- @param region  string  Region name
--- @return table  Node entry
local function new_node(node_id, host, port, region)
    return {
        node_id    = node_id,
        host       = host,
        port       = port,
        region     = region or "unknown",
        sock       = nil,       -- Active connection socket
        state      = "disconnected",  -- "disconnected", "connecting", "connected"
        joined_at  = nil,
        last_seen  = os.time(),
        alive      = false,
    }
end

-- ============================================================
-- Internal State
-- ============================================================

-- Local node identity
local local_node = {
    node_id = config.node_id,
    host    = "127.0.0.1",
    port    = config.cluster_port,
    region  = config.region,
}

-- Peer nodes: node_id → node entry
local peers = {}

-- Cluster leader (node_id of the current leader)
local leader_id = nil

-- Whether cluster sync is running
local running = false

-- ============================================================
-- Local Node
-- ============================================================

--- Set the local node's host address.
--- @param host string  Local host address
function M.set_local_host(host)
    local_node.host = host
end

--- Get the local node info.
--- @return table  { node_id, host, port, region }
function M.get_local_node()
    return {
        node_id = local_node.node_id,
        host    = local_node.host,
        port    = local_node.port,
        region  = local_node.region,
    }
end

-- ============================================================
-- Peer Management
-- ============================================================

--- Add a peer Router to the cluster.
--- @param node_id string  Peer node identifier
--- @param host    string  Peer host
--- @param port    number  Peer port
--- @param region  string  Peer region
function M.add_peer(node_id, host, port, region)
    if peers[node_id] then
        -- Update existing peer
        local peer = peers[node_id]
        peer.host = host
        peer.port = port
        peer.region = region or peer.region
        return
    end

    peers[node_id] = new_node(node_id, host, port, region)
    M._log("info", string.format("Peer added: %s at %s:%d (region=%s)",
        node_id, host, port, region or "unknown"))
end

--- Remove a peer from the cluster.
--- @param node_id string  Peer node identifier
function M.remove_peer(node_id)
    local peer = peers[node_id]
    if not peer then return end

    -- Close connection if active
    if peer.sock then
        pcall(function() peer.sock:close() end)
        peer.sock = nil
    end

    peers[node_id] = nil
    M._log("info", string.format("Peer removed: %s", node_id))

    -- Re-elect leader if the removed peer was leader
    if leader_id == node_id then
        M.elect_leader()
    end
end

--- Get a peer by node_id.
--- @param node_id string
--- @return table|nil  Peer entry
function M.get_peer(node_id)
    return peers[node_id]
end

--- Get all peers.
--- @return table  Array of peer entries
function M.get_all_peers()
    local result = {}
    for _, peer in pairs(peers) do
        result[#result + 1] = peer
    end
    return result
end

--- Get all connected (alive) peers.
--- @return table  Array of peer entries
function M.get_alive_peers()
    local result = {}
    for _, peer in pairs(peers) do
        if peer.alive then
            result[#result + 1] = peer
        end
    end
    return result
end

-- ============================================================
-- Connection Management
-- ============================================================

--- Register a peer connection (called when a peer connects to us).
--- @param node_id string  Peer node identifier
--- @param sock    userdata  LuaSocket socket
function M.register_peer_connection(node_id, sock)
    local peer = peers[node_id]
    if not peer then
        -- Auto-create peer entry for unknown peers
        local addr, port = sock:getpeername()
        M.add_peer(node_id, addr, port, "unknown")
        peer = peers[node_id]
    end

    -- Close old connection if exists
    if peer.sock then
        pcall(function() peer.sock:close() end)
    end

    peer.sock = sock
    peer.state = "connected"
    peer.joined_at = os.time()
    peer.last_seen = os.time()
    peer.alive = true

    M._log("info", string.format("Peer connected: %s", node_id))

    -- Send cluster join message
    local join_msg = protocol.make_cluster_join(
        local_node.node_id, local_node.host, local_node.port, local_node.region
    )
    pcall(function() sock:send(join_msg) end)

    -- Re-elect leader
    M.elect_leader()
end

--- Handle peer disconnection.
--- @param node_id string  Peer node identifier
function M.handle_peer_disconnect(node_id)
    local peer = peers[node_id]
    if not peer then return end

    peer.state = "disconnected"
    peer.sock = nil
    peer.alive = false

    M._log("warn", string.format("Peer disconnected: %s", node_id))

    -- Send cluster leave notification to other peers
    local leave_msg = protocol.pack(protocol.MSG_CLUSTER_LEAVE, node_id)
    for _, p in pairs(peers) do
        if p.alive and p.sock then
            pcall(function() p.sock:send(leave_msg) end)
        end
    end

    -- Re-elect leader
    if leader_id == node_id then
        M.elect_leader()
    end
end

--- Connect to all configured peers.
--- @return number  Count of successful connections
function M.connect_all_peers()
    local connected = 0

    for _, peer in pairs(peers) do
        if peer.state == "connected" then
            connected = connected + 1
        else
            peer.state = "connecting"

            local sock, err = require("socket").tcp()
            if not sock then
                M._log("error", string.format("Failed to create socket for %s: %s",
                    peer.node_id, tostring(err)))
            else
                sock:settimeout(3)  -- 3 second connect timeout
                local ok, conn_err = sock:connect(peer.host, peer.port)
                if not ok then
                    sock:close()
                    M._log("warn", string.format("Failed to connect to peer %s at %s:%d: %s",
                        peer.node_id, peer.host, peer.port, tostring(conn_err)))
                    peer.state = "disconnected"
                else
                    sock:settimeout(0)  -- Non-blocking

                    peer.sock = sock
                    peer.state = "connected"
                    peer.joined_at = os.time()
                    peer.last_seen = os.time()
                    peer.alive = true
                    connected = connected + 1

                    -- Send cluster join message
                    local join_msg = protocol.make_cluster_join(
                        local_node.node_id, local_node.host, local_node.port, local_node.region
                    )
                    pcall(function() sock:send(join_msg) end)

                    M._log("info", string.format("Connected to peer: %s at %s:%d",
                        peer.node_id, peer.host, peer.port))
                end
            end
        end
    end

    -- Re-elect leader after connecting
    if connected > 0 then
        M.elect_leader()
    end

    return connected
end

-- ============================================================
-- Leader Election
-- ============================================================

--- Elect a cluster leader.
--- Simple strategy: the node with the lowest node_id (lexicographic) wins.
--- @return string  Leader node_id
function M.elect_leader()
    local candidates = { local_node.node_id }

    for node_id, peer in pairs(peers) do
        if peer.alive then
            candidates[#candidates + 1] = node_id
        end
    end

    -- Sort lexicographically, lowest wins
    table.sort(candidates)

    local new_leader = candidates[1]
    if new_leader ~= leader_id then
        local old_leader = leader_id
        leader_id = new_leader
        M._log("info", string.format("Leader elected: %s (was: %s)",
            leader_id, old_leader or "none"))

        -- Notify peers of new leader
        if leader_id == local_node.node_id then
            -- We are the leader
            M._log("info", "This node is now the cluster leader")
        end
    end

    return leader_id
end

--- Get the current leader node_id.
--- @return string|nil  Leader node_id, or nil if no leader
function M.get_leader()
    return leader_id
end

--- Check if this node is the leader.
--- @return boolean
function M.is_leader()
    return leader_id == local_node.node_id
end

-- ============================================================
-- Registry Sync
-- ============================================================

--- Sync the local registry to all connected peers.
function M.sync_registry_to_peers()
    if not running then return end

    local data = registry.serialize()
    if not data or data == "" then return end

    local sync_msg = protocol.make_cluster_sync(data)

    for _, peer in pairs(peers) do
        if peer.alive and peer.sock then
            pcall(function() peer.sock:send(sync_msg) end)
        end
    end
end

--- Handle incoming cluster sync data from a peer.
--- @param data string  Serialized registry state
function M.handle_sync_data(data)
    if not data or data == "" then return end
    registry.deserialize_merge(data)
end

-- ============================================================
-- Cluster Lifecycle
-- ============================================================

--- Start the cluster module.
function M.start()
    if running then return end
    running = true
    M._log("info", string.format("Cluster started: node=%s region=%s port=%d",
        local_node.node_id, local_node.region, local_node.port))

    -- Initialize leader (self if no peers)
    if M.elect_leader() == nil then
        leader_id = local_node.node_id
    end
end

--- Stop the cluster module.
function M.stop()
    running = false

    -- Send BYE to all peers
    local bye_msg = protocol.pack(protocol.MSG_ROUTER_BYE, local_node.node_id)
    for _, peer in pairs(peers) do
        if peer.alive and peer.sock then
            pcall(function() peer.sock:send(bye_msg) end)
            pcall(function() peer.sock:close() end)
            peer.sock = nil
        end
        peer.state = "disconnected"
        peer.alive = false
    end

    M._log("info", "Cluster stopped")
end

--- Check if the cluster module is running.
--- @return boolean
function M.is_running()
    return running
end

-- ============================================================
-- Message Handling
-- ============================================================

--- Handle an incoming cluster message from a peer.
--- @param from_node_id string  Sender node_id
--- @param msg_type     number  Message type
--- @param body         string  Message body
function M.handle_message(from_node_id, msg_type, body)
    if msg_type == protocol.MSG_CLUSTER_JOIN then
        -- Parse join message: node_id|host|port|region
        local parts = {}
        for part in (body or ""):gmatch("[^|]+") do
            parts[#parts + 1] = part
        end
        if #parts >= 3 then
            local node_id = parts[1]
            local host    = parts[2]
            local port    = tonumber(parts[3]) or 0
            local region  = parts[4] or "unknown"
            M.add_peer(node_id, host, port, region)
            M._log("info", string.format("Peer %s joined cluster from %s:%d",
                node_id, host, port))
        end

    elseif msg_type == protocol.MSG_CLUSTER_LEAVE then
        M._log("info", string.format("Peer %s left cluster", body or "unknown"))
        M.handle_peer_disconnect(body or from_node_id)

    elseif msg_type == protocol.MSG_CLUSTER_SYNC then
        M.handle_sync_data(body)

    elseif msg_type == protocol.MSG_CLUSTER_ELECT then
        M.elect_leader()

    elseif msg_type == protocol.MSG_ROUTER_HELLO then
        -- Peer discovery: update last_seen
        local peer = peers[from_node_id]
        if peer then
            peer.last_seen = os.time()
            peer.alive = true
        end

    elseif msg_type == protocol.MSG_ROUTER_BYE then
        M._log("info", string.format("Peer %s saying goodbye", from_node_id))
        M.handle_peer_disconnect(from_node_id)
    end
end

-- ============================================================
-- Stats
-- ============================================================

--- Get cluster statistics.
--- @return table
function M.stats()
    local peer_stats = {}
    local alive_count = 0
    local total_count = 0

    for node_id, peer in pairs(peers) do
        total_count = total_count + 1
        if peer.alive then alive_count = alive_count + 1 end
        peer_stats[node_id] = {
            host    = peer.host,
            port    = peer.port,
            region  = peer.region,
            state   = peer.state,
            alive   = peer.alive,
            joined  = peer.joined_at,
        }
    end

    return {
        running       = running,
        local_node    = local_node,
        leader        = leader_id,
        is_leader     = M.is_leader(),
        total_peers   = total_count,
        alive_peers   = alive_count,
        peers         = peer_stats,
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
    local prefix = string.format("[%s] [ROUTER-CLUSTER] [%s]", timestamp, level:upper())
    io.stderr:write(string.format("%s %s\n", prefix, msg))
    io.stderr:flush()
end

return M
