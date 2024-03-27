#include "skynet.h"

#include "skynet_module.h"
#include "spinlock.h"

#include <assert.h>
#include <string.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#define MAX_MODULE_TYPE 32

/**
 * 结构体 modules 用于管理模块信息
 * 一个模块对应着 cservice 的 .so 文件, modules 用来管理所有的已经被加载的 .so 模块
 * @param count 模块数量
 * @param lock  用于模块信息访问控制的自旋锁
 * @param path  模块文件的路径
 * @param m     一个结构体数组，存储不同类型的SkyNet模块
 */
struct modules {
	int count;              // 模块数量
	struct spinlock lock;   // 用于模块信息访问控制的自旋锁
	const char * path;      // 模块文件的路径
	struct skynet_module m[MAX_MODULE_TYPE]; // 一个结构体数组，存储不同类型的SkyNet模块
};

static struct modules * M = NULL;

/**
 * 尝试打开指定名称的模块。
 * @param m 模块管理结构体
 * @param name 模块名称
 * @return 返回模块的动态链接库句柄，如果打开失败则返回NULL。
 */
static void *
_try_open(struct modules *m, const char * name) {
	const char *l;
	const char * path = m->path;
	size_t path_size = strlen(path);
	size_t name_size = strlen(name);

	int sz = path_size + name_size;
	//search path
	void * dl = NULL;
	char tmp[sz];
	do
	{
		memset(tmp,0,sz);
		while (*path == ';') path++;
		if (*path == '\0') break;
		l = strchr(path, ';');
		if (l == NULL) l = path + strlen(path);
		int len = l - path;
		int i;
		for (i=0;path[i]!='?' && i < len ;i++) {
			tmp[i] = path[i];
		}
		memcpy(tmp+i,name,name_size);
		if (path[i] == '?') {
			strncpy(tmp+i+name_size,path+i+1,len - i - 1);
		} else {
			fprintf(stderr,"Invalid C service path\n");
			exit(1);
		}
		dl = dlopen(tmp, RTLD_NOW | RTLD_GLOBAL);
		path = l;
	}while(dl == NULL);

	if (dl == NULL) {
		fprintf(stderr, "try open %s failed : %s\n",name,dlerror());
	}

	return dl;
}

/**
 * 查询指定名称的模块。
 * @param name 模块名称。
 * @return 返回匹配的模块结构体指针，如果没有找到则返回NULL。
 */
static struct skynet_module * 
_query(const char * name) {
	int i;
	for (i=0;i<M->count;i++) {
		if (strcmp(M->m[i].name,name)==0) {
			return &M->m[i];
		}
	}
	return NULL;
}

/**
 * 根据模块名称和API名称获取模块的API指针。
 * @param mod 模块结构体
 * @param api_name API名称
 * @return 返回API的函数指针。
 */
static void *
get_api(struct skynet_module *mod, const char *api_name) {
	size_t name_size = strlen(mod->name);
	size_t api_size = strlen(api_name);
	char tmp[name_size + api_size + 1];
	memcpy(tmp, mod->name, name_size);
	memcpy(tmp+name_size, api_name, api_size+1);
	char *ptr = strrchr(tmp, '.');
	if (ptr == NULL) {
		ptr = tmp;
	} else {
		ptr = ptr + 1;
	}
	return dlsym(mod->module, ptr);
}

/**
 * 解析模块中的符号（如创建、初始化、释放等函数）。
 * @param mod 模块结构体
 * @return 如果初始化函数解析失败，则返回1，否则返回0。
 */
static int
open_sym(struct skynet_module *mod) {
	mod->create = get_api(mod, "_create");
	mod->init = get_api(mod, "_init");
	mod->release = get_api(mod, "_release");
	mod->signal = get_api(mod, "_signal");

	return mod->init == NULL;
}

/**
 * 查询指定名称的模块。首先尝试从当前已加载的模块中查询，如果未找到，则尝试加载该模块。
 * 
 * @param name 模块的名称。
 * @return 返回查询到的模块指针。如果未找到对应模块，返回NULL。
 */
struct skynet_module * 
skynet_module_query(const char * name) {
	struct skynet_module * result = _query(name); // 首次尝试查询模块
	if (result)
		return result;

	// 加锁，确保对模块列表的操作是线程安全的
	SPIN_LOCK(M)

	result = _query(name); // 双重检查，确保在多线程环境下的正确性, 在另外线程交出锁期间, 这个值可能被改变, 所以需要再次检查

	// 如果仍未找到模块，并且模块计数未达到最大值，则尝试加载该模块
	if (result == NULL && M->count < MAX_MODULE_TYPE) {
		int index = M->count; // 计算即将添加的模块在数组中的索引
		void * dl = _try_open(M,name); // 尝试打开（加载）模块
		if (dl) { // 如果模块加载成功
			M->m[index].name = name; // 临时存储模块名称
			M->m[index].module = dl; // 保存模块的动态链接库句柄

			// 尝试解析模块中的符号（如初始化函数等）
			if (open_sym(&M->m[index]) == 0) {
				M->m[index].name = skynet_strdup(name); // 模块名称成功复制，防止原字符串被修改
				M->count++; // 模块计数加一
				result = &M->m[index]; // 设置查询结果为新加载的模块
			}
		}
	}

	// 解锁
	SPIN_UNLOCK(M)

	return result; // 返回查询结果
}

/**
 * 创建模块实例。
 * @param m 模块结构体
 * @return 如果模块定义了创建函数则调用它创建实例，否则返回一个特殊标识符。
 */
void * 
skynet_module_instance_create(struct skynet_module *m) {
	if (m->create) {
		return m->create();
	} else {
		return (void *)(intptr_t)(~0);
	}
}

/**
 * 初始化模块实例。
 * @param m 模块结构体
 * @param inst 模块实例
 * @param ctx 上下文
 * @param parm 参数
 * @return 调用模块的初始化函数，返回初始化结果。
 */
int
skynet_module_instance_init(struct skynet_module *m, void * inst, struct skynet_context *ctx, const char * parm) {
	return m->init(inst, ctx, parm);
}

/**
 * 释放模块实例。
 * @param m 模块结构体
 * @param inst 模块实例
 */
void 
skynet_module_instance_release(struct skynet_module *m, void *inst) {
	if (m->release) {
		m->release(inst);
	}
}

/**
 * 给模块实例发送信号。
 * @param m 模块结构体
 * @param inst 模块实例
 * @param signal 信号值
 */
void
skynet_module_instance_signal(struct skynet_module *m, void *inst, int signal) {
	if (m->signal) {
		m->signal(inst, signal);
	}
}

/**
 * 初始化模块管理器。
 * @param path 模块路径
 */
void 
skynet_module_init(const char *path) {
	struct modules *m = skynet_malloc(sizeof(*m));
	m->count = 0;
	m->path = skynet_strdup(path);

	SPIN_INIT(m)

	M = m;
}