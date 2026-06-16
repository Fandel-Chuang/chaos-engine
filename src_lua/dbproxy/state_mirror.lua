--[[
    state_mirror.lua — In-Memory Entity State Mirror

    Maintains a real-time mirror of Game A's entity state by applying
    incoming SyncFrames in frame-sequence order.

    Responsibilities:
    - Receive SyncFrame from Game A via sync port (9001)
    - Apply entity updates to in-memory mirror (entity_id → entity_data)
    - Track frame sequence for ordering
    - On Game A heartbeat timeout → serialize mirror to archive
    - Provide get_state(entity_id) query interface for DB operations
]]

-- Try to load a JSON library; fall back to manual encoder if unavailable
local json_ok, json = pcall(require, "dkjson")
if not json_ok then
    json_ok, json = pcall(require, "cjson")
end
if not json_ok then
    json = nil  -- Will use manual encoder
end

local M = {}

-- ============================================================
-- Internal State
-- ============================================================

-- Entity mirror: entity_id → {entity_id, component_type, data, updated_at}
local entity_map = {}

-- Frame tracking
local last_frame_seq = 0
local last_frame_time = 0
local total_frames_received = 0

-- Archive path
local archive_dir = nil

-- ============================================================
-- Initialization
-- ============================================================

--- Initialize the state mirror.
--- @param config  table  DBProxy config (for archive path)
function M.init(config)
    archive_dir = config.archive_dir or "/tmp/chaos_dbproxy/archive"
    -- Ensure archive directory exists
    os.execute("mkdir -p " .. archive_dir)
    M.reset()
end

--- Reset the mirror to empty state.
function M.reset()
    entity_map = {}
    last_frame_seq = 0
    last_frame_time = 0
    total_frames_received = 0
end

-- ============================================================
-- Frame Application
-- ============================================================

--- Apply a sync frame to the state mirror.
--- Each entity in the frame updates or inserts into the mirror.
--- @param frame  table  Unpacked frame from protocol.unpack_frame()
--- @return number  Number of entities updated
function M.apply_frame(frame)
    if not frame or not frame.entities then
        return 0
    end

    local updated = 0
    local now = os.time()

    for _, entity in ipairs(frame.entities) do
        local eid = entity.entity_id
        entity_map[eid] = {
            entity_id      = entity.entity_id,
            component_type = entity.component_type,
            data           = entity.data,
            updated_at     = now,
        }
        updated = updated + 1
    end

    -- Update frame tracking
    if frame.frame_seq and frame.frame_seq ~= 0xFFFF then
        last_frame_seq = frame.frame_seq
    end
    last_frame_time = now
    total_frames_received = total_frames_received + 1

    return updated
end

-- ============================================================
-- Query Interface
-- ============================================================

--- Get state for a specific entity.
--- @param entity_id  number  Entity ID
--- @return table|nil  Entity data or nil if not found
function M.get_state(entity_id)
    return entity_map[entity_id]
end

--- Get all entity IDs currently in the mirror.
--- @return table  Array of entity IDs
function M.get_all_entity_ids()
    local ids = {}
    for eid, _ in pairs(entity_map) do
        ids[#ids + 1] = eid
    end
    return ids
end

--- Get the full mirror as a serializable table.
--- @return table  {entities = {[entity_id] = entity_data}, ...}
function M.get_full_state()
    return {
        entities            = entity_map,
        last_frame_seq      = last_frame_seq,
        last_frame_time     = last_frame_time,
        total_frames_received = total_frames_received,
        entity_count        = M.get_entity_count(),
    }
end

--- Get number of entities in the mirror.
--- @return number
function M.get_entity_count()
    local count = 0
    for _ in pairs(entity_map) do
        count = count + 1
    end
    return count
end

-- ============================================================
-- Archive / Serialization
-- ============================================================

--- Serialize the full mirror to a JSON archive file.
--- Called when Game A heartbeat times out.
--- @return string|nil  Archive file path, or nil on failure
function M.archive()
    if M.get_entity_count() == 0 then
        return nil, "mirror is empty, nothing to archive"
    end

    local timestamp = os.date("%Y%m%d_%H%M%S")
    local filename = string.format("%s/state_%s.json", archive_dir, timestamp)

    -- Build serializable state
    local state = M.get_full_state()
    state.archived_at = os.time()

    -- Serialize entities as array for JSON
    local entity_array = {}
    for eid, ent in pairs(state.entities) do
        -- Convert binary data to base64 for JSON safety
        local data_b64 = ""
        if ent.data and #ent.data > 0 then
            -- Simple hex encoding for binary data in JSON
            data_b64 = ent.data:gsub(".", function(c)
                return string.format("%02x", string.byte(c))
            end)
        end
        entity_array[#entity_array + 1] = {
            entity_id      = ent.entity_id,
            component_type = ent.component_type,
            data_hex       = data_b64,
            updated_at     = ent.updated_at,
        }
    end
    state.entities = entity_array

    -- Write JSON file
    local json_str = M._encode_json(state)
    if not json_str then
        return nil, "JSON encoding failed"
    end

    local file, err = io.open(filename, "w")
    if not file then
        return nil, "cannot open archive file: " .. tostring(err)
    end
    file:write(json_str)
    file:close()

    return filename
end

-- ============================================================
-- Stats
-- ============================================================

--- Get mirror statistics.
--- @return table
function M.stats()
    return {
        entity_count          = M.get_entity_count(),
        last_frame_seq        = last_frame_seq,
        last_frame_time       = last_frame_time,
        total_frames_received = total_frames_received,
    }
end

-- ============================================================
-- Internal: JSON Encoding (fallback if dkjson unavailable)
-- ============================================================

function M._encode_json(tbl)
    -- Use already-loaded JSON library if available
    if json then
        local ok, result = pcall(json.encode, json, tbl)
        if ok then return result end
    end

    -- Fallback: manual JSON encoder (handles basic types)
    return M._manual_json_encode(tbl)
end

function M._manual_json_encode(val)
    local t = type(val)
    if t == "nil" then
        return "null"
    elseif t == "boolean" then
        return val and "true" or "false"
    elseif t == "number" then
        return tostring(val)
    elseif t == "string" then
        return string.format("%q", val):gsub("\\\n", "\\n")
    elseif t == "table" then
        -- Check if array
        local is_array = true
        local max_idx = 0
        for k in pairs(val) do
            if type(k) ~= "number" or k < 1 or math.floor(k) ~= k then
                is_array = false
                break
            end
            if k > max_idx then max_idx = k end
        end
        if is_array and max_idx > 0 then
            local parts = {}
            for i = 1, max_idx do
                parts[i] = M._manual_json_encode(val[i])
            end
            return "[" .. table.concat(parts, ",") .. "]"
        else
            local parts = {}
            for k, v in pairs(val) do
                parts[#parts + 1] = string.format("%q", tostring(k)) .. ":" .. M._manual_json_encode(v)
            end
            return "{" .. table.concat(parts, ",") .. "}"
        end
    else
        return "null"
    end
end

return M
