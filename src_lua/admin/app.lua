--[[
    app.lua — Lapis Application: HTTP routes for ChaosEngine Admin Dashboard

    All REST API endpoints. WebSocket push is handled separately
    via the dashboard HTML's polling fallback.
]]

local lapis = require("lapis")
local json = require("cjson")
local ipc = require("admin.ipc_client")
local config = require("admin.config")
local dashboard_html = require("admin.dashboard_html")

local app = lapis.Application()

-- Enable CORS
app:before_filter(function(self)
    self.res.headers["Access-Control-Allow-Origin"] = "*"
    self.res.headers["Access-Control-Allow-Methods"] = "GET, OPTIONS"
    self.res.headers["Access-Control-Allow-Headers"] = "Content-Type"
end)

-- Helper: make an IPC call and return a standard JSON response
local function ipc_response(method, params)
    local sock, err = ipc.connect()
    if not sock then
        return {
            status = 503,
            json = {
                ok = false,
                error = "Cannot connect to chaos_server: " .. tostring(err),
                timestamp = os.time()
            }
        }
    end

    local result, call_err = ipc.call(sock, method, params)
    ipc.close(sock)

    if not result then
        return {
            status = 500,
            json = {
                ok = false,
                error = "IPC call failed: " .. tostring(call_err),
                timestamp = os.time()
            }
        }
    end

    return {
        json = {
            ok = true,
            timestamp = os.time(),
            data = result
        }
    }
end

-- ============================================================
-- HTTP Routes
-- ============================================================

app:get("/", function(self)
    return dashboard_html.get()
end)

app:get("/api/stats", function(self)
    return ipc_response("stats")
end)

app:get("/api/aoi", function(self)
    return ipc_response("aoi")
end)

app:get("/api/cell", function(self)
    return ipc_response("cell")
end)

app:get("/api/network", function(self)
    return ipc_response("network")
end)

app:get("/api/memory", function(self)
    return ipc_response("memory")
end)

app:get("/api/cpu", function(self)
    return ipc_response("cpu")
end)

app:get("/api/log", function(self)
    local lines = tonumber(self.params.lines) or config.LOG_MAX_LINES
    local since_us = tonumber(self.params.since_us) or 0
    return ipc_response("log", { lines = lines, since_us = since_us })
end)

app:get("/api/render", function(self)
    return ipc_response("render")
end)

app:get("/api/system", function(self)
    return ipc_response("system")
end)

app:get("/api/health", function(self)
    return ipc_response("health")
end)

return app
