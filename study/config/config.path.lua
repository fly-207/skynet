-- skynet 根目录
root = "/download/skynet/"

-- 命令执行时的目录的 pwd
project_path="/download/skynet/study/"

-- 服务目录
luaservice = root.."service/?.lua;"
            ..project_path.."service/?.lua"

--[[
    每个 lua 服务都会被 loader.lua 加载并执行

    snlua 服务中会定义的常量, 在 c 代码中被定义

    SERVICE_NAME 第一个参数，通常是服务名
    LUA_PATH config 文件中配置的 lua_path
    LUA_CPATH config 文件中配置的 lua_cpath
    LUA_PRELOAD config 文件中配置的 preload
    LUA_SERVICE config 文件中配置的 luaservice
]]
lualoader = root .. "lualib/loader.lua"

-- require 查找目录
lua_path = root.."lualib/?.lua;"
            ..root.."lualib/?/init.lua;"
            ..project_path .. "lib/?.lua;"
            ..project_path .. "global/?.lua"

-- require 查找目录
lua_cpath = root .. "luaclib/?.so;"
            ..project_path .. "clib/?.so"

-- snax类型便捷服务目录
snax = root.."lualib/snax/?.lua;"
        ..project_path .. "snax/?.so"

-- 用 C 编写的服务模块的位置, 通常指 cservice 下那些 .so 文件
cpath = root.."cservice/?.so"