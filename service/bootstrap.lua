-- 主程序入口
--
-- 此函数启动Skynet服务，并根据环境变量配置启动不同的服务。根据是否是独立模式（standalone）以及是否配置了harbor，决定启动的服务类型。
-- 同时，根据是否启用了SSL，会加载相应的ltls服务。

local service = require "skynet.service"
local skynet = require "skynet.manager"	-- 导入skynet.launch, ...

skynet.start(function()
	local standalone = skynet.getenv "standalone" -- 尝试从环境变量获取是否为独立模式

	-- 启动Skynet服务 launcher
	local launcher = assert(skynet.launch("snlua","launcher"))
	skynet.name(".launcher", launcher)

	-- 获取并处理harbor配置
	local harbor_id = tonumber(skynet.getenv "harbor" or 0)
	if harbor_id == 0 then
		-- 如果未配置harbor，则确保处于独立模式
		assert(standalone ==  nil)
		standalone = true
		skynet.setenv("standalone", "true")

		-- 在独立模式下启动cdummy服务
		local ok, slave = pcall(skynet.newservice, "cdummy")
		if not ok then
			skynet.abort() -- 启动失败，终止服务
		end
		skynet.name(".cslave", slave)

	else
		-- 配置了harbor时的处理逻辑
		if standalone then
			-- 在独立模式下尝试启动cmaster服务
			if not pcall(skynet.newservice,"cmaster") then
				skynet.abort() -- 启动失败，终止服务
			end
		end

		-- 启动cslave服务
		local ok, slave = pcall(skynet.newservice, "cslave")
		if not ok then
			skynet.abort() -- 启动失败，终止服务
		end
		skynet.name(".cslave", slave)
	end

	-- 在独立模式下启动datacenterd服务
	if standalone then
		local datacenter = skynet.newservice "datacenterd"
		skynet.name("DATACENTER", datacenter)
	end

	-- 启动service_mgr服务
	skynet.newservice "service_mgr"

	-- 根据环境变量决定是否启用SSL
	local enablessl = skynet.getenv "enablessl"
	if enablessl == "true" then
		-- 启用SSL时，加载ltls_holder服务
		service.new("ltls_holder", function ()
			local c = require "ltls.init.c"
			c.constructor()
		end)
	end

	-- 启动指定的服务，服务名称由环境变量start指定，默认为"main"
	pcall(skynet.newservice,skynet.getenv "start" or "main")
	skynet.exit() -- 退出Skynet主循环
end)