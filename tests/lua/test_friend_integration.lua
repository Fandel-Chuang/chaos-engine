#!/usr/bin/env lua
--[[
    test_friend_integration.lua — Friend Service Integration Tests

    Tests the friend service business logic without requiring
    a running Router or DBProxy. Tests cover:
    - add_friend / remove_friend
    - get_friend_list / get_online_friends
    - is_friend
    - set_online_status / get_online_status
    - add_blacklist / remove_blacklist / get_blacklist
    - Edge cases: self-friend, duplicates, limits, blocked interactions
    - Statistics
    - Config validation

    Usage:
        lua test_friend_integration.lua
]]

-- ============================================================
-- Bootstrap: Setup package path
-- ============================================================

-- Add project src_lua to package.path
local project_root = "/home/zhongfangdao/chaos-engine"
package.path = project_root .. "/src_lua/?.lua;" .. project_root .. "/src_lua/?/init.lua;" .. package.path

-- ============================================================
-- Test Framework
-- ============================================================

local tests_passed = 0
local tests_failed = 0
local tests_total = 0

local function assert_equal(actual, expected, msg)
    tests_total = tests_total + 1
    if actual == expected then
        tests_passed = tests_passed + 1
        print(string.format("  PASS: %s", msg))
    else
        tests_failed = tests_failed + 1
        print(string.format("  FAIL: %s", msg))
        print(string.format("    expected: %s", tostring(expected)))
        print(string.format("    actual:   %s", tostring(actual)))
    end
end

local function assert_true(value, msg)
    assert_equal(value, true, msg)
end

local function assert_false(value, msg)
    assert_equal(value, false, msg)
end

local function assert_table_contains(tbl, value, msg)
    tests_total = tests_total + 1
    local found = false
    for _, v in ipairs(tbl) do
        if v == value then
            found = true
            break
        end
    end
    if found then
        tests_passed = tests_passed + 1
        print(string.format("  PASS: %s", msg))
    else
        tests_failed = tests_failed + 1
        print(string.format("  FAIL: %s", msg))
        print(string.format("    table does not contain: %s", tostring(value)))
    end
end

local function assert_table_length(tbl, expected_len, msg)
    tests_total = tests_total + 1
    local len = #tbl
    if len == expected_len then
        tests_passed = tests_passed + 1
        print(string.format("  PASS: %s", msg))
    else
        tests_failed = tests_failed + 1
        print(string.format("  FAIL: %s", msg))
        print(string.format("    expected length: %d, actual: %d", expected_len, len))
    end
end

-- ============================================================
-- Imports
-- ============================================================

local service_types = require("services.service_types")
local config = require("services.config_friend")
local friend = require("services.friend")

-- ============================================================
-- Test: service_types.lua
-- ============================================================

print("\n=== Test: service_types.lua ===")

local function test_service_types()
    -- Verify all 8 service types are defined
    assert_equal(service_types.SERVICE_AUTH,   0x01, "SERVICE_AUTH = 0x01")
    assert_equal(service_types.SERVICE_FRIEND, 0x02, "SERVICE_FRIEND = 0x02")
    assert_equal(service_types.SERVICE_CHAT,   0x03, "SERVICE_CHAT = 0x03")
    assert_equal(service_types.SERVICE_MAIL,   0x04, "SERVICE_MAIL = 0x04")
    assert_equal(service_types.SERVICE_GUILD,  0x05, "SERVICE_GUILD = 0x05")
    assert_equal(service_types.SERVICE_MATCH,  0x06, "SERVICE_MATCH = 0x06")
    assert_equal(service_types.SERVICE_SHOP,   0x07, "SERVICE_SHOP = 0x07")
    assert_equal(service_types.SERVICE_SCENE,  0x08, "SERVICE_SCENE = 0x08")

    -- Verify service names
    assert_equal(service_types.get_service_name(0x01), "auth",   "get_service_name(AUTH)")
    assert_equal(service_types.get_service_name(0x02), "friend", "get_service_name(FRIEND)")
    assert_equal(service_types.get_service_name(0x08), "scene",  "get_service_name(SCENE)")
    assert_equal(service_types.get_service_name(0xFF), nil,      "get_service_name(invalid) = nil")

    -- Verify service ports
    assert_equal(service_types.get_service_port(0x01), 9010, "get_service_port(AUTH)")
    assert_equal(service_types.get_service_port(0x02), 9011, "get_service_port(FRIEND)")
    assert_equal(service_types.get_service_port(0x08), 9017, "get_service_port(SCENE)")

    -- Verify validation
    assert_true(service_types.is_valid_service(0x01), "is_valid_service(AUTH)")
    assert_true(service_types.is_valid_service(0x08), "is_valid_service(SCENE)")
    assert_false(service_types.is_valid_service(0x00), "is_valid_service(0x00)")
    assert_false(service_types.is_valid_service(0xFF), "is_valid_service(0xFF)")

    -- Verify message type constants
    assert_equal(service_types.MSG_SERVICE_REGISTER,       0x2000, "MSG_SERVICE_REGISTER")
    assert_equal(service_types.MSG_SERVICE_REGISTER_RESP,  0x2001, "MSG_SERVICE_REGISTER_RESP")
    assert_equal(service_types.MSG_SERVICE_HEARTBEAT,      0x2002, "MSG_SERVICE_HEARTBEAT")
    assert_equal(service_types.MSG_SERVICE_HEARTBEAT_RESP, 0x2003, "MSG_SERVICE_HEARTBEAT_RESP")
    assert_equal(service_types.MSG_SERVICE_DEREGISTER,     0x2004, "MSG_SERVICE_DEREGISTER")
    assert_equal(service_types.MSG_SERVICE_RPC_CALL,       0x2100, "MSG_SERVICE_RPC_CALL")
    assert_equal(service_types.MSG_SERVICE_RPC_RESP,       0x2101, "MSG_SERVICE_RPC_RESP")

    -- Verify service status enum
    assert_equal(service_types.SERVICE_STATUS.UNREGISTERED, 0, "SERVICE_STATUS.UNREGISTERED")
    assert_equal(service_types.SERVICE_STATUS.REGISTERING,  1, "SERVICE_STATUS.REGISTERING")
    assert_equal(service_types.SERVICE_STATUS.REGISTERED,   2, "SERVICE_STATUS.REGISTERED")
    assert_equal(service_types.SERVICE_STATUS.DEREGISTERING, 3, "SERVICE_STATUS.DEREGISTERING")
    assert_equal(service_types.SERVICE_STATUS.ERROR,        4, "SERVICE_STATUS.ERROR")
end

test_service_types()

-- ============================================================
-- Test: config_friend.lua
-- ============================================================

print("\n=== Test: config_friend.lua ===")

local function test_config()
    assert_equal(config.service_type, 0x02, "config.service_type = SERVICE_FRIEND")
    assert_equal(config.service_name, "friend", "config.service_name = 'friend'")
    assert_equal(config.router_host, "127.0.0.1", "config.router_host")
    assert_equal(config.router_port, 9000, "config.router_port")
    assert_equal(config.dbproxy_host, "127.0.0.1", "config.dbproxy_host")
    assert_equal(config.dbproxy_port, 9013, "config.dbproxy_port = 9013 (DBProxy integration)")
    assert_equal(config.max_friends, 200, "config.max_friends = 200")
    assert_equal(config.max_blacklist, 100, "config.max_blacklist = 100")
    assert_equal(config.heartbeat_interval, 5.0, "config.heartbeat_interval")
    assert_equal(config.heartbeat_timeout, 15.0, "config.heartbeat_timeout")
    assert_equal(config.version, "0.1.0", "config.version")
end

test_config()

-- ============================================================
-- Test: friend.lua — Basic Operations
-- ============================================================

print("\n=== Test: friend.lua — Basic Operations ===")

local function test_basic_operations()
    friend.reset()

    -- Test add_friend
    local ok, err = friend.add_friend("player1", "player2")
    assert_true(ok, "add_friend(player1, player2)")
    assert_equal(err, nil, "add_friend no error")

    -- Test is_friend
    assert_true(friend.is_friend("player1", "player2"), "is_friend(player1, player2)")
    assert_true(friend.is_friend("player2", "player1"), "is_friend(player2, player1) — bidirectional")
    assert_false(friend.is_friend("player1", "player3"), "is_friend(player1, player3) — not friends")

    -- Test duplicate add
    ok, err = friend.add_friend("player1", "player2")
    assert_false(ok, "add_friend duplicate should fail")
    assert_equal(err, "already friends", "duplicate error message")

    -- Test self-friend
    ok, err = friend.add_friend("player1", "player1")
    assert_false(ok, "add_friend self should fail")
    assert_equal(err, "cannot add yourself as friend", "self-friend error message")

    -- Test get_friend_list
    local list = friend.get_friend_list("player1")
    assert_table_length(list, 1, "get_friend_list(player1) length = 1")
    assert_equal(list[1].friend_id, "player2", "friend list contains player2")
    assert_false(list[1].online, "player2 is offline by default")

    -- Test remove_friend
    ok, err = friend.remove_friend("player1", "player2")
    assert_true(ok, "remove_friend(player1, player2)")
    assert_false(friend.is_friend("player1", "player2"), "not friends after removal")
    assert_false(friend.is_friend("player2", "player1"), "bidirectional removal")

    -- Test remove non-friend
    ok, err = friend.remove_friend("player1", "player3")
    assert_false(ok, "remove_friend non-friend should fail")
    assert_equal(err, "not friends", "non-friend error message")
end

test_basic_operations()

-- ============================================================
-- Test: friend.lua — Online Status
-- ============================================================

print("\n=== Test: friend.lua — Online Status ===")

local function test_online_status()
    friend.reset()

    -- Add some friends
    friend.add_friend("player1", "player2")
    friend.add_friend("player1", "player3")
    friend.add_friend("player1", "player4")

    -- Set online status
    friend.set_online_status("player2", true)
    friend.set_online_status("player3", true)
    friend.set_online_status("player4", false)

    -- Test get_online_status
    assert_true(friend.get_online_status("player2"), "player2 is online")
    assert_true(friend.get_online_status("player3"), "player3 is online")
    assert_false(friend.get_online_status("player4"), "player4 is offline")
    assert_false(friend.get_online_status("player99"), "unknown player is offline")

    -- Test get_online_friends
    local online = friend.get_online_friends("player1")
    assert_table_length(online, 2, "get_online_friends(player1) length = 2")
    assert_table_contains(online, "player2", "online friends contains player2")
    assert_table_contains(online, "player3", "online friends contains player3")

    -- Test set_online_status toggle
    friend.set_online_status("player2", false)
    assert_false(friend.get_online_status("player2"), "player2 toggled offline")
    online = friend.get_online_friends("player1")
    assert_table_length(online, 1, "online friends reduced to 1")
end

test_online_status()

-- ============================================================
-- Test: friend.lua — Blacklist
-- ============================================================

print("\n=== Test: friend.lua — Blacklist ===")

local function test_blacklist()
    friend.reset()

    -- Test add_blacklist
    local ok, err = friend.add_blacklist("player1", "player2")
    assert_true(ok, "add_blacklist(player1, player2)")

    -- Test duplicate blacklist
    ok, err = friend.add_blacklist("player1", "player2")
    assert_false(ok, "add_blacklist duplicate should fail")
    assert_equal(err, "already blocked", "duplicate blacklist error")

    -- Test self-blacklist
    ok, err = friend.add_blacklist("player1", "player1")
    assert_false(ok, "add_blacklist self should fail")
    assert_equal(err, "cannot block yourself", "self-blacklist error")

    -- Test get_blacklist
    local blacklist = friend.get_blacklist("player1")
    assert_table_length(blacklist, 1, "get_blacklist(player1) length = 1")
    assert_table_contains(blacklist, "player2", "blacklist contains player2")

    -- Test blocked player cannot be friended
    ok, err = friend.add_friend("player1", "player2")
    assert_false(ok, "cannot friend blocked player")
    assert_equal(err, "player is blocked", "blocked friend error")

    -- Test blocked by target
    friend.add_blacklist("player3", "player1")
    ok, err = friend.add_friend("player1", "player3")
    assert_false(ok, "cannot friend player who blocked you")
    assert_equal(err, "blocked by target player", "blocked by target error")

    -- Test remove_blacklist
    ok, err = friend.remove_blacklist("player1", "player2")
    assert_true(ok, "remove_blacklist(player1, player2)")

    -- Test remove non-blocked
    ok, err = friend.remove_blacklist("player1", "player99")
    assert_false(ok, "remove_blacklist non-blocked should fail")
    assert_equal(err, "not blocked", "non-blocked error")

    -- After unblocking, can friend again
    ok, err = friend.add_friend("player1", "player2")
    assert_true(ok, "can friend after unblocking")
end

test_blacklist()

-- ============================================================
-- Test: friend.lua — Blacklist Auto-Remove Friendship
-- ============================================================

print("\n=== Test: friend.lua — Blacklist Auto-Remove Friendship ===")

local function test_blacklist_auto_remove()
    friend.reset()

    -- First make them friends
    friend.add_friend("player1", "player2")
    assert_true(friend.is_friend("player1", "player2"), "initially friends")

    -- Then block
    friend.add_blacklist("player1", "player2")
    assert_false(friend.is_friend("player1", "player2"), "friendship removed after block")
    assert_false(friend.is_friend("player2", "player1"), "bidirectional removal after block")
end

test_blacklist_auto_remove()

-- ============================================================
-- Test: friend.lua — Limits
-- ============================================================

print("\n=== Test: friend.lua — Limits ===")

local function test_limits()
    friend.reset()

    -- Temporarily lower limits for testing
    local orig_max_friends = config.max_friends
    local orig_max_blacklist = config.max_blacklist
    config.max_friends = 5
    config.max_blacklist = 3

    -- Test friend limit
    local all_ok = true
    for i = 1, 5 do
        local ok, _ = friend.add_friend("player1", "friend" .. i)
        if not ok then all_ok = false end
    end
    assert_true(all_ok, "added 5 friends (at limit)")

    local ok, err = friend.add_friend("player1", "friend6")
    assert_false(ok, "cannot exceed friend limit")
    assert_equal(err, "friend list full (max 5)", "friend limit error message")

    -- Test blacklist limit
    for i = 1, 3 do
        friend.add_blacklist("player1", "blocked" .. i)
    end
    ok, err = friend.add_blacklist("player1", "blocked4")
    assert_false(ok, "cannot exceed blacklist limit")
    assert_equal(err, "blacklist full (max 3)", "blacklist limit error message")

    -- Restore limits
    config.max_friends = orig_max_friends
    config.max_blacklist = orig_max_blacklist
end

test_limits()

-- ============================================================
-- Test: friend.lua — Statistics
-- ============================================================

print("\n=== Test: friend.lua — Statistics ===")

local function test_statistics()
    friend.reset()

    -- Add some data
    friend.add_friend("p1", "p2")
    friend.add_friend("p1", "p3")
    friend.add_friend("p4", "p5")
    friend.set_online_status("p1", true)
    friend.set_online_status("p2", true)
    friend.set_online_status("p3", false)

    local stats = friend.get_stats()
    assert_equal(stats.total_players, 5, "stats: 5 players")
    assert_equal(stats.total_friendships, 3, "stats: 3 friendships")
    assert_equal(stats.total_online, 2, "stats: 2 online")
end

test_statistics()

-- ============================================================
-- Test: friend.lua — Nil/Edge Cases
-- ============================================================

print("\n=== Test: friend.lua — Nil/Edge Cases ===")

local function test_edge_cases()
    friend.reset()

    -- Nil player_id
    local ok, err = friend.add_friend(nil, "player2")
    assert_false(ok, "add_friend(nil, ...) fails")
    assert_equal(err, "invalid player_id or friend_id", "nil player_id error")

    ok, err = friend.add_friend("player1", nil)
    assert_false(ok, "add_friend(..., nil) fails")

    -- Empty friend list
    local list = friend.get_friend_list("nonexistent")
    assert_table_length(list, 0, "get_friend_list for unknown player = empty")

    -- Empty online friends
    local online = friend.get_online_friends("nonexistent")
    assert_table_length(online, 0, "get_online_friends for unknown player = empty")

    -- Empty blacklist
    local blacklist = friend.get_blacklist("nonexistent")
    assert_table_length(blacklist, 0, "get_blacklist for unknown player = empty")

    -- is_friend with nil
    assert_false(friend.is_friend(nil, "p2"), "is_friend(nil, ...) = false")
    assert_false(friend.is_friend("p1", nil), "is_friend(..., nil) = false")

    -- get_online_status with nil
    assert_false(friend.get_online_status(nil), "get_online_status(nil) = false")
end

test_edge_cases()

-- ============================================================
-- Test: friend.lua — Multiple Players
-- ============================================================

print("\n=== Test: friend.lua — Multiple Players ===")

local function test_multiple_players()
    friend.reset()

    -- Create a social graph
    -- p1 friends: p2, p3, p4
    -- p2 friends: p1, p5
    -- p3 friends: p1
    friend.add_friend("p1", "p2")
    friend.add_friend("p1", "p3")
    friend.add_friend("p1", "p4")
    friend.add_friend("p2", "p5")

    -- Verify each player's list
    assert_table_length(friend.get_friend_list("p1"), 3, "p1 has 3 friends")
    assert_table_length(friend.get_friend_list("p2"), 2, "p2 has 2 friends")
    assert_table_length(friend.get_friend_list("p3"), 1, "p3 has 1 friend")
    assert_table_length(friend.get_friend_list("p4"), 1, "p4 has 1 friend")
    assert_table_length(friend.get_friend_list("p5"), 1, "p5 has 1 friend")

    -- Remove p1-p2 friendship
    friend.remove_friend("p1", "p2")
    assert_table_length(friend.get_friend_list("p1"), 2, "p1 has 2 friends after removal")
    assert_table_length(friend.get_friend_list("p2"), 1, "p2 has 1 friend after removal")
    assert_true(friend.is_friend("p2", "p5"), "p2-p5 still friends")
end

test_multiple_players()

-- ============================================================
-- Summary
-- ============================================================

print("\n========================================")
print(string.format("Tests: %d total, %d passed, %d failed",
    tests_total, tests_passed, tests_failed))
print("========================================")

if tests_failed > 0 then
    os.exit(1)
else
    os.exit(0)
end
