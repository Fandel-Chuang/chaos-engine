--[[
    friend.lua — Friend Service Business Logic

    Provides friend system operations:
    - add_friend(player_id, friend_id)     — Send/accept friend request
    - remove_friend(player_id, friend_id)  — Remove a friend
    - get_friend_list(player_id)           — Get friend list
    - get_online_friends(player_id)        — Get online friends
    - is_friend(player_id, friend_id)      — Check friendship
    - set_online_status(player_id, status) — Update online status
    - add_blacklist(player_id, blocked_id) — Block a player
    - remove_blacklist(player_id, blocked_id) — Unblock a player

    This module uses ce_rpc_call() for inter-service communication
    and DBProxy for friend data persistence.
]]

local service_types = require("services.service_types")
local config = require("services.config_friend")

local M = {}

-- ============================================================
-- Internal State
-- ============================================================

-- In-memory friend data (in production, this would be backed by DBProxy)
-- Structure: { [player_id] = { friends = { friend_id = { online = bool, added_at = ts }, ... }, blacklist = { blocked_id = true, ... } } }
local friend_data = {}

-- Online status tracking
-- Structure: { [player_id] = true/false }
local online_status = {}

-- ============================================================
-- Internal Helpers
-- ============================================================

--- Get or create friend data for a player.
--- @param player_id string|number  Player identifier
--- @return table  Friend data table
local function get_or_create_player_data(player_id)
    local key = tostring(player_id)
    if not friend_data[key] then
        friend_data[key] = {
            friends = {},
            blacklist = {},
        }
    end
    return friend_data[key]
end

--- Check if a player has reached the max friends limit.
--- @param player_id string|number  Player identifier
--- @return boolean  True if at limit
local function is_friend_limit_reached(player_id)
    local data = get_or_create_player_data(player_id)
    local count = 0
    for _ in pairs(data.friends) do
        count = count + 1
    end
    return count >= config.max_friends
end

--- Check if a player has reached the max blacklist limit.
--- @param player_id string|number  Player identifier
--- @return boolean  True if at limit
local function is_blacklist_limit_reached(player_id)
    local data = get_or_create_player_data(player_id)
    local count = 0
    for _ in pairs(data.blacklist) do
        count = count + 1
    end
    return count >= config.max_blacklist
end

-- ============================================================
-- Friend Operations
-- ============================================================

--- Add a friend relationship between two players.
--- This creates a bidirectional friendship.
--- @param player_id  string|number  The player initiating the request
--- @param friend_id  string|number  The target friend
--- @return boolean   True if successful
--- @return string    Error message if failed
function M.add_friend(player_id, friend_id)
    if not player_id or not friend_id then
        return false, "invalid player_id or friend_id"
    end

    local p_key = tostring(player_id)
    local f_key = tostring(friend_id)

    if p_key == f_key then
        return false, "cannot add yourself as friend"
    end

    -- Check limits
    if is_friend_limit_reached(p_key) then
        return false, "friend list full (max " .. config.max_friends .. ")"
    end
    if is_friend_limit_reached(f_key) then
        return false, "target friend list full"
    end

    -- Check blacklist
    local p_data = get_or_create_player_data(p_key)
    if p_data.blacklist[f_key] then
        return false, "player is blocked"
    end

    local f_data = get_or_create_player_data(f_key)
    if f_data.blacklist[p_key] then
        return false, "blocked by target player"
    end

    -- Check if already friends
    if p_data.friends[f_key] then
        return false, "already friends"
    end

    -- Create bidirectional friendship
    local now = os.time()
    p_data.friends[f_key] = {
        added_at = now,
    }
    f_data.friends[p_key] = {
        added_at = now,
    }

    return true, nil
end

--- Remove a friend relationship.
--- @param player_id  string|number  The player removing the friend
--- @param friend_id  string|number  The friend to remove
--- @return boolean   True if successful
--- @return string    Error message if failed
function M.remove_friend(player_id, friend_id)
    if not player_id or not friend_id then
        return false, "invalid player_id or friend_id"
    end

    local p_key = tostring(player_id)
    local f_key = tostring(friend_id)

    local p_data = get_or_create_player_data(p_key)
    local f_data = get_or_create_player_data(f_key)

    if not p_data.friends[f_key] then
        return false, "not friends"
    end

    -- Remove bidirectional friendship
    p_data.friends[f_key] = nil
    f_data.friends[p_key] = nil

    return true, nil
end

--- Get a player's friend list.
--- @param player_id  string|number  Player identifier
--- @return table     Array of { friend_id, online, added_at }
function M.get_friend_list(player_id)
    if not player_id then
        return {}
    end

    local p_key = tostring(player_id)
    local data = get_or_create_player_data(p_key)

    local result = {}
    for friend_id, info in pairs(data.friends) do
        result[#result + 1] = {
            friend_id = friend_id,
            online = online_status[friend_id] or false,
            added_at = info.added_at,
        }
    end

    -- Sort by added_at (newest first)
    table.sort(result, function(a, b)
        return (a.added_at or 0) > (b.added_at or 0)
    end)

    return result
end

--- Get a player's online friends.
--- @param player_id  string|number  Player identifier
--- @return table     Array of friend_id strings
function M.get_online_friends(player_id)
    local all_friends = M.get_friend_list(player_id)
    local online = {}
    for _, friend in ipairs(all_friends) do
        if friend.online then
            online[#online + 1] = friend.friend_id
        end
    end
    return online
end

--- Check if two players are friends.
--- @param player_id  string|number  Player identifier
--- @param friend_id  string|number  Potential friend
--- @return boolean   True if they are friends
function M.is_friend(player_id, friend_id)
    if not player_id or not friend_id then
        return false
    end

    local p_key = tostring(player_id)
    local f_key = tostring(friend_id)

    local data = get_or_create_player_data(p_key)
    return data.friends[f_key] ~= nil
end

-- ============================================================
-- Online Status
-- ============================================================

--- Set a player's online status.
--- @param player_id  string|number  Player identifier
--- @param status     boolean       True if online, false if offline
function M.set_online_status(player_id, status)
    if not player_id then return end
    local key = tostring(player_id)
    online_status[key] = status and true or false
end

--- Get a player's online status.
--- @param player_id  string|number  Player identifier
--- @return boolean   True if online
function M.get_online_status(player_id)
    if not player_id then return false end
    return online_status[tostring(player_id)] or false
end

-- ============================================================
-- Blacklist Operations
-- ============================================================

--- Add a player to the blacklist.
--- @param player_id   string|number  The player adding to blacklist
--- @param blocked_id  string|number  The player to block
--- @return boolean    True if successful
--- @return string     Error message if failed
function M.add_blacklist(player_id, blocked_id)
    if not player_id or not blocked_id then
        return false, "invalid player_id or blocked_id"
    end

    local p_key = tostring(player_id)
    local b_key = tostring(blocked_id)

    if p_key == b_key then
        return false, "cannot block yourself"
    end

    if is_blacklist_limit_reached(p_key) then
        return false, "blacklist full (max " .. config.max_blacklist .. ")"
    end

    local data = get_or_create_player_data(p_key)

    if data.blacklist[b_key] then
        return false, "already blocked"
    end

    data.blacklist[b_key] = true

    -- If they were friends, remove the friendship
    if data.friends[b_key] then
        M.remove_friend(player_id, blocked_id)
    end

    return true, nil
end

--- Remove a player from the blacklist.
--- @param player_id   string|number  The player removing from blacklist
--- @param blocked_id  string|number  The player to unblock
--- @return boolean    True if successful
--- @return string     Error message if failed
function M.remove_blacklist(player_id, blocked_id)
    if not player_id or not blocked_id then
        return false, "invalid player_id or blocked_id"
    end

    local p_key = tostring(player_id)
    local b_key = tostring(blocked_id)

    local data = get_or_create_player_data(p_key)

    if not data.blacklist[b_key] then
        return false, "not blocked"
    end

    data.blacklist[b_key] = nil
    return true, nil
end

--- Get a player's blacklist.
--- @param player_id  string|number  Player identifier
--- @return table     Array of blocked_id strings
function M.get_blacklist(player_id)
    if not player_id then
        return {}
    end

    local p_key = tostring(player_id)
    local data = get_or_create_player_data(p_key)

    local result = {}
    for blocked_id, _ in pairs(data.blacklist) do
        result[#result + 1] = blocked_id
    end

    return result
end

-- ============================================================
-- Statistics
-- ============================================================

--- Get friend service statistics.
--- @return table  Stats table
function M.get_stats()
    local total_players = 0
    local total_friendships = 0
    local total_online = 0

    for _, data in pairs(friend_data) do
        total_players = total_players + 1
        for _ in pairs(data.friends) do
            total_friendships = total_friendships + 1
        end
    end

    for _, online in pairs(online_status) do
        if online then
            total_online = total_online + 1
        end
    end

    return {
        total_players = total_players,
        total_friendships = total_friendships / 2,  -- bidirectional, so divide by 2
        total_online = total_online,
    }
end

-- ============================================================
-- Reset (for testing)
-- ============================================================

--- Reset all friend data (for testing purposes).
function M.reset()
    friend_data = {}
    online_status = {}
end

return M
