--[[
    config_friend.lua — Friend Service Configuration

    Central configuration for the Friend service process.
    This process connects to the Router cluster and DBProxy for
    friend data persistence.
]]

local config = {
    -- Service identity
    service_type = 0x02,    -- SERVICE_FRIEND (from service_types.lua)
    service_name = "friend",

    -- Router connection
    router_host = "127.0.0.1",
    router_port = 9000,

    -- DBProxy connection (port 9013 as specified)
    dbproxy_host = "127.0.0.1",
    dbproxy_port = 9013,

    -- Heartbeat
    heartbeat_interval = 5.0,   -- seconds between heartbeats
    heartbeat_timeout  = 15.0,  -- seconds before declaring dead

    -- Friend system limits
    max_friends = 200,          -- max friends per player
    max_blacklist = 100,        -- max blacklisted players

    -- Connection limits
    max_connections = 1000,

    -- Buffer sizes
    recv_buf_size = 64 * 1024,     -- 64 KiB receive buffer
    send_buf_size = 64 * 1024,     -- 64 KiB send buffer

    -- Logging
    log_level = "info",   -- "trace", "debug", "info", "warn", "error"

    -- Version
    version = "0.1.0",
}

return config
