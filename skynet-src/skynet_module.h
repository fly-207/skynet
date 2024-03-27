#ifndef SKYNET_MODULE_H
#define SKYNET_MODULE_H

// 定义skynet模块的结构体以及相关的函数原型

struct skynet_context;

// 定义模块创建函数的类型，该函数负责创建模块的实例
typedef void * (*skynet_dl_create)(void);
// 定义模块初始化函数的类型，初始化模块实例
typedef int (*skynet_dl_init)(void * inst, struct skynet_context *, const char * parm);
// 定义模块释放函数的类型，负责释放模块实例
typedef void (*skynet_dl_release)(void * inst);
// 定义模块发送信号函数的类型，用于向模块实例发送信号
typedef void (*skynet_dl_signal)(void * inst, int signal);

// 定义skynet模块的结构体
// 一个模块是 cservice 目录下 .so 的抽象/封装
struct skynet_module {
	const char * name; // 模块名称
	void * module; // 模块的指针
	skynet_dl_create create; // 模块创建函数
	skynet_dl_init init; // 模块初始化函数
	skynet_dl_release release; // 模块释放函数
	skynet_dl_signal signal; // 模块发送信号函数
};

// 通过模块名称查询模块
struct skynet_module * skynet_module_query(const char * name);
// 创建模块实例
void * skynet_module_instance_create(struct skynet_module *);
// 初始化模块实例
int skynet_module_instance_init(struct skynet_module *, void * inst, struct skynet_context *ctx, const char * parm);
// 释放模块实例
void skynet_module_instance_release(struct skynet_module *, void *inst);
// 向模块实例发送信号
void skynet_module_instance_signal(struct skynet_module *, void *inst, int signal);

// 初始化模块，从指定路径加载模块
void skynet_module_init(const char *path);

#endif