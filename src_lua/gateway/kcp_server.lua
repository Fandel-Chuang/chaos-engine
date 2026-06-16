--[[
    ChaosEngine Gateway — KCP 服务器
    基于 KCP 可靠 UDP 传输协议的服务器实现

    用法:
        local kcp_server = require("gateway.kcp_server")
        local server = kcp_server.new("0.0.0.0", 8888)
        server:start()
]]

local kcp = require("gateway.kcp")

-- ============================================================
-- KCP 服务器
-- ============================================================

local KcpServer = {}
KcpServer.__index = KcpServer

--- 创建新的 KCP 服务器
--- @param host string 监听地址
--- @param port number 监听端口
--- @return KcpServer
function KcpServer.new(host, port)
    local self = setmetatable({}, KcpServer)

    self.host = host or "0.0.0.0"
    self.port = port or 8888
    self.running = false
    self.clients = {}  -- { [conv] = { kcp_ctx, addr, last_active } }
    self.next_conv = 1

    -- 配置参数
    self.config = {
        nodelay  = 1,   -- 启用快速模式
        interval = 10,  -- 内部更新间隔 10ms
        resend   = 2,   -- 启用快速重传
        nc       = 1,   -- 禁用拥塞控制（游戏场景）
        sndwnd   = 128, -- 发送窗口
        rcvwnd   = 128, -- 接收窗口
        mtu      = 512, -- MTU（适合游戏小包）
    }

    return self
end

--- 启动服务器主循环
function KcpServer:start()
    self.running = true
    self:_main_loop()
end

--- 停止服务器
function KcpServer:stop()
    self.running = false
end

-- ============================================================
-- 内部方法
-- ============================================================

--- 主循环
function KcpServer:_main_loop()
    while self.running do
        local current_ms = self:_get_time_ms()

        -- 1. 接收 UDP 数据包
        self:_poll_udp()

        -- 2. 更新所有 KCP 客户端
        self:_update_clients(current_ms)

        -- 3. 处理应用层消息
        self:_process_messages()

        -- 4. 清理超时客户端
        self:_cleanup_timeouts(current_ms)

        -- 5. 睡眠一小段时间（避免忙等）
        self:_sleep_ms(5)
    end
end

--- 获取当前时间戳（毫秒）
function KcpServer:_get_time_ms()
    local socket = require("socket")
    return math.floor(socket.gettime() * 1000)
end

--- 睡眠
function KcpServer:_sleep_ms(ms)
    local socket = require("socket")
    socket.sleep(ms / 1000)
end

--- 轮询 UDP 数据包
function KcpServer:_poll_udp()
    -- 由 C 层网络模块处理 UDP recvfrom
    -- 这里作为 Lua 层的调度入口
    -- 实际实现依赖 gateway.network 模块
end

--- 更新所有 KCP 客户端状态
function KcpServer:_update_clients(current_ms)
    for conv, client in pairs(self.clients) do
        if client.kcp_ctx then
            kcp.update(client.kcp_ctx, current_ms)
        end
    end
end

--- 处理应用层消息
function KcpServer:_process_messages()
    for conv, client in pairs(self.clients) do
        if client.kcp_ctx then
            local buf = kcp.recv(client.kcp_ctx, 65536)
            while buf do
                self:_on_message(conv, buf)
                buf = kcp.recv(client.kcp_ctx, 65536)
            end
        end
    end
end

--- 消息回调（子类可重写）
function KcpServer:_on_message(conv, data)
    -- 默认：回显
    local client = self.clients[conv]
    if client and client.kcp_ctx then
        kcp.send(client.kcp_ctx, data)
    end
end

--- 清理超时客户端
function KcpServer:_cleanup_timeouts(current_ms)
    local timeout_ms = 30000  -- 30 秒超时
    for conv, client in pairs(self.clients) do
        if current_ms - client.last_active > timeout_ms then
            self:_remove_client(conv)
        end
    end
end

-- ============================================================
-- 客户端管理
-- ============================================================

--- 处理新 UDP 数据包（由网络层回调）
--- @param data string 原始 UDP 数据
--- @param addr table 来源地址 { host, port }
function KcpServer:on_udp_packet(data, addr)
    -- 尝试从数据包中提取 conv（KCP 包头前 4 字节）
    local conv = self:_extract_conv(data)
    if not conv then
        return  -- 无效数据包
    end

    local client = self.clients[conv]

    if not client then
        -- 新客户端：创建 KCP 控制块
        client = self:_create_client(conv, addr)
        self.clients[conv] = client
    end

    -- 更新客户端地址（可能变化，NAT 穿透场景）
    client.addr = addr
    client.last_active = self:_get_time_ms()

    -- 输入数据到 KCP
    kcp.input(client.kcp_ctx, data)
end

--- 创建新客户端
function KcpServer:_create_client(conv, addr)
    local ctx = kcp.create(conv, addr)

    -- 配置 KCP 参数
    kcp.set_config(ctx,
        self.config.nodelay,
        self.config.interval,
        self.config.resend,
        self.config.nc
    )
    kcp.set_wndsize(ctx, self.config.sndwnd, self.config.rcvwnd)
    kcp.set_mtu(ctx, self.config.mtu)

    -- 设置输出回调（通过 UDP 发送）
    kcp.set_output(ctx, function(data)
        -- 由 C 层网络模块发送 UDP 数据
        -- 实际实现依赖 gateway.network 模块
        return #data
    end)

    return {
        kcp_ctx     = ctx,
        addr        = addr,
        last_active = self:_get_time_ms(),
    }
end

--- 移除客户端
function KcpServer:_remove_client(conv)
    local client = self.clients[conv]
    if client then
        if client.kcp_ctx then
            kcp.destroy(client.kcp_ctx)
        end
        self.clients[conv] = nil
    end
end

--- 从 KCP 数据包中提取 conv（前 4 字节，小端）
function KcpServer:_extract_conv(data)
    if #data < 4 then
        return nil
    end
    local b1, b2, b3, b4 = string.byte(data, 1, 4)
    return b1 + b2 * 256 + b3 * 65536 + b4 * 16777216
end

-- ============================================================
-- 发送接口
-- ============================================================

--- 向指定客户端发送数据
function KcpServer:send(conv, data)
    local client = self.clients[conv]
    if client and client.kcp_ctx then
        return kcp.send(client.kcp_ctx, data)
    end
    return -1
end

--- 广播数据到所有客户端
function KcpServer:broadcast(data)
    for conv, client in pairs(self.clients) do
        if client.kcp_ctx then
            kcp.send(client.kcp_ctx, data)
        end
    end
end

-- ============================================================
-- 统计信息
-- ============================================================

--- 获取服务器统计
function KcpServer:get_stats()
    local count = 0
    for _ in pairs(self.clients) do
        count = count + 1
    end
    return {
        client_count = count,
        port         = self.port,
        running      = self.running,
    }
end

return KcpServer
