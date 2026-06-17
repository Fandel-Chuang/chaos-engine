--[[
    config.lua — Gateway Configuration

    Central configuration for the Lua Gateway process.
    All tunable parameters live here.
]]

local config = {
    -- TCP ports
    tcp_port = 9000,   -- TCP client connection port
    kcp_port = 9001,   -- KCP client connection port (reserved)
    ws_port  = 9002,   -- WebSocket client connection port (reserved)

    -- Backend Game service addresses
    backends = {
        { host = "127.0.0.1", port = 7777 },
    },

    -- TODO: 后续改为向 Router 查询本机 Game 列表
    -- Game server connections (same-machine Game processes)
    game_servers = {
        { host = "127.0.0.1", port = 9100 },
    },

    -- Heartbeat
    heartbeat_interval = 30,   -- seconds between heartbeat checks
    heartbeat_timeout  = 90,   -- seconds before declaring client dead

    -- Connection limits
    max_connections = 10000,

    -- Buffer sizes
    recv_buf_size = 64 * 1024,     -- 64 KiB receive buffer
    send_buf_size = 64 * 1024,     -- 64 KiB send buffer

    -- Backend connection pool
    backend_pool_size = 32,        -- max persistent connections to each backend

    -- Logging
    log_level = "info",   -- "trace", "debug", "info", "warn", "error"

    -- Version
    version = "0.1.0",
}

return config
