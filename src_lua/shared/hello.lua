-- ChaosEngine Lua 示例脚本
-- 演示如何使用引擎绑定的 Lua API

print("Hello from ChaosEngine Lua!")

-- 创建一个实体
local e = entity_create()
log_info("Created entity: " .. e)

-- 检查实体是否存活
if entity_is_alive(e) then
    log_info("Entity " .. e .. " is alive!")
end

-- 获取 delta time
local dt = get_delta_time()
log_info("Delta time: " .. dt)

-- 获取总运行时间
local total = get_total_time()
log_info("Total time: " .. total)

-- 添加组件
local ok = entity_add_component(e, "Transform")
if ok then
    log_info("Added Transform component to entity " .. e)
end

-- 销毁实体
entity_destroy(e)
log_info("Entity " .. e .. " destroyed")
