local skynet = require "skynet"
require "skynet.manager"

local i = 0
local serviceHandle1 = nil

print("AAA自定义变量", AAA)

local function timeout()
    skynet.send(serviceHandle1, "lua", "发送消息内容="..i)
    i= i + 1
    skynet.timeout(10, timeout)
end

local function startFunction()
    skynet.error("主服务, 开始执行")
    serviceHandle1 = skynet.newservice("sub")

    --skynet.timeout(10, timeout)
    --skynet.newservice("debug_console",8000)

    skynet.error("主服务, 准备退出")
end

skynet.start(startFunction)

print("test 顶层代码执行完成")