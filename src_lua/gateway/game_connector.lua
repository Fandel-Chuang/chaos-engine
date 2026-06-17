--[[
    game_connector.lua — Game Server Connection Module

    Dedicated module for managing persistent TCP connections to
    same-machine Game server processes.

    Features:
    - GameConnector class with connect/disconnect/send/is_connected
    - State machine: DISCONNECTED → CONNECTING → CONNECTED → DISCONNECTED
    - LuaSocket primary backend, raw TCP fallback
    - Async receive callback support
]]

-- ============================================================
-- State Constants
-- ============================================================

local STATE = {
    DISCONNECTED = "disconnected",
    CONNECTING   = "connecting",
    CONNECTED    = "connected",
}

-- ============================================================
-- GameConnector Class
-- ============================================================

local GameConnector = {}
GameConnector.__index = GameConnector

--- Create a new GameConnector instance.
--- @param name  string  Human-readable name for logging
--- @return table  GameConnector instance
function GameConnector.new(name)
    local self = setmetatable({}, GameConnector)
    self.name      = name or "unnamed"
    self.state     = STATE.DISCONNECTED
    self.host      = nil
    self.port      = nil
    self.sock      = nil
    self.on_receive = nil   -- callback(connector, data)
    self._socket_available = pcall(require, "socket")
    return self
end

--- Connect to a Game server.
--- @param host  string  Game server host
--- @param port  number  Game server port
--- @return boolean  true on success
--- @return string|nil  Error message on failure
function GameConnector:connect(host, port)
    if self.state == STATE.CONNECTED then
        self:disconnect()
    end

    self.host = host
    self.port = port
    self.state = STATE.CONNECTING

    if self._socket_available then
        return self:_connect_luasocket(host, port)
    else
        return self:_connect_raw(host, port)
    end
end

--- Internal: Connect via LuaSocket.
function GameConnector:_connect_luasocket(host, port)
    local socket = require("socket")

    local sock, err = socket.tcp()
    if not sock then
        self.state = STATE.DISCONNECTED
        return false, "socket.tcp() failed: " .. tostring(err)
    end

    sock:settimeout(5)  -- 5 second connect timeout
    local ok, conn_err = sock:connect(host, port)
    if not ok then
        sock:close()
        self.state = STATE.DISCONNECTED
        return false, "connect to " .. host .. ":" .. port .. " failed: " .. tostring(conn_err)
    end

    sock:settimeout(0)  -- Non-blocking for event loop
    self.sock = sock
    self.state = STATE.CONNECTED
    return true
end

--- Internal: Connect via raw TCP (os.execute + netcat fallback).
function GameConnector:_connect_raw(host, port)
    -- Attempt a simple TCP check via netcat to verify reachability
    local cmd = string.format("nc -z -w 2 %s %d 2>/dev/null", host, port)
    local ok = os.execute(cmd)

    if ok then
        -- Raw fallback: store connection info for os.execute-based send
        -- Note: raw mode has limited functionality (no persistent socket)
        self.state = STATE.CONNECTED
        return true
    else
        self.state = STATE.DISCONNECTED
        return false, "raw connect to " .. host .. ":" .. port .. " failed (nc unreachable)"
    end
end

--- Disconnect from the Game server.
function GameConnector:disconnect()
    if self.sock then
        self.sock:close()
        self.sock = nil
    end
    self.state = STATE.DISCONNECTED
end

--- Send data to the connected Game server.
--- @param data  string  Data to send
--- @return boolean  true on success
--- @return string|nil  Error message on failure
function GameConnector:send(data)
    if self.state ~= STATE.CONNECTED then
        return false, "not connected (state: " .. self.state .. ")"
    end

    if self.sock then
        -- LuaSocket path
        local sent, err = self.sock:send(data)
        if not sent then
            -- Connection may be broken
            self:disconnect()
            return false, "send failed: " .. tostring(err)
        end
        return true
    else
        -- Raw fallback: use netcat to send
        local cmd = string.format(
            "echo -n '%s' | nc -w 2 %s %d 2>/dev/null",
            data:gsub("'", "'\\''"),  -- Escape single quotes
            self.host,
            self.port
        )
        local ok = os.execute(cmd)
        if ok then
            return true
        else
            self:disconnect()
            return false, "raw send failed"
        end
    end
end

--- Check if the connector is currently connected.
--- @return boolean
function GameConnector:is_connected()
    return self.state == STATE.CONNECTED
end

--- Get the current state.
--- @return string  One of "disconnected", "connecting", "connected"
function GameConnector:get_state()
    return self.state
end

--- Set the receive callback.
--- Called when data is received from the Game server.
--- @param callback  function|nil  function(connector, data)
function GameConnector:set_on_receive(callback)
    self.on_receive = callback
end

--- Get the underlying socket for select()-based event loops.
--- @return userdata|nil  Socket, or nil if not connected via LuaSocket
function GameConnector:get_socket()
    return self.sock
end

--- Try to receive data from the socket (non-blocking).
--- Should be called from the event loop when the socket is readable.
--- @return string|nil  Received data, or nil if nothing available
--- @return string|nil  Error message
function GameConnector:receive()
    if not self.sock or self.state ~= STATE.CONNECTED then
        return nil, "not connected"
    end

    -- Non-blocking receive
    self.sock:settimeout(0)
    local data, err = self.sock:receive("*a")

    if data then
        -- Fire callback if set
        if self.on_receive then
            self.on_receive(self, data)
        end
        return data
    elseif err == "timeout" or err == "wantread" then
        -- No data available, not an error
        return nil
    else
        -- Connection error
        self:disconnect()
        return nil, err
    end
end

-- ============================================================
-- Module Exports
-- ============================================================

return {
    STATE = STATE,
    GameConnector = GameConnector,
    new = GameConnector.new,
}
