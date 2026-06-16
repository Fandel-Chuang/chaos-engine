--[[
    service_types.lua — Game Process Service Type Enumerations

    Defines 8 service types for Game processes split by functional module.
    Each service type maps to a dedicated process that communicates with
    the Router cluster for message routing and load balancing.

    Service Types:
        1. SERVICE_AUTH      — Authentication & login
        2. SERVICE_FRIEND    — Friend system (add/delete/list/online status)
        3. SERVICE_CHAT      — Chat & messaging
        4. SERVICE_MAIL      — Mail & notifications
        5. SERVICE_GUILD     — Guild/clan management
        6. SERVICE_MATCH     — Matchmaking & queue
        7. SERVICE_SHOP      — Shop & economy
        8. SERVICE_SCENE     — Scene/world management
]]

local M = {}

-- ============================================================
-- Service Type Enumeration
-- ============================================================

M.SERVICE_AUTH   = 0x01   -- Authentication & login service
M.SERVICE_FRIEND = 0x02   -- Friend system service
M.SERVICE_CHAT   = 0x03   -- Chat & messaging service
M.SERVICE_MAIL   = 0x04   -- Mail & notifications service
M.SERVICE_GUILD  = 0x05   -- Guild/clan management service
M.SERVICE_MATCH  = 0x06   -- Matchmaking & queue service
M.SERVICE_SHOP   = 0x07   -- Shop & economy service
M.SERVICE_SCENE  = 0x08   -- Scene/world management service

-- ============================================================
-- Service Name Mapping
-- ============================================================

M.SERVICE_NAMES = {
    [M.SERVICE_AUTH]   = "auth",
    [M.SERVICE_FRIEND] = "friend",
    [M.SERVICE_CHAT]   = "chat",
    [M.SERVICE_MAIL]   = "mail",
    [M.SERVICE_GUILD]  = "guild",
    [M.SERVICE_MATCH]  = "match",
    [M.SERVICE_SHOP]   = "shop",
    [M.SERVICE_SCENE]  = "scene",
}

-- ============================================================
-- Service Default Ports
-- ============================================================

M.SERVICE_PORTS = {
    [M.SERVICE_AUTH]   = 9010,
    [M.SERVICE_FRIEND] = 9011,
    [M.SERVICE_CHAT]   = 9012,
    [M.SERVICE_MAIL]   = 9013,
    [M.SERVICE_GUILD]  = 9014,
    [M.SERVICE_MATCH]  = 9015,
    [M.SERVICE_SHOP]   = 9016,
    [M.SERVICE_SCENE]  = 9017,
}

-- ============================================================
-- Service Message Types (for RPC communication)
-- ============================================================

-- Router registration messages
M.MSG_SERVICE_REGISTER   = 0x2000   -- Service → Router: register
M.MSG_SERVICE_REGISTER_RESP = 0x2001 -- Router → Service: register response
M.MSG_SERVICE_HEARTBEAT  = 0x2002   -- Service → Router: heartbeat
M.MSG_SERVICE_HEARTBEAT_RESP = 0x2003 -- Router → Service: heartbeat response
M.MSG_SERVICE_DEREGISTER = 0x2004   -- Service → Router: deregister

-- Inter-service RPC messages
M.MSG_SERVICE_RPC_CALL   = 0x2100   -- Service → Router → Service: RPC call
M.MSG_SERVICE_RPC_RESP   = 0x2101   -- Service → Router → Service: RPC response

-- ============================================================
-- Service Status
-- ============================================================

M.SERVICE_STATUS = {
    UNREGISTERED = 0,   -- Not yet registered with Router
    REGISTERING  = 1,   -- Registration in progress
    REGISTERED   = 2,   -- Successfully registered
    DEREGISTERING = 3,  -- Deregistration in progress
    ERROR        = 4,   -- Error state
}

-- ============================================================
-- Utility Functions
-- ============================================================

--- Get the human-readable name for a service type.
--- @param service_type number  Service type enum value
--- @return string|nil  Service name, or nil if unknown
function M.get_service_name(service_type)
    return M.SERVICE_NAMES[service_type]
end

--- Get the default port for a service type.
--- @param service_type number  Service type enum value
--- @return number|nil  Default port, or nil if unknown
function M.get_service_port(service_type)
    return M.SERVICE_PORTS[service_type]
end

--- Check if a service type is valid.
--- @param service_type number  Service type enum value
--- @return boolean  True if valid
function M.is_valid_service(service_type)
    return M.SERVICE_NAMES[service_type] ~= nil
end

return M
