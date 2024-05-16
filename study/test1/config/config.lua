include "config.path.lua"


--[[
    每个服务在启动的时候都会执行这个文件, 可以将公共导入.lua放到这个文件中
    注意这不是一个服务, 所以不能放到服务目录下而依靠 path 配置来被找到, 需要指定为一个最终路径
]]
preload = project_path.."/global/preload.lua"

--[[
    启动多少个工作线程。通常不要将它配置超过你实际拥有的 CPU 核心数。
]]
thread = 2

--[[
    skynet 启动的第一个服务以及其启动参数。默认配置为 snlua bootstrap ，即启动一个名为 bootstrap 的 lua 服务。通常指的是 service/bootstrap.lua 这段代码
]]
bootstrap = "snlua bootstrap"

--[[
    它决定了 skynet 内建的 skynet_error 这个 C API 将信息输出到什么文件中。如果 logger 配置为 nil ，将输出到标准输出。你可以配置一个文件名来将信息记录在特定文件中
    logger = "study.log"
]]
logger = nil

--[[
    默认为 "logger" ，你可以配置为你定制的 log 服务（比如加上时间戳等更多信息）。可以参考 service_logger.c 来实现它。
    注：如果你希望用 lua 来编写这个服务，可以在这里填写 snlua ，然后在 logger 配置具体的 lua 服务的名字。
    在 examples 目录下，有 config.userlog 这个范例可供参考
]]
logservice  = nil


--[[
    配置一个路径，当你运行时为一个服务打开 log 时，这个服务所有的输入消息都会被记录在这个目录下，文件名为服务地址
]]
logpath = "logs"

--[[
    standalone 是 skynet 的一个启动模式，它启动一个独立的 skynet 节点，这个节点不连接到其它节点，它只作为其它节点的启动器。
    standalone 的格式为 ip:port ，ip 可以是 0.0.0.0 ，表示监听所有网卡。
    如果 standalone 为 nil ，则 skynet 将工作在单节点模式下，此时 master 和address 以及 standalone 都不必设置

    每个 skynet 进程都是一个 slave 节点。但其中一个 slave 节点可以通过配置 standalone 来多启动一个 cmaster 服务，用来协调 slave 组网。对于每个 slave 节点，都内置一个 harbor 服务用于和其它 slave 节点通讯
]]
standalone = nil


--[[
    master 指定 skynet 控制中心的地址和端口，如果你配置了 standalone 项，那么这一项通常和 standalone 相同(如果监听的是集群内部网卡地址, 那就一样, 如果监听了 0.0.0.0, 那这里就配置成公网ip)
    这是个将要与之连接的地址端口
]]
master = nil
 
--[[
    address 当前 skynet 节点的地址和端口，方便其它节点和它组网。注：即使你只使用一个节点，也需要开启控制中心，并额外配置这个节点的地址和端口
    这是一个 ip:port 格式的字符串, 是监听地址端口, 等待其他节点的连接(集群中每个节点都会与其他连接进行连接, 节点不支持动态上下线)
]]
address = nil

--[[
    harbor 可以是 1-255 间的任意整数。一个 skynet 网络最多支持 255 个节点。每个节点有必须有一个唯一的编号。
    如果 harbor 为 0 ，skynet 工作在单节点模式下。此时 master 和 address 以及 standalone 都不必设置
]]
harbor = 0

--[[
    start 这是 bootstrap 最后一个环节将启动的 lua 服务, 也就是你定制的 skynet 节点的主程序。
    默认为 main ，即启动 main.lua 这个脚本。这个 lua 服务的路径由下面的 luaservice 指定
]]
start = "main_pb"

--[[
    默认为空。如果需要通过 ltls 模块支持 https ，那么需要设置为 true    
]]
enablessl = nil


--[[
    可以以后台模式启动 skynet 。注意，同时请配置 logger 项输出 log
    daemon = "./skynet.pid"
]]
daemon = nil

--[[
    config 本身作为一个 .lua 代码文件, 其定义的变量是作为环境变量 env 存在
    其他定义则出现在内部的 env 表中, 可以使用 skynet.getenv("myname") 来获取
]]
myname = "zl"