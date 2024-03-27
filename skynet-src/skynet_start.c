#include "skynet.h"
#include "skynet_server.h"
#include "skynet_imp.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "skynet_module.h"
#include "skynet_timer.h"
#include "skynet_monitor.h"
#include "skynet_socket.h"
#include "skynet_daemon.h"
#include "skynet_harbor.h"

#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

// 监控结构体，用于管理worker线程和一些同步操作
struct monitor {
	int count; // worker线程数
	struct skynet_monitor ** m; // 指向监控对象的指针数组
	pthread_cond_t cond; // 条件变量，用于线程同步
	pthread_mutex_t mutex; // 互斥锁，用于线程同步
	int sleep; // 睡眠的worker线程数
	int quit; // 标记是否退出
};

// worker线程参数结构体
struct worker_parm {
	struct monitor *m; // 指向监控结构体的指针
	int id; // worker线程ID
	int weight; // worker线程处理消息的权重
};

// 全局变量，用于接收SIGHUP信号
static volatile int SIG = 0;

// 处理SIGHUP信号的函数
static void
handle_hup(int signal) {
	if (signal == SIGHUP) {
		SIG = 1;
	}
}

// 检查是否应该中止线程循环的宏定义
#define CHECK_ABORT if (skynet_context_total()==0) break;

// 创建新线程
static void
create_thread(pthread_t *thread, void *(*start_routine) (void *), void *arg) {
	if (pthread_create(thread,NULL, start_routine, arg)) {
		fprintf(stderr, "Create thread failed");
		exit(1);
	}
}

// 唤醒监控线程
static void
wakeup(struct monitor *m, int busy) {
	if (m->sleep >= m->count - busy) {
		// 如果睡眠的线程数大于等于剩余的线程数，则发送信号以唤醒睡眠的worker线程
		pthread_cond_signal(&m->cond);
	}
}

// socket处理线程
static void *
thread_socket(void *p) {
	struct monitor * m = p;
	skynet_initthread(THREAD_SOCKET);
	for (;;) {
		int r = skynet_socket_poll();
		if (r==0)
			break;
		if (r<0) {
			CHECK_ABORT
			continue;
		}
		// 如果有事件发生，则唤醒所有的worker线程
		wakeup(m,0);
	}
	return NULL;
}

// 释放监控结构体资源
static void
free_monitor(struct monitor *m) {
	int i;
	int n = m->count;
	for (i=0;i<n;i++) {
		skynet_monitor_delete(m->m[i]);
	}
	pthread_mutex_destroy(&m->mutex);
	pthread_cond_destroy(&m->cond);
	skynet_free(m->m);
	skynet_free(m);
}

// 监控线程函数
// 参数 p 为指向 struct monitor 结构体的指针，该结构体包含了监控线程需要处理的对象列表和数量
// 返回值为 void*
static void *
thread_monitor(void *p) {
	struct monitor * m = p; // 将传入的参数转换为 struct monitor 类型的指针
	int i;
	int n = m->count; // 获取监控对象的数量
	skynet_initthread(THREAD_MONITOR); // 初始化线程，设置为监控线程
	for (;;) {
		CHECK_ABORT // 检查是否有终止线程的请求
		// 遍历所有监控对象，进行检查
		for (i=0;i<n;i++) {
			skynet_monitor_check(m->m[i]);
		}
		// 每隔一段时间（5秒）检查一次所有监控对象
		for (i=0;i<5;i++) {
			CHECK_ABORT // 在每个循环开始前再次检查终止请求
			sleep(1); // 暂停一秒
		}
	}

	return NULL; // 线程结束，返回 NULL
}

// 处理SIGHUP信号，重新打开日志文件
static void
signal_hup() {
    // 生成一个用于通知日志服务重新打开日志文件的系统消息
	struct skynet_message smsg;
	smsg.source = 0; // 消息来源
	smsg.session = 0; // 会话ID
	smsg.data = NULL; // 消息数据
	// 设置消息类型为系统类型
	smsg.sz = (size_t)PTYPE_SYSTEM << MESSAGE_TYPE_SHIFT;
	// 查找名为"logger"的服务句柄
	uint32_t logger = skynet_handle_findname("logger");
	if (logger) {
		// 如果找到，则向该服务发送消息
		skynet_context_push(logger, &smsg);
	}
}

// 定时器线程函数
// 参数 p 为指向 monitor 结构体的指针，用于线程间的数据共享和同步
// 返回值为空指针，表示线程执行完毕
static void *
thread_timer(void *p) {
	struct monitor * m = p;
	// 初始化线程，设置线程类型为定时器线程
	skynet_initthread(THREAD_TIMER);
	for (;;) {
		// 更新系统时间
		skynet_updatetime();
		// 更新socket时间
		skynet_socket_updatetime();
		// 检查是否有终止线程的请求
		CHECK_ABORT
		// 唤醒除socket线程外的所有worker线程进行工作
		wakeup(m,m->count-1);
		// 线程休眠2.5毫秒，进行周期性的任务调度
		usleep(2500);
		if (SIG) {
			// 如果收到SIGHUP信号，则处理日志重开操作
			signal_hup();
			// 重置信号标志
			SIG = 0;
		}
	}
	// 在线程退出前，唤醒所有线程，进行资源的清理工作
	skynet_socket_exit();
	pthread_mutex_lock(&m->mutex);
	m->quit = 1;
	pthread_cond_broadcast(&m->cond);
	pthread_mutex_unlock(&m->mutex);
	return NULL;
}

// worker线程
static void *
thread_worker(void *p) {
	struct worker_parm *wp = p;
	int id = wp->id;
	int weight = wp->weight;
	struct monitor *m = wp->m;
	struct skynet_monitor *sm = m->m[id];
	skynet_initthread(THREAD_WORKER);
	struct message_queue * q = NULL;
	while (!m->quit) {
		// 处理消息
		q = skynet_context_message_dispatch(sm, q, weight);
		if (q == NULL) {
			// 如果没有消息，则进入睡眠状态
			if (pthread_mutex_lock(&m->mutex) == 0) {
				++ m->sleep;
				pthread_cond_wait(&m->cond, &m->mutex);
				-- m->sleep;
				if (pthread_mutex_unlock(&m->mutex)) {
					fprintf(stderr, "unlock mutex error");
					exit(1);
				}
			}
		}
	}
	return NULL;
}

/**
 * 初始化并启动各个监控线程和工作线程。
 * 
 * @param thread 线程数量，包括监控线程和工作线程。
 */
static void
start(int thread) {
	pthread_t pid[thread+3]; // 创建线程数组，可容纳所有监控线程和工作线程的pthread_t。

	struct monitor *m = skynet_malloc(sizeof(*m)); // 分配内存给监控结构体。
	memset(m, 0, sizeof(*m)); // 清零监控结构体。
	m->count = thread; // 设置线程数量。
	m->sleep = 0; // 初始化休眠计数。

	// 分配内存给监控指针数组，并为每个监控结构体初始化。
	m->m = skynet_malloc(thread * sizeof(struct skynet_monitor *));
	int i;
	for (i=0;i<thread;i++) {
		m->m[i] = skynet_monitor_new();
	}

	// 初始化互斥锁，用于线程间同步。
	if (pthread_mutex_init(&m->mutex, NULL)) {
		fprintf(stderr, "Init mutex error");
		exit(1);
	}
	// 初始化条件变量，用于线程间的通信。
	if (pthread_cond_init(&m->cond, NULL)) {
		fprintf(stderr, "Init cond error");
		exit(1);
	}

	// 创建监控、定时器、socket处理三个主线程。
	create_thread(&pid[0], thread_monitor, m);
	create_thread(&pid[1], thread_timer, m);
	create_thread(&pid[2], thread_socket, m);

	// 定义权重数组，用于工作线程的任务分配权重。
	static int weight[] = { 
		-1, -1, -1, -1, 0, 0, 0, 0,
		1, 1, 1, 1, 1, 1, 1, 1, 
		2, 2, 2, 2, 2, 2, 2, 2, 
		3, 3, 3, 3, 3, 3, 3, 3, };
	struct worker_parm wp[thread]; // 工作线程参数结构体数组。
	for (i=0;i<thread;i++) {
		wp[i].m = m; // 设置监控结构体指针。
		wp[i].id = i; // 设置线程ID。
		if (i < sizeof(weight)/sizeof(weight[0])) {
			wp[i].weight= weight[i]; // 如果线程ID在权重数组范围内，设置权重。
		} else {
			wp[i].weight = 0; // 对于ID超出权重数组范围的线程，设置默认权重。
		}
		create_thread(&pid[i+3], thread_worker, &wp[i]); // 创建工作线程。
	}

	// 等待所有线程完成。
	for (i=0;i<thread+3;i++) {
		pthread_join(pid[i], NULL); 
	}

	free_monitor(m); // 释放监控结构体及相关资源。
}

/**
 * 初始化函数，用于启动服务。
 * 
 * @param logger 用于记录错误信息的skynet上下文结构体。
 * @param cmdline 启动命令行参数，包含要启动的服务名称和参数。
 */
static void
bootstrap(struct skynet_context * logger, const char * cmdline) {
	int sz = strlen(cmdline); // 获取命令行字符串长度
	char name[sz+1]; // 存储服务名称
	char args[sz+1]; // 存储服务参数
	int arg_pos; // 用于定位参数开始的位置
	sscanf(cmdline, "%s", name);  // 从命令行中提取服务名称
	arg_pos = strlen(name); // 获取服务名称的长度，用于寻找参数的起始位置
	if (arg_pos < sz) {
		// 如果服务名称长度小于命令行长度，则存在参数
		while(cmdline[arg_pos] == ' ') { // 跳过空格，找到参数的起始字符
			arg_pos++;
		}
		strncpy(args, cmdline + arg_pos, sz); // 将参数复制到args变量中
	} else {
		// 如果命令行中没有参数
		args[0] = '\0'; // args为空字符串
	}
	// 尝试创建服务上下文
	struct skynet_context *ctx = skynet_context_new(name, args);
	if (ctx == NULL) {
		// 如果创建失败，记录错误信息并退出
		skynet_error(NULL, "Bootstrap error : %s\n", cmdline);
		skynet_context_dispatchall(logger); // 尝试分发所有消息
		exit(1); // 退出程序
	}
}

/**
 * 初始化skynet服务。
 * @param config 包含启动参数的结构体，指定了日志服务、守护进程模式、harbor配置等。
 * 该函数负责设置SIGHUP信号处理程序，初始化各种服务（如harbor, handle, message queue, module, timer, socket等），
 * 创建并启动logger服务，加载引导配置，并启动线程。
 * 如果在启动过程中遇到错误，会打印错误信息并退出。
 */
void 
skynet_start(struct skynet_config * config) {
	// 注册SIGHUP信号，用于重新打开日志文件
	struct sigaction sa;
	sa.sa_handler = &handle_hup;
	sa.sa_flags = SA_RESTART;
	sigfillset(&sa.sa_mask);
	sigaction(SIGHUP, &sa, NULL);

	// 如果配置了守护进程模式，则尝试初始化守护进程
	if (config->daemon) {
		if (daemon_init(config->daemon)) {
			exit(1);
		}
	}

	// 初始化harbor、handle、消息队列、模块、定时器、socket等服务
	skynet_harbor_init(config->harbor);
	skynet_handle_init(config->harbor);
	skynet_mq_init();
	skynet_module_init(config->module_path);
	skynet_timer_init();
	skynet_socket_init();
	// 启用性能监控
	skynet_profile_enable(config->profile);

	// 创建logger服务
	struct skynet_context *ctx = skynet_context_new(config->logservice, config->logger);
	if (ctx == NULL) {
		fprintf(stderr, "Can't launch %s service\n", config->logservice);
		exit(1);
	}

	// 将logger服务注册到全局handle表中
	skynet_handle_namehandle(skynet_context_handle(ctx), "logger");

	// 执行引导序列
	bootstrap(ctx, config->bootstrap);

	// 启动额外的线程
	start(config->thread);

	// 在释放socket资源前，确保harbor服务安全退出
	skynet_harbor_exit();
	skynet_socket_free();
	// 如果是守护进程模式，执行守护进程的退出清理
	if (config->daemon) {
		daemon_exit(config->daemon);
	}
}
