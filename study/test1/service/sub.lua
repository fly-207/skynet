local skynet = require "skynet"



local function startFunction()
    skynet.error("从服务器开始启动")

    skynet.dispatch("lua", function(...)
        print("收到请求", ...)
    end)

    skynet.error("从服务器启动完成")

end

skynet.start(startFunction)
