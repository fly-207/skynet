#include "skynet.h"
#include "skynet_mq.h"
#include "skynet_handle.h"
#include "spinlock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#define DEFAULT_QUEUE_SIZE 64
#define MAX_GLOBAL_MQ 0x10000

// 0 means mq is not in global mq.
// 1 means mq is in global mq , or the message is dispatching.

#define MQ_IN_GLOBAL 1
#define MQ_OVERLOAD 1024

struct message_queue
{
	struct spinlock lock;
	uint32_t handle;
	int cap;
	int head;
	int tail;
	int release;
	int in_global;
	int overload;
	int overload_threshold;
	struct skynet_message *queue;
	struct message_queue *next;
};

struct global_queue
{
	struct message_queue *head;
	struct message_queue *tail;
	struct spinlock lock;
};

static struct global_queue *Q = NULL;

void skynet_globalmq_push(struct message_queue *queue)
{
	struct global_queue *q = Q;

	SPIN_LOCK(q)
	assert(queue->next == NULL);
	if (q->tail)
	{
		q->tail->next = queue;
		q->tail = queue;
	}
	else
	{
		q->head = q->tail = queue;
	}
	SPIN_UNLOCK(q)
}

struct message_queue *
skynet_globalmq_pop()
{
	struct global_queue *q = Q;

	SPIN_LOCK(q)
	struct message_queue *mq = q->head;
	if (mq)
	{
		q->head = mq->next;
		if (q->head == NULL)
		{
			assert(mq == q->tail);
			q->tail = NULL;
		}
		mq->next = NULL;
	}
	SPIN_UNLOCK(q)

	return mq;
}

struct message_queue *
skynet_mq_create(uint32_t handle)
{
	struct message_queue *q = skynet_malloc(sizeof(*q));
	q->handle = handle;
	q->cap = DEFAULT_QUEUE_SIZE;
	q->head = 0;
	q->tail = 0;
	SPIN_INIT(q)
	// When the queue is create (always between service create and service init) ,
	// set in_global flag to avoid push it to global queue .
	// If the service init success, skynet_context_new will call skynet_mq_push to push it to global queue.
	q->in_global = MQ_IN_GLOBAL;
	q->release = 0;
	q->overload = 0;
	q->overload_threshold = MQ_OVERLOAD;
	q->queue = skynet_malloc(sizeof(struct skynet_message) * q->cap);
	q->next = NULL;

	return q;
}

static void
_release(struct message_queue *q)
{
	assert(q->next == NULL);
	SPIN_DESTROY(q)
	skynet_free(q->queue);
	skynet_free(q);
}

uint32_t
skynet_mq_handle(struct message_queue *q)
{
	return q->handle;
}

int skynet_mq_length(struct message_queue *q)
{
	int head, tail, cap;

	SPIN_LOCK(q)
	head = q->head;
	tail = q->tail;
	cap = q->cap;
	SPIN_UNLOCK(q)

	if (head <= tail)
	{
		return tail - head;
	}
	return tail + cap - head;
}

int skynet_mq_overload(struct message_queue *q)
{
	if (q->overload)
	{
		int overload = q->overload;
		q->overload = 0;
		return overload;
	}
	return 0;
}

/**
 * 从消息队列中弹出一条消息。
 * 如果队列中有消息，则取出消息并返回0，否则返回1。
 *
 * @param q 指向消息队列的指针。
 * @param message 指向要取出的消息的指针，如果队列非空，则会填充消息内容。
 * @return 如果成功弹出消息，则返回0；如果队列为空，则返回1。
 */
int skynet_mq_pop(struct message_queue *q, struct skynet_message *message)
{
	int ret = 1; // 默认返回1，表示队列为空
	SPIN_LOCK(q) // 获取消息队列的自旋锁

	// 当队列的头和尾不相等时，即队列非空
	if (q->head != q->tail)
	{
		*message = q->queue[q->head++]; // 取出消息并更新头指针
		ret = 0;						// 队列非空，返回0
		int head = q->head;
		int tail = q->tail;
		int cap = q->cap;

		// 处理头指针越界的情况
		if (head >= cap)
		{
			q->head = head = 0;
		}
		// 计算队列中剩余的消息数量
		int length = tail - head;
		if (length < 0)
		{
			// 处理尾指针在头指针之前的场景
			length += cap;
		}
		// 当队列中的消息数量超过阈值时，动态调整阈值
		while (length > q->overload_threshold)
		{
			q->overload = length;		// 记录当前队列长度作为过载标志
			q->overload_threshold *= 2; // 将过载阈值翻倍
		}
	}
	else
	{
		// 当队列为空时，重置过载阈值
		q->overload_threshold = MQ_OVERLOAD;
	}

	// 如果没有弹出消息，则将队列从全局队列中移除
	if (ret)
	{
		q->in_global = 0;
	}

	SPIN_UNLOCK(q) // 释放消息队列的自旋锁

	return ret; // 返回操作结果
}

static void
expand_queue(struct message_queue *q)
{
	struct skynet_message *new_queue = skynet_malloc(sizeof(struct skynet_message) * q->cap * 2);
	int i;
	for (i = 0; i < q->cap; i++)
	{
		new_queue[i] = q->queue[(q->head + i) % q->cap];
	}
	q->head = 0;
	q->tail = q->cap;
	q->cap *= 2;

	skynet_free(q->queue);
	q->queue = new_queue;
}

void skynet_mq_push(struct message_queue *q, struct skynet_message *message)
{
	assert(message);
	SPIN_LOCK(q)

	q->queue[q->tail] = *message;
	if (++q->tail >= q->cap)
	{
		q->tail = 0;
	}

	if (q->head == q->tail)
	{
		expand_queue(q);
	}

	if (q->in_global == 0)
	{
		q->in_global = MQ_IN_GLOBAL;
		skynet_globalmq_push(q);
	}

	SPIN_UNLOCK(q)
}

void skynet_mq_init()
{
	struct global_queue *q = skynet_malloc(sizeof(*q));
	memset(q, 0, sizeof(*q));
	SPIN_INIT(q);
	Q = q;
}

void skynet_mq_mark_release(struct message_queue *q)
{
	SPIN_LOCK(q)
	assert(q->release == 0);
	q->release = 1;
	if (q->in_global != MQ_IN_GLOBAL)
	{
		skynet_globalmq_push(q);
	}
	SPIN_UNLOCK(q)
}

static void
_drop_queue(struct message_queue *q, message_drop drop_func, void *ud)
{
	struct skynet_message msg;
	while (!skynet_mq_pop(q, &msg))
	{
		drop_func(&msg, ud);
	}
	_release(q);
}

void skynet_mq_release(struct message_queue *q, message_drop drop_func, void *ud)
{
	SPIN_LOCK(q)

	if (q->release)
	{
		SPIN_UNLOCK(q)
		_drop_queue(q, drop_func, ud);
	}
	else
	{
		skynet_globalmq_push(q);
		SPIN_UNLOCK(q)
	}
}
