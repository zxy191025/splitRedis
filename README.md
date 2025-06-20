## Split Redis version 6.2.6
* Author  :   Jakee Zhao
* Date    :   2025.05.20

### 1.0 Data Struct
* 2025/05/30 add zmalloc        <zmalloc>
* 2025/05/30 add SDS            <SDS>
* 2025/05/30 add adlist         <adlist>
* 2025/05/30 add dict           <dict>
* 2025/05/30 add skiplist       <skiplist>
* 2025/05/30 add intset         <intset>
* 2025/06/03 add ziplist        <ziplist>
* 2025/06/03 add redisObject    <redisObject>

### 2.0 Implementation of a Single-User Database

#### 2.1 redisIO        
 *  A rio object provides the following methods:
 *  read: read from stream.
 *  write: write to stream.
 *  tell: get the current offset.

#### 2.2 rdb aof stream
 *  RDB persistence method can snapshot and store instance data within a specified time interval (when there are M changes within N seconds), which means full backup.

#### 2.3 rax
 *  Redis 在rax.c和rax.h中实现了 Radix 树，主要用于：Stream 数据类型：存储消息 ID（如1625430872345-0），利用前缀快速查询时间范围内的消息。有序键的高效遍历：支持按字典序迭代键，比哈希表更高效。<br>
    假设有以下键需要存储：<br>
    hello<br>
    help<br>
    her<br>
    world<br>
    在普通前缀树中，这些键会被拆分为单个字符节点。而在 Radix 树中，它们可能被组织为：<br>
    [root]<br>
    |-- he<br>
    |    |-- llo (value)<br>
    |    |-- lp (value)<br>
    |    |-- r (value)<br>
    |-- world (value)<br>
    Redis Stream 的核心是通过 **Radix 树（基数树）** 存储消息 ID 到消息内容的映射：<br>
    简化版的Stream数据结构<br>
    typedef struct stream {<br>
        rax *rax;                 // Radix树：存储消息ID到消息内容的映射<br>
        uint64_t length;          // 流中的消息总数<br>
        streamCG *cgroups;        // 消费者组字典<br>
        // 其他字段...<br>
    } stream;<br>
    消息ID结构
    typedef struct streamID {<br>
        uint64_t ms;              // 毫秒时间戳<br>
        uint64_t seq;             // 序列号（同一毫秒内的顺序）<br>
    } streamID;<br>

#### 2.4 SHA-256
 *  SHA-256 是一种安全哈希算法，能够将任意长度的数据转换为固定长度（256 位 / 32 字节）的哈希值。<br>

#### 2.5 background IO
 *  bio.c 文件实现了 Redis 中的后台 I/O 服务，其核心目的是将一些可能会阻塞主线程的 I/O 操作放到后台线程中执行，以此避免这些操作影响 Redis 服务器的性能和响应速度。<br>

#### 2.6 network
 *  ae.c 异步事件驱动模型(Asynchronous Event-Driven Model)是redis高性能的关键，封装了不同操作系统的 I/O 多路复用机制（如 Linux 的 epoll、macOS 的 kqueue、Windows 的 select）<br>
    事件循环(aeEventLoop)<br>
    文件事件(aeFileEvent)<br>
    时间事件(aeTimeEvent)<br>

#### 2.7 redisClient
 *   redis-cli 是用于与Redis服务进行交互。

#### 2.8 redisServer
 *  启动之后执行步骤<br>
    1. 解析命令行参数和配置文件<br>
    initServerConfig();<br>
    
    2. 初始化服务器数据结构<br>
    initServer();<br>
    
    3. 加载持久化文件（RDB 或 AOF）<br>
    loadDataFromDisk();<br>
    
    4. 绑定端口并监听客户端连接<br>
    serverPortOpen();<br>
    
    5. 启动事件循环，开始处理请求<br>
    aeMain(server.el);<br>
    
    6. 服务器关闭，释放资源<br>
    serverShutdown();<br>

