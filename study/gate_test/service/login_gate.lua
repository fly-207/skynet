

local skynet  = require "skynet"
local gateserver = require "snax.gateserver"
local netpack = require "skynet.netpack"

local handler = {}
local CMD = {}

function handler.connect(fd, ipaddr)
    skynet.error("新连接到达 ipaddr:", ipaddr, "fd:", fd)
    gateserver.openclient(fd)
end


function handler.disconnect(fd)
    skynet.error("连接断开:", fd)
end

function handler.message(fd, msg, sz)
    skynet.error("接受新消息:", fd, "长度:", sz, "内容:", netpack.tostring(msg, sz))
end

function CMD.kick(arg1, arg2)
    skynet.error("收到命令 kick", arg1, arg2)
end

function handler.command(cmd, source, ...)
    local f = assert(CMD[cmd])

    return f(...)
end

gateserver.start(handler)

skynet.register_protocol({
    name="client",
    id=skynet.PTYPE_CLIENT,
    -- unpack=netpack.tostring,
    dispatch = function(_, _, cmd)
        skynet.error("收到 client 消息", cmd)
    end
})