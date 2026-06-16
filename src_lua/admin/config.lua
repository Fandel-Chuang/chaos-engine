local config = {
    ADMIN_PORT = 9090,
    IPC_PATH = "/tmp/chaos_admin.sock",
    IPC_TIMEOUT = 5,  -- seconds
    PUSH_INTERVALS = {
        stats   = 0.5,
        aoi     = 0.5,
        cell    = 1.0,
        network = 1.0,
        memory  = 2.0,
        render  = 0.5,
    },
    LOG_MAX_LINES = 200,
    VERSION = "0.2.0",
}
return config
