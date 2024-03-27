#include "skynet.h"

#include "skynet_imp.h"
#include "skynet_env.h"
#include "skynet_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <signal.h>
#include <assert.h>

static int
optint(const char *key, int opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		char tmp[20];
		sprintf(tmp,"%d",opt);
		skynet_setenv(key, tmp);
		return opt;
	}
	return strtol(str, NULL, 10);
}

static int
optboolean(const char *key, int opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		skynet_setenv(key, opt ? "true" : "false");
		return opt;
	}
	return strcmp(str,"true")==0;
}

static const char *
optstring(const char *key,const char * opt) {
	const char * str = skynet_getenv(key);
	if (str == NULL) {
		if (opt) {
			skynet_setenv(key, opt);
			opt = skynet_getenv(key);
		}
		return opt;
	}
	return str;
}

static void
_init_env(lua_State *L) {
	lua_pushnil(L);  /* first key */
	while (lua_next(L, -2) != 0) {
		int keyt = lua_type(L, -2);
		if (keyt != LUA_TSTRING) {
			fprintf(stderr, "Invalid config table\n");
			exit(1);
		}
		const char * key = lua_tostring(L,-2);
		if (lua_type(L,-1) == LUA_TBOOLEAN) {
			int b = lua_toboolean(L,-1);
			skynet_setenv(key,b ? "true" : "false" );
		} else {
			const char * value = lua_tostring(L,-1);
			if (value == NULL) {
				fprintf(stderr, "Invalid config table key = %s\n", key);
				exit(1);
			}
			skynet_setenv(key,value);
		}
		lua_pop(L,1);
	}
	lua_pop(L,1);
}

int sigign() {
	struct sigaction sa;
	sa.sa_handler = SIG_IGN;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGPIPE, &sa, 0);
	return 0;
}

static const char * load_config = "\
	local result = {}\n\
	local function getenv(name) return assert(os.getenv(name), [[os.getenv() failed: ]] .. name) end\n\
	local sep = package.config:sub(1,1)\n\
	local current_path = [[.]]..sep\n\
	local function include(filename)\n\
		local last_path = current_path\n\
		local path, name = filename:match([[(.*]]..sep..[[)(.*)$]])\n\
		if path then\n\
			if path:sub(1,1) == sep then	-- root\n\
				current_path = path\n\
			else\n\
				current_path = current_path .. path\n\
			end\n\
		else\n\
			name = filename\n\
		end\n\
		local f = assert(io.open(current_path .. name))\n\
		local code = assert(f:read [[*a]])\n\
		code = string.gsub(code, [[%$([%w_%d]+)]], getenv)\n\
		f:close()\n\
		assert(load(code,[[@]]..filename,[[t]],result))()\n\
		current_path = last_path\n\
	end\n\
	setmetatable(result, { __index = { include = include } })\n\
	local config_name = ...\n\
	include(config_name)\n\
	setmetatable(result, nil)\n\
	return result\n\
";

/*
 * 主函数，负责初始化Skynet服务框架，并根据配置文件启动服务。
 * 参数argc：命令行参数的数量。
 * 参数argv：命令行参数的值。
 * 返回值：程序执行的结果，0代表成功，非0代表失败。
 */
int
main(int argc, char *argv[]) {
	const char * config_file = NULL ; // 配置文件路径初始化为空

	// 检查命令行参数是否提供了配置文件路径
	if (argc > 1) {
		config_file = argv[1]; // 从命令行参数中获取配置文件路径
	} else {
		// 如果没有提供配置文件路径，打印错误信息并退出
		fprintf(stderr, "Need a config file. Please read skynet wiki : https://github.com/cloudwu/skynet/wiki/Config\n"
			"usage: skynet configfilename\n");
		return 1;
	}

	// 全局初始化Skynet框架
	skynet_globalinit();
	// 初始化环境变量
	skynet_env_init();

	// 忽略特定信号
	sigign();

	// 初始化Skynet配置结构体
	struct skynet_config config;

#ifdef LUA_CACHELIB
	// 初始化代码缓存锁
	luaL_initcodecache();
#endif

	// 创建一个新的Lua状态机
	struct lua_State *L = luaL_newstate();
	// 打开Lua标准库
	luaL_openlibs(L);

	// 加载并编译配置加载函数
	int err =  luaL_loadbufferx(L, load_config, strlen(load_config), "=[skynet config]", "t");
	assert(err == LUA_OK);
	// 将配置文件路径压入Lua栈
	lua_pushstring(L, config_file);

	// 调用Lua配置加载函数
	err = lua_pcall(L, 1, 1, 0);
	if (err) {
		// 如果执行出错，打印错误信息并退出
		fprintf(stderr,"%s\n",lua_tostring(L,-1));
		lua_close(L);
		return 1;
	}
	// 初始化环境变量
	_init_env(L);
	// 关闭Lua状态机
	lua_close(L);

	// 从配置文件中读取各项设置到config结构体中
	config.thread =  optint("thread",8);
	config.module_path = optstring("cpath","./cservice/?.so");
	config.harbor = optint("harbor", 1);
	config.bootstrap = optstring("bootstrap","snlua bootstrap");
	config.daemon = optstring("daemon", NULL);
	config.logger = optstring("logger", NULL);
	config.logservice = optstring("logservice", "logger");
	config.profile = optboolean("profile", 1);

	// 启动Skynet服务
	skynet_start(&config);
	// 请求Skynet全局退出
	skynet_globalexit();

	return 0;
}
