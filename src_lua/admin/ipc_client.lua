--[[
    ipc_client.lua — JSON-RPC 2.0 Client (Unix Socket via ngx.socket)

    Connects to chaos_server's admin IPC socket and sends JSON-RPC requests.
    Uses ngx.socket.connect for Unix domain sockets (OpenResty native).
]]

local json = require("cjson")
local config = require("admin.config")

local M = {}

--- Connect to chaos_server admin IPC socket.
function M.connect()
    local sock, err = ngx.socket.connect("unix:" .. config.IPC_PATH)
    if not sock then
        return nil, "Cannot connect to chaos_server at " .. config.IPC_PATH .. ": " .. tostring(err)
    end
    sock:settimeout(config.IPC_TIMEOUT * 1000) -- ngx uses milliseconds
    return sock
end

--- Send a JSON-RPC 2.0 request and receive the response.
function M.call(sock, method, params)
    if not sock then
        return nil, "Socket is nil"
    end

    local req = json.encode({
        jsonrpc = "2.0",
        id = ngx.time() * 1000,
        method = method,
        params = params or {}
    })

    local bytes, err = sock:send(req .. "\n")
    if not bytes then
        return nil, "Send failed: " .. tostring(err)
    end

    -- Read response line (newline-delimited JSON)
    sock:settimeout(config.IPC_TIMEOUT * 1000)
    local line, read_err = sock:receive("*l")
    sock:settimeout(0)

    if not line then
        return nil, "Receive failed: " .. tostring(read_err)
    end

    local ok, resp = pcall(json.decode, line)
    if not ok then
        return nil, "JSON decode failed: " .. tostring(resp)
    end

    if resp.error then
        return nil, "RPC error: " .. tostring(resp.error.message or resp.error)
    end

    return resp.result
end

--- Close the IPC socket connection.
function M.close(sock)
    if sock then
        pcall(sock.close, sock)
    end
end

return M
