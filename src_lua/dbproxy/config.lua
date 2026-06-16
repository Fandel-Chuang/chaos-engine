--[[
    config.lua — DBProxy Configuration

    Central configuration for the Lua DBProxy process.
    All tunable parameters live here.
]]

local config = {
    -- TCP ports
    sync_port = 9001,   -- State sync port (Game → DBProxy frame sync)
    db_port   = 9003,   -- Database proxy port (Game → DBProxy DB ops)

    -- MongoDB connection
    mongo_uri = "mongodb://127.0.0.1:27017",
    mongo_db  = "chaos_engine",

    -- Heartbeat
    heartbeat_interval = 0.5,   -- seconds between heartbeat checks
    heartbeat_timeout  = 3.0,   -- seconds before declaring peer dead

    -- Role: "primary" or "backup"
    role = "primary",

    -- Peer DBProxy (for primary↔backup heartbeat & failover)
    peer_host = "127.0.0.1",
    peer_port = 9101,           -- peer heartbeat/sync port

    -- Buffer sizes
    max_frame_size = 64 * 1024,     -- 64 KiB (matches CE_SYNC_MAX_FRAME_SIZE)
    recv_buf_size  = 128 * 1024,    -- 128 KiB receive buffer

    -- Connection limits
    max_sync_connections = 16,
    max_db_connections   = 32,

    -- Logging
    log_level = "info",   -- "trace", "debug", "info", "warn", "error"

    -- Version
    version = "0.1.0",
}

return config
