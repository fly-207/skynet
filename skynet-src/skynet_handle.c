#include "skynet.h"

#include "skynet_handle.h"
#include "skynet_server.h"
#include "rwlock.h"

#include <stdlib.h>
#include <assert.h>
#include <string.h>

// 定义默认槽大小为4
#define DEFAULT_SLOT_SIZE 4
// 定义最大槽大小为0x40000000（二进制为10000000000000000000000000000000）
#define MAX_SLOT_SIZE 0x40000000

// 定义一个结构体用于存储名称和句柄的映射关系
struct handle_name
{
	char *name;		 // 名称字符串
	uint32_t handle; // 对应的句柄
};

// 定义一个结构体用于管理句柄存储
struct handle_storage
{
	struct rwlock lock; // 读写锁，用于并发控制

	uint32_t harbor;			  // 港口编号，用于区分不同的服务实例
	uint32_t handle_index;		  // 句柄索引，用于快速定位句柄
	int slot_size;				  // 每个槽的大小，决定了可以存储的最大句柄数量
	struct skynet_context **slot; // 指向skynet_context的指针数组，存储实际的服务句柄

	int name_cap;			  // 名称容量，表示name数组当前的容量
	int name_count;			  // 名称数量，表示当前已存储的名称数量
	struct handle_name *name; // 指向handle_name结构体的指针数组，存储名称和句柄的映射关系
};

// 声明一个全局的handle_storage指针，用于全局访问句柄存储
static struct handle_storage *H = NULL;

/**
 * 为skynet上下文注册一个唯一的handle（句柄）。
 *
 * @param ctx 指向skynet上下文的指针。
 * @return 返回注册的handle，是一个32位无符号整数。handle的高8位用于表示harbor（集群信息），低24位用于索引具体的服务。
 */
uint32_t
skynet_handle_register(struct skynet_context *ctx)
{
	struct handle_storage *s = H; // 使用全局变量H，指向handle存储结构。

	rwlock_wlock(&s->lock); // 获取写锁，保证线程安全。

	for (;;)
	{
		int i;

		// 获取当前的handle索引值。 不从头开始找, 从0开始找一个空目标会发生大概率的失败(找不到空闲槽位)
		uint32_t handle = s->handle_index;
		for (i = 0; i < s->slot_size; i++, handle++)
		{ // 遍历所有的slot槽。
			if (handle > HANDLE_MASK)
			{ // 如果handle超过最大值，则从1开始重新计数。
				handle = 1;
			}
			int hash = handle & (s->slot_size - 1); // 计算hash值，用于定位slot槽。
			if (s->slot[hash] == NULL)
			{ // 如果找到空的slot槽，将上下文添加进去，并更新handle索引。
				s->slot[hash] = ctx;
				s->handle_index = handle + 1;

				rwlock_wunlock(&s->lock); // 解锁。

				handle |= s->harbor; // 将harbor值与handle合并，形成最终的handle。
				return handle;		 // 返回注册的handle。
			}
		}
		assert((s->slot_size * 2 - 1) <= HANDLE_MASK); // 确保slot大小增长不会导致handle计算溢出。
		// 当slot槽全部占满时，将slot大小翻倍，并重新哈希所有已存在的上下文。
		struct skynet_context **new_slot = skynet_malloc(s->slot_size * 2 * sizeof(struct skynet_context *));
		memset(new_slot, 0, s->slot_size * 2 * sizeof(struct skynet_context *));
		for (i = 0; i < s->slot_size; i++)
		{
			if (s->slot[i])
			{
				int hash = skynet_context_handle(s->slot[i]) & (s->slot_size * 2 - 1); // 为每个上下文计算新的hash值。
				assert(new_slot[hash] == NULL);										   // 确保新槽位未被占用。
				new_slot[hash] = s->slot[i];										   // 将上下文迁移到新的槽位。
			}
		}
		skynet_free(s->slot); // 释放旧的slot数组。
		s->slot = new_slot;	  // 使用新的slot数组。
		s->slot_size *= 2;	  // 更新slot大小。
	}
}

/**
 * 使某个服务的句柄退役。
 * @param handle 要退役的服务句柄。
 * @return 如果成功使句柄退役，则返回1；否则返回0。
 */
int skynet_handle_retire(uint32_t handle)
{
	int ret = 0; // 默认返回值为0，表示句柄未成功退役
	struct handle_storage *s = H; // 使用全局句柄存储变量

	rwlock_wlock(&s->lock); // 获取写锁，确保处理过程中句柄表的一致性

	uint32_t hash = handle & (s->slot_size - 1); // 计算句柄的哈希值，用于定位句柄在表中的位置
	struct skynet_context *ctx = s->slot[hash]; // 获取对应哈希值的服务上下文

	if (ctx != NULL && skynet_context_handle(ctx) == handle) // 确认ctx不为空且其句柄与要退役的句柄匹配
	{
		s->slot[hash] = NULL; // 从句柄表中移除该句柄
		ret = 1; // 表示句柄成功退役
		int i;
		int j = 0, n = s->name_count; // j用于重新整理name数组，n为当前name数组中的服务数量
		for (i = 0; i < n; ++i) // 遍历name数组，移除退役句柄对应的服务名，并重构数组
		{
			if (s->name[i].handle == handle)
			{
				skynet_free(s->name[i].name); // 释放对应服务名的内存
				continue; // 跳过当前循环，继续下一个
			}
			else if (i != j)
			{
				s->name[j] = s->name[i]; // 将有效的服务信息向前移动
			}
			++j; // 更新有效服务数量
		}
		s->name_count = j; // 更新name数组中的服务数量
	}
	else
	{
		ctx = NULL; // 如果句柄未找到，则ctx为NULL
	}

	rwlock_wunlock(&s->lock); // 释放写锁

	if (ctx) // 如果ctx不为NULL，表示句柄之前存在，现在已成功移除
	{
		// 释放ctx资源，可能触发对句柄相关操作，故先解锁
		skynet_context_release(ctx);
	}

	return ret; // 返回退役结果
}

void skynet_handle_retireall()
{
	struct handle_storage *s = H;
	for (;;)
	{
		int n = 0;
		int i;
		for (i = 0; i < s->slot_size; i++)
		{
			rwlock_rlock(&s->lock);
			struct skynet_context *ctx = s->slot[i];
			uint32_t handle = 0;
			if (ctx)
			{
				handle = skynet_context_handle(ctx);
				++n;
			}
			rwlock_runlock(&s->lock);
			if (handle != 0)
			{
				skynet_handle_retire(handle);
			}
		}
		if (n == 0)
			return;
	}
}

struct skynet_context *
skynet_handle_grab(uint32_t handle)
{
	struct handle_storage *s = H;
	struct skynet_context *result = NULL;

	rwlock_rlock(&s->lock);

	uint32_t hash = handle & (s->slot_size - 1);
	struct skynet_context *ctx = s->slot[hash];
	if (ctx && skynet_context_handle(ctx) == handle)
	{
		result = ctx;
		skynet_context_grab(result);
	}

	rwlock_runlock(&s->lock);

	return result;
}

uint32_t
skynet_handle_findname(const char *name)
{
	struct handle_storage *s = H;

	rwlock_rlock(&s->lock);

	uint32_t handle = 0;

	int begin = 0;
	int end = s->name_count - 1;
	while (begin <= end)
	{
		int mid = (begin + end) / 2;
		struct handle_name *n = &s->name[mid];
		int c = strcmp(n->name, name);
		if (c == 0)
		{
			handle = n->handle;
			break;
		}
		if (c < 0)
		{
			begin = mid + 1;
		}
		else
		{
			end = mid - 1;
		}
	}

	rwlock_runlock(&s->lock);

	return handle;
}

static void
_insert_name_before(struct handle_storage *s, char *name, uint32_t handle, int before)
{
	if (s->name_count >= s->name_cap)
	{
		s->name_cap *= 2;
		assert(s->name_cap <= MAX_SLOT_SIZE);
		struct handle_name *n = skynet_malloc(s->name_cap * sizeof(struct handle_name));
		int i;
		for (i = 0; i < before; i++)
		{
			n[i] = s->name[i];
		}
		for (i = before; i < s->name_count; i++)
		{
			n[i + 1] = s->name[i];
		}
		skynet_free(s->name);
		s->name = n;
	}
	else
	{
		int i;
		for (i = s->name_count; i > before; i--)
		{
			s->name[i] = s->name[i - 1];
		}
	}
	s->name[before].name = name;
	s->name[before].handle = handle;
	s->name_count++;
}

static const char *
_insert_name(struct handle_storage *s, const char *name, uint32_t handle)
{
	int begin = 0;
	int end = s->name_count - 1;
	while (begin <= end)
	{
		int mid = (begin + end) / 2;
		struct handle_name *n = &s->name[mid];
		int c = strcmp(n->name, name);
		if (c == 0)
		{
			return NULL;
		}
		if (c < 0)
		{
			begin = mid + 1;
		}
		else
		{
			end = mid - 1;
		}
	}
	char *result = skynet_strdup(name);

	_insert_name_before(s, result, handle, begin);

	return result;
}

const char *
skynet_handle_namehandle(uint32_t handle, const char *name)
{
	rwlock_wlock(&H->lock);

	const char *ret = _insert_name(H, name, handle);

	rwlock_wunlock(&H->lock);

	return ret;
}

/**
 * 初始化句柄管理器
 * @param harbor 避难所编号，用于服务的本地标识
 * 该函数不返回任何值。
 */
void skynet_handle_init(int harbor)
{
	// 断言句柄管理器尚未初始化
	assert(H == NULL);
	// 分配内存给句柄存储结构体，并初始化其大小和内容
	struct handle_storage *s = skynet_malloc(sizeof(*H));
	s->slot_size = DEFAULT_SLOT_SIZE;
	s->slot = skynet_malloc(s->slot_size * sizeof(struct skynet_context *));
	memset(s->slot, 0, s->slot_size * sizeof(struct skynet_context *));

	// 初始化读写锁，用于并发控制
	rwlock_init(&s->lock);
	// 保留0号句柄给系统使用
	s->harbor = (uint32_t)(harbor & 0xff) << HANDLE_REMOTE_SHIFT;
	s->handle_index = 1; // 句柄索引从1开始
	s->name_cap = 2; // 初始化名称容量为2
	s->name_count = 0; // 当前没有名称记录
	// 分配名称存储空间
	s->name = skynet_malloc(s->name_cap * sizeof(struct handle_name));

	// 全局句柄管理器指向初始化好的存储结构
	H = s;

	// 不需要释放H指向的内存，在程序结束时会整体释放
}
