--[[
    handler.lua — DB Request Handler

    Processes incoming DB operation requests from Game processes
    on the db port (9003). Each request is dispatched by message type.

    MongoDB operations use os.execute with mongo shell commands
    (simplified implementation — production would use lua-mongo or
    a native driver via FFI).

    Message Types:
        MSG_SAVE_PLAYER  (0x01) — Save single player data
        MSG_LOAD_PLAYER  (0x02) — Load player data
        MSG_SAVE_WORLD   (0x03) — Save full world state
        MSG_HEARTBEAT    (0x04) — Heartbeat ping/pong
]]

local config   = require("dbproxy.config")
local protocol = require("dbproxy.protocol")
local mirror   = require("dbproxy.state_mirror")

local M = {}

-- ============================================================
-- Internal: MongoDB Helpers
-- ============================================================

--- Execute a mongo shell command and return stdout.
--- Uses mongosh if available, falls back to legacy mongo shell.
--- @param js_code  string  JavaScript code to execute
--- @return string|nil  stdout on success, nil on failure
local function mongo_exec(js_code)
    -- Escape single quotes in JS code
    local escaped = js_code:gsub("'", "'\\''")

    -- Try mongosh first (MongoDB 6+)
    local cmd = string.format(
        "mongosh '%s' --quiet --eval '%s' 2>/dev/null",
        config.mongo_uri, escaped
    )
    local handle = io.popen(cmd)
    if handle then
        local result = handle:read("*a")
        handle:close()
        if result and #result > 0 then
            return result
        end
    end

    -- Fallback to legacy mongo shell
    cmd = string.format(
        "mongo '%s' --quiet --eval '%s' 2>/dev/null",
        config.mongo_uri, escaped
    )
    handle = io.popen(cmd)
    if handle then
        local result = handle:read("*a")
        handle:close()
        return result
    end

    return nil
end

--- Build a MongoDB insert/update JavaScript statement.
--- @param collection  string  Collection name
--- @param doc_json    string  JSON document string
--- @param id_field    string  ID field name (default: "_id")
--- @return string     JavaScript code
local function build_upsert_js(collection, doc_json, id_field)
    id_field = id_field or "_id"
    return string.format([[
        var doc = %s;
        var result = db.getSiblingDB('%s').%s.updateOne(
            { %s: doc.%s },
            { $set: doc },
            { upsert: true }
        );
        printjson({ ok: 1, matched: result.matchedCount, modified: result.modifiedCount, upserted: result.upsertedCount });
    ]], doc_json, config.mongo_db, collection, id_field, id_field)
end

--- Build a MongoDB find JavaScript statement.
--- @param collection  string  Collection name
--- @param query_json  string  JSON query string
--- @return string     JavaScript code
local function build_find_js(collection, query_json)
    return string.format([[
        var doc = db.getSiblingDB('%s').%s.findOne(%s);
        if (doc) { printjson(doc); } else { printjson(null); }
    ]], config.mongo_db, collection, query_json)
end

-- ============================================================
-- Request Handlers
-- ============================================================

--- Handle DB_SAVE_PLAYER: Save single player data to MongoDB.
--- Payload format: JSON {player_id, data}
--- @param payload  string  Raw payload
--- @return string  Binary response
function M.handle_save_player(payload)
    local ok, player_data = pcall(json_decode, payload)
    if not ok or not player_data or not player_data.player_id then
        return protocol.pack_response(protocol.RESP_ERROR, "invalid player data")
    end

    -- Ensure _id is set
    if not player_data._id then
        player_data._id = tostring(player_data.player_id)
    end

    local doc_json = M._encode_json(player_data)
    if not doc_json then
        return protocol.pack_response(protocol.RESP_ERROR, "JSON encoding failed")
    end

    local js = build_upsert_js("players", doc_json, "_id")
    local result = mongo_exec(js)

    if not result then
        return protocol.pack_response(protocol.RESP_ERROR, "MongoDB save failed")
    end

    return protocol.pack_response(protocol.RESP_OK, result)
end

--- Handle DB_LOAD_PLAYER: Load player data from MongoDB.
--- Payload format: JSON {player_id}
--- @param payload  string  Raw payload
--- @return string  Binary response
function M.handle_load_player(payload)
    local ok, query = pcall(json_decode, payload)
    if not ok or not query or not query.player_id then
        return protocol.pack_response(protocol.RESP_ERROR, "invalid query")
    end

    local query_json = M._encode_json({ _id = tostring(query.player_id) })
    if not query_json then
        return protocol.pack_response(protocol.RESP_ERROR, "JSON encoding failed")
    end

    local js = build_find_js("players", query_json)
    local result = mongo_exec(js)

    if not result or result:match("^null%s*$") then
        return protocol.pack_response(protocol.RESP_ERROR, "player not found")
    end

    return protocol.pack_response(protocol.RESP_DATA, result)
end

--- Handle DB_SAVE_WORLD: Save full world state to MongoDB.
--- Uses the state mirror's current snapshot.
--- Payload format: JSON {world_id, metadata}
--- @param payload  string  Raw payload
--- @return string  Binary response
function M.handle_save_world(payload)
    local ok, world_info = pcall(json_decode, payload)
    if not ok then
        world_info = { world_id = "default" }
    end

    local state = mirror.get_full_state()
    state.world_id = world_info.world_id or "default"
    state.saved_at = os.time()

    local doc_json = M._encode_json(state)
    if not doc_json then
        return protocol.pack_response(protocol.RESP_ERROR, "JSON encoding failed")
    end

    local js = build_upsert_js("world_states", doc_json, "world_id")
    local result = mongo_exec(js)

    if not result then
        return protocol.pack_response(protocol.RESP_ERROR, "MongoDB save failed")
    end

    return protocol.pack_response(protocol.RESP_OK, result)
end

--- Handle DB_HEARTBEAT: Respond to heartbeat ping.
--- @param payload  string  Raw payload (ignored)
--- @return string  Binary response
function M.handle_heartbeat(payload)
    -- Simple pong: echo back with OK status
    return protocol.pack_response(protocol.RESP_OK, "pong")
end

-- ============================================================
-- Message Dispatch
-- ============================================================

--- Dispatch an incoming DB message to the appropriate handler.
--- @param msg   table  Unpacked message from protocol.unpack_message()
--- @return string  Binary response to send back
function M.dispatch(msg)
    if not msg then
        return protocol.pack_response(protocol.RESP_ERROR, "empty message")
    end

    local msg_type = msg.msg_type
    local payload  = msg.payload or ""

    if msg_type == protocol.MSG_SAVE_PLAYER then
        return M.handle_save_player(payload)
    elseif msg_type == protocol.MSG_LOAD_PLAYER then
        return M.handle_load_player(payload)
    elseif msg_type == protocol.MSG_SAVE_WORLD then
        return M.handle_save_world(payload)
    elseif msg_type == protocol.MSG_HEARTBEAT then
        return M.handle_heartbeat(payload)
    else
        return protocol.pack_response(protocol.RESP_ERROR,
            "unknown message type: " .. tostring(msg_type))
    end
end

-- ============================================================
-- Internal: JSON Helpers
-- ============================================================

--- Decode JSON string to Lua table.
--- Tries dkjson, cjson, then falls back to manual parser.
local function json_decode(str)
    -- Try dkjson
    local ok, dkjson = pcall(require, "dkjson")
    if ok and dkjson then
        return dkjson.decode(str)
    end

    -- Try cjson
    local cjson
    ok, cjson = pcall(require, "cjson")
    if ok and cjson then
        return cjson.decode(str)
    end

    -- Fallback: minimal JSON parser
    return M._minimal_json_parse(str)
end

function M._encode_json(tbl)
    -- Try dkjson first
    local ok, dkjson = pcall(require, "dkjson")
    if ok and dkjson then
        return dkjson.encode(tbl)
    end

    -- Try cjson
    local cjson
    ok, cjson = pcall(require, "cjson")
    if ok and cjson then
        return cjson.encode(tbl)
    end

    -- Fallback: manual encoder
    return M._manual_json_encode(tbl)
end

function M._manual_json_encode(val)
    local t = type(val)
    if t == "nil" then return "null"
    elseif t == "boolean" then return val and "true" or "false"
    elseif t == "number" then return tostring(val)
    elseif t == "string" then
        return string.format("%q", val)
    elseif t == "table" then
        local parts = {}
        for k, v in pairs(val) do
            parts[#parts + 1] = string.format("%q", tostring(k)) .. ":" .. M._manual_json_encode(v)
        end
        return "{" .. table.concat(parts, ",") .. "}"
    end
    return "null"
end

--- Minimal JSON parser for simple objects (fallback).
function M._minimal_json_parse(str)
    -- This is a minimal parser; for production, use dkjson or cjson
    -- Attempt to load as Lua (JSON is nearly valid Lua for simple objects)
    local cleaned = str:gsub("null", "nil"):gsub("true", "true"):gsub("false", "false")
    local fn, err = load("return " .. cleaned)
    if fn then
        local ok, result = pcall(fn)
        if ok then return result end
    end
    return nil
end

return M
