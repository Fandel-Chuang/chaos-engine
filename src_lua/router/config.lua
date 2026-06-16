--[[
    config.lua — Router Configuration

    Central configuration for the Lua Router process.
    All tunable parameters live here.
]]

local config = {
    -- ============================================================
    -- Router Identity
    -- ============================================================

    -- Unique Router node ID
    node_id = "router-1",

    -- Region this Router belongs to
    region = "default",

    -- ============================================================
    -- TCP Ports
    -- ============================================================

    -- Port 9100: Game ↔ Router (Game processes connect here)
    game_port = 9100,

    -- Port 9101: Router ↔ Router (cluster sync between Routers)
    cluster_port = 9101,

    -- ============================================================
    -- Cluster Peers
    -- ============================================================

    -- Other Router nodes in the cluster
    -- Each entry: { node_id, host, port }
    peers = {
        -- { node_id = "router-2", host = "127.0.0.1", port = 9101 },
    },

    -- ============================================================
    -- Cross-Region
    -- ============================================================

    -- Remote region Routers for cross-region routing
    -- Each entry: { region, host, port }
    remote_regions = {
        -- { region = "us-east", host = "router-us.example.com", port = 9101 },
        -- { region = "eu-west", host = "router-eu.example.com", port = 9101 },
    },

    -- ============================================================
    -- Hash Ring
    -- ============================================================

    -- Number of virtual nodes per physical node in the consistent hash ring
    hash_ring_vnodes = 150,

    -- ============================================================
    -- Heartbeat
    -- ============================================================

    -- Interval between PING/PONG heartbeats (seconds)
    heartbeat_interval = 1,

    -- Time before declaring a connection dead (seconds)
    heartbeat_timeout = 3,

    -- ============================================================
    -- Connection Limits
    -- ============================================================

    -- Max Game connections on port 9100
    max_game_connections = 1024,

    -- Max cluster peer connections on port 9101
    max_cluster_connections = 64,

    -- ============================================================
    -- Buffer Sizes
    -- ============================================================

    recv_buf_size = 128 * 1024,     -- 128 KiB receive buffer
    send_buf_size = 128 * 1024,     -- 128 KiB send buffer

    -- ============================================================
    -- Logging
    -- ============================================================

    log_level = "info",   -- "trace", "debug", "info", "warn", "error"

    -- ============================================================
    -- Version
    -- ============================================================

    version = "0.1.0",
}

return config
