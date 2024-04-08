/*
 * 此文件包含 skynet 服务框架的初始化代码。
 * 使用了 LuaJIT、原子操作等技术。
 * 支持在多种操作系统上运行，包括苹果操作系统。
 */

// 包含 skynet 服务的主要接口
#include "skynet.h"
// 包含原子操作的接口
#include "atomic.h"

// Lua 相关的库文件
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

// 断言、字符串处理、内存分配、标准输入输出、时间处理的系统库
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

// 在苹果操作系统上，包含与任务和机器相关的头文件
#if defined(__APPLE__)
#include <mach/task.h>
#include <mach/mach.h>
#endif

// 定义纳秒和微秒的常量
#define NANOSEC 1000000000
#define MICROSEC 1000000

// 可选的调试日志宏定义
// #define DEBUG_LOG

// 内存警告报告的阈值
#define MEMORY_WARNING_REPORT (1024 * 1024 * 32)

/**
 * 结构体snlua用于存储与lua状态机相关的信息以及Skynet上下文的相关配置。
 * 它维护了lua_State的实例，用于Skynet服务的lua脚本执行。
 */
struct snlua
{
	lua_State *L;				// lua_State的指针，代表一个lua虚拟机实例。
	struct skynet_context *ctx; // Skynet上下文的指针，用于与Skynet的服务管理框架交互。
	size_t mem;					// 当前lua虚拟机使用的内存大小。
	size_t mem_report;			// 上一次报告内存使用情况时的内存大小。
	size_t mem_limit;			// 设置的内存使用上限。
	lua_State *activeL;			// 当前活跃的lua_State，用于管理多个lua状态机。
	ATOM_INT trap;				// 用于标记是否捕获到异常或错误。
};

// 当LUA_CACHELIB被定义时，表示使用patched lua进行共享proto的缓存
#ifdef LUA_CACHELIB

// 定义codecache为luaopen_cache，用于打开缓存库
#define codecache luaopen_cache

#else

// 清除操作的空函数，不执行任何操作
static int
cleardummy(lua_State *L)
{
	return 0;
}

// codecache函数，用于创建并初始化一个缓存库
static int
codecache(lua_State *L)
{
	// 定义库的函数表
	luaL_Reg l[] = {
		{"clear", cleardummy}, // 定义clear方法，实际上不做任何操作
		{"mode", cleardummy},  // 定义mode方法，实际上不做任何操作
		{NULL, NULL},		   // 结束标志
	};
	// 创建一个新的lua库
	luaL_newlib(L, l);
	// 将loadfile函数设置为该库的一个字段
	lua_getglobal(L, "loadfile");
	lua_setfield(L, -2, "loadfile");
	return 1; // 返回1，表示成功创建并初始化了库
}

#endif

/**
 * 处理Lua信号钩子函数。
 * 当Lua执行达到特定条件时（如执行一定数量的指令），此函数将被调用。
 *
 * @param L Lua状态机指针，表示当前的Lua环境。
 * @param ar Lua调试信息结构体指针，包含当前激活的函数的调试信息。
 */
static void
signal_hook(lua_State *L, lua_Debug *ar)
{
	void *ud = NULL;					  // 用于存储Lua的分配函数的用户数据
	lua_getallocf(L, &ud);				  // 获取Lua的分配函数和其用户数据
	struct snlua *l = (struct snlua *)ud; // 将用户数据转换为struct snlua类型，用于进一步操作

	// 停止Lua的钩子机制，防止无限循环或过度调用
	lua_sethook(L, NULL, 0, 0);
	if (ATOM_LOAD(&l->trap))
	{							   // 使用原子操作加载l->trap的值，检查是否需要触发信号处理
		ATOM_STORE(&l->trap, 0);   // 使用原子操作将l->trap的值设置为0，表示信号已处理
		luaL_error(L, "signal 0"); // 触发Lua错误，用于通知调用者信号0已被处理
	}
}

/**
 * 切换lua状态机的活动状态。
 *
 * 该函数用于将当前的lua状态机（L）设置为活动状态，并根据trap标志决定是否设置信号钩子。
 *
 * @param L 指向当前lua状态机的指针。
 * @param l 指向结构体snlua的指针，该结构体包含了与lua状态机相关的额外信息。
 */
static void
switchL(lua_State *L, struct snlua *l)
{
	l->activeL = L; // 设置当前lua状态机为活动状态。

	if (ATOM_LOAD(&l->trap))
	{												   // 如果trap标志被设置，则设置信号钩子。
		lua_sethook(L, signal_hook, LUA_MASKCOUNT, 1); // 设置钩子，以便在执行一定数量的指令后触发信号处理。
	}
}

/*
 * 在指定的lua_State上恢复一个协程的执行。
 *
 * 参数：
 * L - 要恢复的协程所在的lua_State。
 * from - 发起协程恢复操作的lua_State，通常为调用者。
 * nargs - 传递给协程的参数数量。
 * nresults - 指向一个整数的指针，用于接收协程执行完成后返回的结果数量。
 *
 * 返回值：
 * 返回协程恢复操作的结果，具体的返回值含义由lua_resume自身定义。
 */
static int
lua_resumeX(lua_State *L, lua_State *from, int nargs, int *nresults)
{
	void *ud = NULL;
	// 获取L的内存分配函数及其用户数据。
	lua_getallocf(L, &ud);
	struct snlua *l = (struct snlua *)ud;
	// 将当前线程的lua_State切换到L。
	switchL(L, l);
	// 恢复协程的执行。
	int err = lua_resume(L, from, nargs, nresults);
	// 如果设置了trap标志，则等待lua_sethook的调用。
	if (ATOM_LOAD(&l->trap))
	{
		// 当l->trap大于等于0时，循环等待。
		while (ATOM_LOAD(&l->trap) >= 0)
			;
	}
	// 将线程的lua_State切换回原始状态。
	switchL(from, l);
	// 返回协程恢复操作的结果。
	return err;
}

/**
 * 获取当前时间的函数
 * 本函数用于获取当前时间，精确到纳秒级别。根据不同的操作系统，使用不同的方法获取时间。
 *
 * @return 返回当前时间的double值，单位为秒，精确到纳秒。
 */
static double
get_time()
{
#if !defined(__APPLE__)
	// 在非苹果系统上，使用POSIX标准的时间函数
	struct timespec ti;
	clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ti); // 获取当前线程的CPU时间

	int sec = ti.tv_sec & 0xffff; // 秒数，只保留低16位
	int nsec = ti.tv_nsec;		  // 纳秒数

	return (double)sec + (double)nsec / NANOSEC; // 将时间转换为秒，并保留纳秒精度
#else
	// 在苹果系统上，使用mach_port_t和task_info函数获取时间
	struct task_thread_times_info aTaskInfo;
	mach_msg_type_number_t aTaskInfoCount = TASK_THREAD_TIMES_INFO_COUNT;
	// 尝试获取当前任务的线程时间信息
	if (KERN_SUCCESS != task_info(mach_task_self(), TASK_THREAD_TIMES_INFO, (task_info_t)&aTaskInfo, &aTaskInfoCount))
	{
		return 0; // 如果获取失败，返回0
	}

	int sec = aTaskInfo.user_time.seconds & 0xffff; // 用户时间的秒数，只保留低16位
	int msec = aTaskInfo.user_time.microseconds;	// 用户时间的微秒数

	return (double)sec + (double)msec / MICROSEC; // 将时间转换为秒，并保留微秒精度
#endif
}

/**
 * 计算并返回从指定开始时间到当前时间的差值。
 *
 * @param start 开始时间，为一个double类型的值，通常为调用get_time()函数获得的时间戳。
 * @return 返回从开始时间到当前时间的差值，为一个double类型的值，表示时间的流逝。
 */
static inline double
diff_time(double start)
{
	double now = get_time(); // 获取当前时间

	// 如果当前时间小于开始时间，说明时间戳发生了回绕（例如1秒到2秒之间的时间戳可能会回绕到0）
	// 此时需要调整当前时间，确保返回的时间差是正确的
	if (now < start)
	{
		return now + 0x10000 - start;
	}
	else
	{
		// 如果当前时间大于或等于开始时间，则直接计算时间差
		return now - start;
	}
}

// coroutine lib, add profile

/*
** 恢复一个协程的执行。在非错误情况下返回结果的数量，错误情况下返回-1。
**
** @param L 主 Lua 状态机。
** @param co 要恢复的协程的 Lua 状态机。
** @param narg 要传递给协程的参数数量。
** @return 如果成功，返回传递给协程的结果数量；如果发生错误，返回-1。
*/
static int auxresume(lua_State *L, lua_State *co, int narg)
{
	int status, nres;
	// 检查协程的堆栈是否有足够的空间接收参数，若没有则报错。
	if (!lua_checkstack(co, narg))
	{
		lua_pushliteral(L, "too many arguments to resume"); // 报告错误信息
		return -1; /* 错误标志 */
	}
	// 将参数从主状态机移动到协程状态机中。
	lua_xmove(L, co, narg);
	// 尝试恢复协程的执行。
	status = lua_resumeX(co, L, narg, &nres);
	// 如果协程正常返回或进入挂起状态，检查主状态机是否有足够的空间接收协程返回的结果。
	if (status == LUA_OK || status == LUA_YIELD)
	{
		if (!lua_checkstack(L, nres + 1))
		{
			lua_pop(co, nres); /* 移除结果，因为空间不足 */
			lua_pushliteral(L, "too many results to resume"); // 报告错误信息
			return -1; /* 错误标志 */
		}
		// 将协程返回的结果移动到主状态机中。
		lua_xmove(co, L, nres); 
		return nres; // 返回结果数量
	}
	else
	{
		// 如果协程执行中产生错误，将错误消息移动到主状态机中。
		lua_xmove(co, L, 1); 
		return -1; /* 错误标志 */
	}
}

/**
 * 为指定的协程启用计时功能。
 * 
 * @param L lua_State的指针，表示当前的Lua环境。
 * @param co_index 协程在lua_State中的索引。
 * @param start_time 指向lua_Number的指针，用于存储协程的开始时间。
 * @return 返回1表示成功启用计时，返回0表示未能启用（可能是由于协程尚未开始计时）。
 */
static int
timing_enable(lua_State *L, int co_index, lua_Number *start_time)
{
	// 将协程索引压入堆栈，并尝试从上值索引1中获取对应的计时信息。
	lua_pushvalue(L, co_index);
	lua_rawget(L, lua_upvalueindex(1));
	if (lua_isnil(L, -1))
	{ // 检查是否存在总时间信息，若不存在，则未开始计时。
		lua_pop(L, 1);
		return 0;
	}
	// 存储协程的开始时间，并清空堆栈。
	*start_time = lua_tonumber(L, -1);
	lua_pop(L, 1);
	return 1;
}
/**
 * 计算并返回指定协程的总执行时间。
 * 
 * @param L lua_State的指针，表示当前的Lua环境。
 * @param co_index 指定协程在lua_State中的索引。
 * @return 返回指定协程的总执行时间，单位为秒。
 */
static double
timing_total(lua_State *L, int co_index)
{
	// 将协程索引压入栈，并从上值索引2中获取对应的执行时间。
	lua_pushvalue(L, co_index);
	lua_rawget(L, lua_upvalueindex(2));
	double total_time = lua_tonumber(L, -1); // 将栈顶的执行时间转换为double类型
	lua_pop(L, 1); // 弹出执行时间
	return total_time;
}

/**
 * 处理协程的恢复操作，并对其进行性能计时。
 * 
 * @param L lua_State的指针，表示当前的Lua环境。
 * @param co_index 指定协程在lua_State中的索引。
 * @param n 要传递给协程的参数个数。
 * @return 返回auxresume函数的返回值，表示协程的执行状态。
 */
static int
timing_resume(lua_State *L, int co_index, int n)
{
	lua_State *co = lua_tothread(L, co_index); // 获取协程状态
	lua_Number start_time = 0; // 初始化开始时间
	// 如果启用了性能计时，则记录开始时间
	if (timing_enable(L, co_index, &start_time))
	{
		start_time = get_time(); // 获取当前时间作为开始时间
#ifdef DEBUG_LOG
		fprintf(stderr, "PROFILE [%p] resume %lf\n", co, ti);
#endif
		// 将协程索引和开始时间压入栈，并存储为协程的上值，用于后续计时
		lua_pushvalue(L, co_index);
		lua_pushnumber(L, start_time);
		lua_rawset(L, lua_upvalueindex(1)); // 设置开始时间
	}

	int r = auxresume(L, co, n); // 恢复协程执行

	// 如果启用了性能计时，计算并更新协程的总执行时间
	if (timing_enable(L, co_index, &start_time))
	{
		double total_time = timing_total(L, co_index); // 获取当前的总执行时间
		double diff = diff_time(start_time); // 计算自开始时间以来的增量时间
		total_time += diff; // 更新总执行时间
#ifdef DEBUG_LOG
		fprintf(stderr, "PROFILE [%p] yield (%lf/%lf)\n", co, diff, total_time);
#endif
		// 将协程索引和更新后的总执行时间压入栈，并存储为协程的上值
		lua_pushvalue(L, co_index);
		lua_pushnumber(L, total_time);
		lua_rawset(L, lua_upvalueindex(2));
	}

	return r; // 返回协程的执行状态
}
/*
 * luaB_coresume: 继续执行一个协程。
 * 参数:
 *   L - Lua状态机。
 * 返回值:
 *   返回协程恢复执行的结果数量。
 */
static int luaB_coresume(lua_State *L)
{
    // 检查第一个参数是否是协程。
    luaL_checktype(L, 1, LUA_TTHREAD);
    // 尝试恢复协程执行。
    int r = timing_resume(L, 1, lua_gettop(L) - 1);
    if (r < 0)
    {
        // 如果恢复失败，返回false和错误信息。
        lua_pushboolean(L, 0);
        lua_insert(L, -2);
        return 2; /* return false + error message */
    }
    else
    {
        // 如果恢复成功，返回true和协程返回的结果。
        lua_pushboolean(L, 1);
        lua_insert(L, -(r + 1));
        return r + 1; /* return true + 'resume' returns */
    }
}

/*
 * luaB_auxwrap: 辅助函数，用于封装协程的调用。
 * 参数:
 *   L - Lua状态机。
 * 返回值:
 *   返回协程执行的结果数量。
 */
static int luaB_auxwrap(lua_State *L)
{
    // 获取封装的协程。
    lua_State *co = lua_tothread(L, lua_upvalueindex(3));
    // 尝试恢复协程执行。
    int r = timing_resume(L, lua_upvalueindex(3), lua_gettop(L));
    if (r < 0)
    {
        // 如果有错误发生，处理并传播错误。
        int stat = lua_status(co);
        if (stat != LUA_OK && stat != LUA_YIELD)
            lua_closethread(co, L); /* close variables in case of errors */
        if (lua_type(L, -1) == LUA_TSTRING)
        {                          /* error object is a string? */
            luaL_where(L, 1);     /* add extra info, if available */
            lua_insert(L, -2);
            lua_concat(L, 2);
        }
        return lua_error(L); /* propagate error */
    }
    return r;
}

/*
 * luaB_cocreate: 创建一个新的协程。
 * 参数:
 *   L - Lua状态机。
 * 返回值:
 *   返回1，将新创建的协程压入堆栈。
 */
static int luaB_cocreate(lua_State *L)
{
    // 检查第一个参数是否是函数。
    luaL_checktype(L, 1, LUA_TFUNCTION);
    // 创建一个新的协程。
    NL = lua_newthread(L);
    // 将创建函数移动到堆栈顶部。
    lua_pushvalue(L, 1); /* move function to top */
    // 将函数从主状态机移动到新协程。
    lua_xmove(L, NL, 1); /* move function from L to NL */
    return 1;
}

/*
 * luaB_cowrap: 用于封装协程创建和启动的函数。
 * 参数:
 *   L - Lua状态机。
 * 返回值:
 *   返回1，压入一个启动协程的闭包。
 */
static int luaB_cowrap(lua_State *L)
{
    // 将函数和状态机压入闭包作为上值。
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, lua_upvalueindex(2));
    // 创建新的协程。
    luaB_cocreate(L);
    // 创建并压入一个辅助包装器闭包，用于启动协程。
    lua_pushcclosure(L, luaB_auxwrap, 3);
    return 1;
}

/*
 * lstart: 开始对一个协程进行性能分析。
 * 参数:
 *   L - Lua状态机。
 * 返回值:
 *   返回0。
 */
static int
lstart(lua_State *L)
{
    // 确保堆栈顶部是协程。
    if (lua_gettop(L) != 0)
    {
        lua_settop(L, 1);
        luaL_checktype(L, 1, LUA_TTHREAD);
    }
    else
    {
        lua_pushthread(L);
    }
    // 初始化性能分析。
    lua_Number start_time = 0;
    if (timing_enable(L, 1, &start_time))
    {
        // 如果协程已经启动过，则报错。
        return luaL_error(L, "Thread %p start profile more than once", lua_topointer(L, 1));
    }

    // 重置协程的总执行时间。
    lua_pushvalue(L, 1);
    lua_pushnumber(L, 0);
    lua_rawset(L, lua_upvalueindex(2));

    // 记录协程的开始执行时间。
    lua_pushvalue(L, 1);
    start_time = get_time();
#ifdef DEBUG_LOG
    fprintf(stderr, "PROFILE [%p] start\n", L);
#endif
    lua_pushnumber(L, start_time);
    lua_rawset(L, lua_upvalueindex(1));

    return 0;
}
/*
 * 功能：停止lua协程的性能 profiling，并记录总时间。
 * 参数：
 *  - L: lua_State的指针，表示当前的lua环境。
 * 返回值：
 *  - 返回1，表示函数向lua栈中压入了一个元素。
 */
static int
lstop(lua_State *L)
{
    // 检查lua栈顶是否只有一个元素，并将其设为栈顶
	if (lua_gettop(L) != 0)
	{
		lua_settop(L, 1);
		// 检查栈顶元素是否为线程类型
		luaL_checktype(L, 1, LUA_TTHREAD);
	}
	else
	{
        // 如果栈为空，则将当前线程压入栈中
		lua_pushthread(L);
	}
    // 初始化开始时间
	lua_Number start_time = 0;
    // 检查是否已启用性能计时，未启用则报错
	if (!timing_enable(L, 1, &start_time))
	{
		return luaL_error(L, "Call profile.start() before profile.stop()");
	}
    // 计算执行时间
	double ti = diff_time(start_time);
    // 获取总执行时间
	double total_time = timing_total(L, 1);

    // 从lua_upvalueindex(1)开始更新协程的关联数据
	lua_pushvalue(L, 1); // 将协程压入栈中
	lua_pushnil(L);
	lua_rawset(L, lua_upvalueindex(1));

    // 更新累计时间
	lua_pushvalue(L, 1); // 将协程再次压入栈中
	lua_pushnumber(L, total_time);
	lua_rawset(L, lua_upvalueindex(2));

    // 将累计时间压入lua栈中，供外部使用
	total_time += ti;
	lua_pushnumber(L, total_time);
#ifdef DEBUG_LOG
    // 打印调试日志（如果定义了DEBUG_LOG）
	fprintf(stderr, "PROFILE [%p] stop (%lf/%lf)\n", lua_tothread(L, 1), ti, total_time);
#endif

	return 1; // 表示函数执行完毕，向lua栈中压入了一个元素
}

/*
 * 初始化性能分析模块
 *
 * 参数:
 *   L - Lua状态机的指针
 *
 * 返回值:
 *   返回1，表示在Lua中创建了一个新的库。
 */
static int
init_profile(lua_State *L)
{
    // 定义模块中导出的函数
	luaL_Reg l[] = {
		{"start", lstart},
		{"stop", lstop},
		{"resume", luaB_coresume},
		{"wrap", luaB_cowrap},
		{NULL, NULL},
	};

    // 创建一个新的Lua库，并将导出函数注册到该库中
	luaL_newlibtable(L, l);

    // 创建一个用于记录线程开始时间的表
	lua_newtable(L); 
    // 创建一个用于记录线程总时间的表
	lua_newtable(L); 

    // 创建一个弱引用表，用于线程表的元表，以支持垃圾回收
	lua_newtable(L); 
	lua_pushliteral(L, "kv"); // 设置表的引用模式为"kv"
	lua_setfield(L, -2, "__mode"); // 将引用模式设置到新创建的表中

    // 将弱引用表设置为线程表和时间表的元表
	lua_pushvalue(L, -1); // 复制弱引用表的引用
	lua_setmetatable(L, -3); // 设置线程->开始时间表的元表
	lua_setmetatable(L, -3); // 设置线程->总时间表的元表

    // 将导出函数表关联到Lua中，并提供两个附加参数
	luaL_setfuncs(L, l, 2);

	return 1; // 表示成功创建了一个新的Lua库
}

/// end of coroutine

/*
 * traceback函数用于处理Lua错误信息。
 * 参数：
 *     L: Lua状态机指针。
 * 返回值：
 *     返回1，表示函数向Lua栈中压入了一个元素（错误消息）。
 */
static int
traceback(lua_State *L)
{
	const char *msg = lua_tostring(L, 1); // 尝试将Lua栈顶的元素转换为字符串
	if (msg)
		luaL_traceback(L, L, msg, 1); // 如果转换成功，则调用luaL_traceback打印错误信息
	else
	{
		lua_pushliteral(L, "(no error message)"); // 如果没有错误消息，则压入一个默认错误消息
	}
	return 1; // 函数完成，返回1
}

/*
 * report_launcher_error函数用于报告启动器错误。
 * 参数：
 *     ctx: skynet上下文结构体指针。
 */
static void
report_launcher_error(struct skynet_context *ctx)
{
	// 向".launcher"服务发送错误消息
	skynet_sendname(ctx, 0, ".launcher", PTYPE_TEXT, 0, "ERROR", 5);
}

/*
 * optstring函数用于获取环境变量的值，如果未定义则返回默认字符串。
 * 参数：
 *     ctx: skynet上下文结构体指针。
 *     key: 要查询的环境变量键名。
 *     str: 如果键不存在时返回的默认字符串。
 * 返回值：
 *     返回环境变量的值或者默认字符串。
 */
static const char *
optstring(struct skynet_context *ctx, const char *key, const char *str)
{
	const char *ret = skynet_command(ctx, "GETENV", key); // 调用skynet_command查询环境变量
	if (ret == NULL) // 如果未找到环境变量
	{
		return str; // 返回默认字符串
	}
	return ret; // 返回环境变量的值
}

/**
 * 初始化Lua环境并加载指定的Lua文件。
 * 
 * @param l 指向snlua结构的指针，用于操作Lua状态机。
 * @param ctx 指向skynet上下文的指针，用于与skynet服务进行交互。
 * @param args 初始化参数，为Lua脚本传递的字符串参数。
 * @param sz args字符串的长度。
 * @return 成功返回0，失败返回1。
 */
static int
init_cb(struct snlua *l, struct skynet_context *ctx, const char *args, size_t sz)
{
	lua_State *L = l->L; // 获取当前的Lua状态机
	l->ctx = ctx; // 存储skynet上下文到snlua结构中
	lua_gc(L, LUA_GCSTOP, 0); // 停止Lua垃圾回收器

	// 设置Lua环境忽略环境变量
	lua_pushboolean(L, 1);
	lua_setfield(L, LUA_REGISTRYINDEX, "LUA_NOENV");

	// 打开Lua标准库
	luaL_openlibs(L);
	// 初始化性能分析库
	luaL_requiref(L, "skynet.profile", init_profile, 0);

	int profile_lib = lua_gettop(L); // 获取性能分析库所在栈位置

	// 替换coroutine模块中的resume和wrap函数
	lua_getglobal(L, "coroutine");
	lua_getfield(L, profile_lib, "resume");
	lua_setfield(L, -2, "resume");
	lua_getfield(L, profile_lib, "wrap");
	lua_setfield(L, -2, "wrap");

	// 调整栈顶位置，只保留性能分析库
	lua_settop(L, profile_lib - 1);

	// 存储skynet上下文到Lua注册表
	lua_pushlightuserdata(L, ctx);
	lua_setfield(L, LUA_REGISTRYINDEX, "skynet_context");
	// 初始化代码缓存库
	luaL_requiref(L, "skynet.codecache", codecache, 0);
	lua_pop(L, 1); // 弹出代码缓存库

	// 启动垃圾回收
	lua_gc(L, LUA_GCGEN, 0, 0);

	// 设置Lua路径和C路径
	const char *path = optstring(ctx, "lua_path", "./lualib/?.lua;./lualib/?/init.lua");
	lua_pushstring(L, path);
	lua_setglobal(L, "LUA_PATH");
	const char *cpath = optstring(ctx, "lua_cpath", "./luaclib/?.so");
	lua_pushstring(L, cpath);
	lua_setglobal(L, "LUA_CPATH");
	
	// 设置服务路径和预加载库
	const char *service = optstring(ctx, "luaservice", "./service/?.lua");
	lua_pushstring(L, service);
	lua_setglobal(L, "LUA_SERVICE");
	const char *preload = skynet_command(ctx, "GETENV", "preload");
	lua_pushstring(L, preload);
	lua_setglobal(L, "LUA_PRELOAD");

	// 设置Lua出错时的回溯函数
	lua_pushcfunction(L, traceback);
	assert(lua_gettop(L) == 1);

	// 加载Lua加载器
	const char *loader = optstring(ctx, "lualoader", "./lualib/loader.lua");

	// 尝试加载Lua文件
	int r = luaL_loadfile(L, loader);
	if (r != LUA_OK)
	{
		// 如果加载失败，记录错误信息并报告
		skynet_error(ctx, "Can't load %s : %s", loader, lua_tostring(L, -1));
		report_launcher_error(ctx);
		return 1;
	}

	// 调用Lua加载器，并传入初始化参数
	lua_pushlstring(L, args, sz);
	r = lua_pcall(L, 1, 0, 1);
	if (r != LUA_OK)
	{
		// 如果执行失败，记录错误信息并报告
		skynet_error(ctx, "lua loader error : %s", lua_tostring(L, -1));
		report_launcher_error(ctx);
		return 1;
	}

	// 清空Lua栈
	lua_settop(L, 0);
	// 检查并设置Lua内存限制
	if (lua_getfield(L, LUA_REGISTRYINDEX, "memlimit") == LUA_TNUMBER)
	{
		size_t limit = lua_tointeger(L, -1);
		l->mem_limit = limit;
		skynet_error(ctx, "Set memory limit to %.2f M", (float)limit / (1024 * 1024));
		// 清除内存限制标志
		lua_pushnil(L);
		lua_setfield(L, LUA_REGISTRYINDEX, "memlimit");
	}
	lua_pop(L, 1); // 弹出内存限制值

	// 重启Lua垃圾回收器
	lua_gc(L, LUA_GCRESTART, 0);

	return 0; // 初始化成功
}

/**
 * 当前函数是skynet服务启动时的回调函数。
 * 
 * @param context skynet上下文结构体，用于与skynet框架进行交互。
 * @param ud 用户数据，此处为指向snlua结构体的指针，用于操作lua状态机。
 * @param type 消息类型，此处函数假设为0。
 * @param session 会话ID，此处函数假设为0。
 * @param source 消息来源，即发送此消息的服务的句柄。
 * @param msg 消息内容的指针。
 * @param sz 消息的大小。
 * @return 返回0，表示函数执行完毕。
 */
static int
launch_cb(struct skynet_context *context, void *ud, int type, int session, uint32_t source, const void *msg, size_t sz)
{
    // 断言消息类型和会话ID都为0
    assert(type == 0 && session == 0);
    struct snlua *l = ud; // 将用户数据转换为snlua结构体指针

    // 注销当前服务的回调
    skynet_callback(context, NULL, NULL);
    // 初始化回调，如果失败则关闭服务
    int err = init_cb(l, context, msg, sz);
    if (err)
    {
        // 发送"EXIT"命令以关闭当前服务
        skynet_command(context, "EXIT", NULL);
    }

    return 0; // 表示函数执行完毕
}

/**
 * 初始化snuua结构体
 * @param l 指向snuua结构体的指针，用于lua环境的初始化和管理
 * @param ctx 指向skynet上下文的指针，用于与skynet的服务进行交互
 * @param args 启动参数，字符串形式，用于lua脚本的启动配置
 * @return 总是返回0
 */
int snlua_init(struct snlua *l, struct skynet_context *ctx, const char *args)
{
    // 获取启动参数的长度，并动态分配足够空间进行复制
    int sz = strlen(args);
    char *tmp = skynet_malloc(sz);
    memcpy(tmp, args, sz);

    // 设置回调函数，以便在服务启动时调用
    skynet_callback(ctx, l, launch_cb);

    // 注册服务，并获取服务自身handle（标识）
    const char *self = skynet_command(ctx, "REG", NULL);
    
    // 将handle转换为数字形式，用于后续的消息发送
    uint32_t handle_id = strtoul(self + 1, NULL, 16);

    // 向服务自身发送启动消息，确保这是服务接收到的第一个消息
    skynet_send(ctx, 0, handle_id, PTYPE_TAG_DONTCOPY, 0, tmp, sz);
    
    return 0;
}

/**
 * 用于lua内存分配的定制分配器。
 * 
 * @param ud 用户数据，此处为struct snlua结构体指针，用于记录内存使用情况。
 * @param ptr 原始内存块的指针，如果为NULL表示需要分配新内存，非NULL表示需要重新分配内存。
 * @param osize 原始内存块的大小，仅在重新分配内存时有意义。
 * @param nsize 需要的新内存大小，如果大于原始内存大小，则表示需要扩大内存；如果小于等于原始内存大小，则表示需要缩小内存或者不变。
 * @return 返回新分配的内存块的指针，如果分配失败则返回NULL。
 */
static void *
lalloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
	struct snlua *l = ud; // 将用户数据转换为struct snlua结构体指针。
	size_t mem = l->mem; // 记录当前内存使用量。
	l->mem += nsize; // 更新内存使用量。
	if (ptr)
		l->mem -= osize; // 如果ptr不为NULL，则在更新内存使用量时减去原始内存大小。
	
	// 检查是否超过预设的内存限制。
	if (l->mem_limit != 0 && l->mem > l->mem_limit)
	{
		// 如果申请新内存大于原始内存，或者申请新内存但原始内存不存在（即新分配内存），则内存使用量重置且返回NULL。
		if (ptr == NULL || nsize > osize)
		{
			l->mem = mem; // 重置内存使用量。
			return NULL;
		}
	}
	
	// 当内存使用量超过一定阈值时，加倍报告内存警告。
	if (l->mem > l->mem_report)
	{
		l->mem_report *= 2; // 更新内存报告阈值。
		skynet_error(l->ctx, "Memory warning %.2f M", (float)l->mem / (1024 * 1024)); // 发出内存警告。
	}
	return skynet_lalloc(ptr, osize, nsize); // 调用默认内存分配器进行实际的内存分配操作。
}

struct snlua *
snlua_create(void)
{
	struct snlua *l = skynet_malloc(sizeof(*l));
	memset(l, 0, sizeof(*l));
	l->mem_report = MEMORY_WARNING_REPORT;
	l->mem_limit = 0;
	l->L = lua_newstate(lalloc, l);
	l->activeL = NULL;
	ATOM_INIT(&l->trap, 0);
	return l;
}

void snlua_release(struct snlua *l)
{
	lua_close(l->L);
	skynet_free(l);
}

/**
 * 向snlua实例发送一个信号。
 * 
 * @param l 指向snlua结构体的指针，包含了lua环境和相关上下文信息。
 * @param signal 要发送的信号，可以是0或1。
 * 
 * 对于信号0，尝试设置一个钩子（trap），用于捕获接下来的一个Lua事件。
 * 对于信号1，打印当前的内存使用情况。
 */
void snlua_signal(struct snlua *l, int signal)
{
    // 记录接收到的信号
    skynet_error(l->ctx, "recv a signal %d", signal);
    if (signal == 0)
    {
        // 当trap未被设置时，尝试设置它来捕获Lua执行中的一个事件
        if (ATOM_LOAD(&l->trap) == 0)
        {
            // 只有当当前没有其他线程设置trap时，才尝试进行设置
            if (!ATOM_CAS(&l->trap, 0, 1))
                return;
            // 设置Lua钩子，以1次执行为限
            lua_sethook(l->activeL, signal_hook, LUA_MASKCOUNT, 1);
            // 完成trap的设置，并标记为已使用
            ATOM_CAS(&l->trap, 1, -1);
        }
    }
    else if (signal == 1)
    {
        // 打印当前服务的内存使用情况
        skynet_error(l->ctx, "Current Memory %.3fK", (float)l->mem / 1024);
    }
}
