
local skynet = require "skynet"
require "skynet.manager"

local login_gate_service

local agent_balance = 1
local agent_list= {}

local function gate_kick()
    skynet.call(gateserver, "lua", "kick", "arg1实参", skynet.time())
    skynet.timeout(100, gate_kick)
end



skynet.start(function()

    skynet.error("登录服务启动")

    skynet.newservice("debug_console", 8000)
    
    login_gate_service = skynet.newservice "login_gate"
    skynet.name(".login_gate_service")

    for i=1, agent_balance do
        table.insert(agent_list, skynet.newservice("login_gate_agent", login_gate_service))
    end

    skynet.call(login_gate_service, "lua", "open", {
        port=18888,
        maxclient=64,
        nodelay=true,
    })

    -- skynet.timeout(100, gate_kick)

    skynet.error("登录服务 gate 启动完成")
end)