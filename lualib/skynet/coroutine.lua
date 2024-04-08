-- 在skynet框架中，应该使用这个模块（skynet.coroutine）代替原始的lua协程

local coroutine = coroutine
-- 原始的lua协程模块
local coroutine_resume = coroutine.resume
local coroutine_yield = coroutine.yield
local coroutine_status = coroutine.status
local coroutine_running = coroutine.running
local coroutine_close = coroutine.close

local select = select
local skynetco = {}

-- 直接从lua协程模块中导出的函数
skynetco.isyieldable = coroutine.isyieldable
skynetco.running = coroutine.running
skynetco.status = coroutine.status

-- 存储所有skynet协程的状态
local skynet_coroutines = setmetatable({}, { __mode = "kv" })
-- true: skynet协程
-- false: 由skynet框架挂起的协程
-- nil: 协程退出

-- 创建一个新的skynet协程
function skynetco.create(f)
	local co = coroutine.create(f)
	-- 将新创建的协程标记为skynet协程
	skynet_coroutines[co] = true
	return co
end

-- skynetco.resume块：处理协程的恢复逻辑
do 
	local function unlock(co, ...)
		skynet_coroutines[co] = true
		return ...
	end

	local function skynet_yielding(co, ...)
		skynet_coroutines[co] = false
		return unlock(co, coroutine_resume(co, coroutine_yield(...)))
	end

	local function resume(co, ok, ...)
		if not ok then
			return ok, ...
		elseif coroutine_status(co) == "dead" then
			-- 主函数退出
			skynet_coroutines[co] = nil
			return true, ...
		elseif (...) == "USER" then
			return true, select(2, ...)
		else
			-- 在skynet框架内被阻塞，因此引发挂起的消息
			return resume(co, skynet_yielding(co, ...))
		end
	end

	-- 记录协程调用者的根（应该是skynet的线程）
	local coroutine_caller = setmetatable({} , { __mode = "kv" })

	-- 恢复一个skynet协程的执行
	function skynetco.resume(co, ...)
		local co_status = skynet_coroutines[co]
		if not co_status then
			if co_status == false then
				-- 由skynet框架挂起，无法恢复
				return false, "cannot resume a skynet coroutine suspend by skynet framework"
			end
			if coroutine_status(co) == "dead" then
				-- 协程已死，无法恢复
				return coroutine_resume(co, ...)
			else
				-- 非skynet协程，无法恢复
				return false, "cannot resume none skynet coroutine"
			end
		end
		local from = coroutine_running()
		local caller = coroutine_caller[from] or from
		coroutine_caller[co] = caller
		return resume(co, coroutine_resume(co, ...))
	end

	-- 获取协程的线程信息
	function skynetco.thread(co)
		co = co or coroutine_running()
		if skynet_coroutines[co] ~= nil then
			return coroutine_caller[co] , false
		else
			return co, true
		end
	end

end -- 结束skynetco.resume块

-- 获取协程的状态，特别处理了被skynet挂起的状态
function skynetco.status(co)
	local status = coroutine_status(co)
	if status == "suspended" then
		if skynet_coroutines[co] == false then
			return "blocked"
		else
			return "suspended"
		end
	else
		return status
	end
end

-- 用于协程内主动挂起
function skynetco.yield(...)
	return coroutine_yield("USER", ...)
end

-- skynetco.wrap块：封装协程函数，使其易于调用
do 

	local function wrap_co(ok, ...)
		if ok then
			return ...
		else
			error(...)
		end
	end

	-- 封装一个函数，使其在协程中运行
	function skynetco.wrap(f)
		local co = skynetco.create(function(...)
			return f(...)
		end)
		return function(...)
			return wrap_co(skynetco.resume(co, ...))
		end
	end

end	-- 结束skynetco.wrap块

-- 关闭一个协程
function skynetco.close(co)
	skynet_coroutines[co] = nil
	return coroutine_close(co)
end

return skynetco