# QuickFIX 第四阶段导读：进入网络和持久化

这份文档只做第四阶段的事情。

如果说：

- 第一阶段是在看“示例程序怎么把 QuickFIX 用起来”
- 第二阶段是在看“FIX 消息对象和协议定义是怎么长出来的”
- 第三阶段是在看“Session 会话引擎怎么管理登录、心跳、序号和校验”

那么第四阶段就是继续往下钻，真正去看：

- 消息字符串是怎么经过 socket 收发的
- QuickFIX 为什么能把消息和序号落到磁盘
- SSL/TLS 到底是在哪一层接进去的

这一阶段聚焦你指定的这些文件：

- `src/C++/SocketInitiator.cpp`
- `src/C++/SocketAcceptor.cpp`
- `src/C++/SocketConnection.cpp`
- `src/C++/FileStore.cpp`
- `src/C++/FileLog.cpp`
- `src/C++/UtilitySSL.cpp`

为了把它们真正讲通，我会补几份非常必要的配套文件：

- `src/C++/Responder.h`
- `src/C++/Parser.cpp`
- `src/C++/SSLSocketInitiator.cpp`
- `src/C++/SSLSocketAcceptor.cpp`
- `src/C++/SSLSocketConnection.cpp`
- `src/C++/SessionState.h`
- `src/C++/Initiator.cpp`
- `src/C++/Acceptor.cpp`

原因很简单：

- `SocketConnection` 不是凭空和 `Session` 连上的，中间靠的是 `Responder`
- “网络字节流怎么切成一条完整 FIX 字符串”这件事，真正做事的是 `Parser`
- `UtilitySSL.cpp` 提供的是 SSL 工具函数，但 SSL 真正接入运行链路，还得看 `SSLSocket*`
- `FileStore` 和 `FileLog` 不是自己主动落盘的，它们是被 `Session` / `SessionState` 在固定时机调用的

这份文档专门回答你这阶段最关键的三个问题：

1. 字符串到底怎么进出网络
2. 消息为什么能落盘
3. SSL 在哪里接进去

---

## 1. 先给这一阶段一个总图

这一阶段最重要的，不是先抠某个 `send()` 或 `recv()`，而是先建立一个新的脑图。

第三阶段你已经知道：

- `Session` 是会话引擎
- 它负责登录、心跳、序号、校验、重传、登出

第四阶段你要补上的，是 `Session` 上下两头的东西：

- 往下：怎么接到网络
- 往旁边：怎么接到文件

可以先把整体关系想成这样：

```text
应用层 Application
-> Session
-> Responder 接口
-> SocketConnection / SSLSocketConnection
-> SocketConnector / SocketServer
-> 操作系统 socket
-> 网络
```

同时，`Session` 旁边还挂着两条“落盘支线”：

```text
Session
-> SessionState
-> MessageStore(FileStore)

Session
-> SessionState
-> Log(FileLog)
```

所以这一阶段其实是在看三条并行链路：

1. 出站链路：应用消息怎么发出去
2. 入站链路：网络字节怎么变成一条 FIX 消息
3. 持久化链路：消息、序号、日志怎么写入文件

---

## 2. 先分清楚每个文件各管什么

如果一上来逐行看源码，很容易把这些类混成一团。

更好的办法是先按职责拆开。

### 2.1 `SocketInitiator.cpp`

这是主动连接的一侧。

它负责：

- 按配置去连远端地址和端口
- 管重连间隔
- 维护“待连接 / 已连接”的 socket 集合
- 在 event loop 里把可读、可写、超时事件转发给具体连接对象

你可以把它理解成：

- “initiator 这一边的网络总控”

### 2.2 `SocketAcceptor.cpp`

这是监听端口的一侧。

它负责：

- 读取每个 session 对应的监听端口
- 建立监听 socket
- 接受新连接
- 把新连接交给对应的 `SocketConnection`
- 在 event loop 里处理可读、可写、断开、超时

你可以把它理解成：

- “acceptor 这一边的网络总控”

### 2.3 `SocketConnection.cpp`

这是最关键的网络类。

它不是一个“傻乎乎的 socket 封装”。

它同时负责：

- 保存单个连接的 socket
- 维护发送队列
- 从 socket 读取字节
- 交给 `Parser` 拼接完整 FIX 消息
- 把完整的原始字符串交给 `Session`
- 在 acceptor 场景下，第一次收到 Logon 时决定这个连接到底属于哪个 `Session`

所以它更像：

- “单个连接级别的传输适配器”

### 2.4 `FileStore.cpp`

它是消息存储器。

它负责：

- 保存出站消息原文
- 保存 sender / target 的下一个序号
- 保存 session 创建时间
- 在需要重传时按序号把旧消息取回来

注意：

- 它不是普通日志
- 它是 QuickFIX 会话恢复和重传机制的一部分

### 2.5 `FileLog.cpp`

它是日志器。

它负责：

- 记录入站消息
- 记录出站消息
- 记录事件日志

注意它和 `FileStore` 的区别：

- `FileStore` 是“功能性持久化”
- `FileLog` 是“观察性日志”

也就是说：

- 没有 `FileStore`，会影响序号恢复和重传
- 没有 `FileLog`，引擎还能跑，只是你看不到详细日志

### 2.6 `UtilitySSL.cpp`

它本身不是“SSL 版 socket 连接类”。

它更像一个 SSL 工具箱，负责：

- 初始化 OpenSSL
- 创建 SSL context
- 加载证书、私钥、CA、CRL
- 设置 cipher / protocol / verify level
- 做一些握手和校验相关的辅助逻辑

真正把它接到网络链路里的，是：

- `SSLSocketInitiator.cpp`
- `SSLSocketAcceptor.cpp`
- `SSLSocketConnection.cpp`

---

## 3. 这一阶段最核心的边界：`Session` 不直接操作 socket

这是第四阶段最重要的一句话。

你必须先牢牢记住：

> `Session` 负责 FIX 协议和会话状态，不直接负责底层 socket 收发。

那它怎么把字符串发出去？

靠的是一个很小但非常关键的接口：

- `Responder`

`Responder.h` 里接口非常简单：

- `send(const std::string&)`
- `disconnect()`

意思是：

- `Session` 只要求“有人能把字符串发出去、能把连接断开”
- 它并不关心那个人到底是普通 socket、SSL socket，还是别的传输层

这就是一个非常典型的分层设计。

### 3.1 `SocketConnection` 为什么能接上 `Session`

因为 `SocketConnection` 实现了 `Responder`。

所以当 `Session` 拿到一个 `Responder*` 之后，它只知道：

- 这里有个传输对象
- 我可以把 FIX 字符串交给它发出去

至于这个传输对象内部是：

- `socket_send`
- 还是 `SSL_write`

`Session` 完全不需要知道。

### 3.2 这个 `Responder` 是什么时候挂上去的

这点也很关键。

在：

- `Initiator::getSession(sessionID, responder)`
- `Acceptor::getSession(msg, responder)`

里，都会调用：

- `session->setResponder(&responder)`

也就是：

- 当某个连接和某个 `Session` 成功绑定之后
- 这个连接对象就变成了该 `Session` 的出站通道

所以这条边可以用一句话记：

> `Session` 的“发消息能力”，实际上是运行时动态插上的。

---

## 4. 出站链路：一条消息是怎么发到网络上的

现在开始回答第一个核心问题的一半：

- “字符串怎么出去”

先给一条总链路：

```text
应用构造 Message
-> Session::send(message)
-> Session::sendRaw(message)
-> Session::send(messageString)
-> Responder::send(string)
-> SocketConnection::send(string)
-> SocketConnection::processQueue()
-> socket_send / SSL_write
-> 网卡 / 网络
```

下面分步骤讲。

### 4.1 第一步：应用层构造 `Message`

你在示例程序里写的通常是：

- `FIX42::NewOrderSingle`
- `FIX42::OrderCancelRequest`
- `FIX42::MarketDataRequest`

这些都还是内存里的消息对象。

它们还不是网络里的字节流。

### 4.2 第二步：`Session::send()`

第三阶段你已经看过：

- `Session::send(Message&)`

它会先清理一些重传相关字段，然后进入：

- `sendRaw(message)`

真正重要的是 `sendRaw()`。

### 4.3 第三步：`Session::sendRaw()` 做协议层组装

`sendRaw()` 是出站总装配线。

它会做这些事：

1. 取消息头 `Header`
2. 读取 `MsgType`
3. 调 `fill(header)` 自动补齐：
   - `BeginString`
   - `SenderCompID`
   - `TargetCompID`
   - `MsgSeqNum`
   - `SendingTime`
4. 调应用回调：
   - admin 消息走 `toAdmin`
   - app 消息走 `toApp`
5. 调 `message.toString(messageString)`，把消息对象序列化成原始 FIX 字符串
6. 调 `persist(...)`，必要时先落盘并推进 sender seq num
7. 最后才调用 `send(messageString)`

这里要特别注意：

> 真正发到网络上的，不是 `Message` 对象，而是 `message.toString()` 生成的那条原始 FIX 字符串。

### 4.4 第四步：`Session::send(const std::string&)`

这个函数很短，但很关键。

它做两件事：

1. 记录 outgoing 日志
2. 调 `m_pResponder->send(string)`

也就是说：

- 到了这一步，`Session` 的工作基本结束了
- 接下来就进入传输层

### 4.5 第五步：`SocketConnection::send()`

`SocketConnection::send(const std::string&)` 并不是直接一把 `send()` 到系统 socket。

它先做的是：

1. 把字符串放进 `m_sendQueue`
2. 立即尝试 `processQueue()`
3. 调 `signal()` 通知 monitor：这个 socket 现在有待发送数据

这里为什么要用队列？

因为网络发送不保证一次写完。

可能出现：

- socket 当前不可写
- 只写出去一部分
- 还要等下一轮可写事件再继续

所以 `SocketConnection` 内部要维护：

- `m_sendQueue`
- `m_sendLength`

前者表示还有哪些消息待发，后者表示当前队首消息已经发到了第几个字节。

### 4.6 第六步：`SocketConnection::processQueue()`

这是真正往 socket 写数据的地方。

它的思路是：

1. 如果队列为空，直接返回
2. 用 `select` 或 `poll` 检查当前 socket 是否可写
3. 拿队首消息
4. 调 `socket_send(...)`
5. 如果只发了一部分，就更新 `m_sendLength`
6. 如果整条消息都发完了，就把这条消息从队列弹出

这说明：

- QuickFIX 的 socket 发送是“带缓冲和续写”的
- 不是每次 `Session::sendRaw()` 都意味着立刻一次写完

### 4.7 为什么这里还有 `signal()` / `unsignal()`

这两个小函数是为了和 `SocketMonitor` 配合。

你可以先粗糙地理解成：

- `signal()`：告诉事件循环，这个连接还有东西要写
- `unsignal()`：告诉事件循环，这个连接暂时写完了

这就是单线程 event loop 的一个典型做法。

---

## 5. 入站链路：网络字节是怎么变成一条 FIX 字符串的

现在回答第一个核心问题的另一半：

- “字符串怎么进来”

先给总链路：

```text
socket 收到字节
-> SocketConnection::readFromSocket()
-> Parser::addToStream(...)
-> Parser::readFixMessage(...)
-> 拿到一条完整 FIX 原始字符串
-> Session::next(rawString, now)
-> Message 解析
-> DataDictionary 校验
-> Session::verify()
-> fromAdmin / fromApp
```

这里最关键的是：

> 网络层先把“字节流”切成“完整 FIX 消息字符串”，然后才交给 `Session`。

### 5.1 `SocketInitiator` / `SocketAcceptor` 只是事件分发者

不管是 initiator 还是 acceptor，它们在收到“这个 socket 可读了”的事件后，做的都很薄：

- 找到对应的 `SocketConnection`
- 调它的 `read(...)`

所以真正处理入站消息的主角还是：

- `SocketConnection`

### 5.2 `SocketConnection::readFromSocket()`

这是最底层的收字节动作。

它会：

1. 调 `socket_recv(...)`
2. 把读到的字节放进 `m_buffer`
3. 再调用 `m_parser.addToStream(m_buffer, size)`

这一步非常重要，因为它说明：

- 从网卡读进来的，不一定正好就是一整条 FIX 消息
- 也可能是半条、两条、甚至几条半截拼一起

所以不能“收一次就 parse 一次”。

必须先放进一个持续累积的流缓冲区里。

### 5.3 真正做“拆包”的是 `Parser.cpp`

`Parser` 做的事情非常值得你记住，因为它就是“FIX 流解析器”。

它的核心逻辑是：

1. 在缓冲区里找到 `8=`，也就是 `BeginString`
2. 再找 `\0019=`，也就是 `BodyLength`
3. 读出 `BodyLength`
4. 根据长度往后推进
5. 再找 `\00110=`，也就是 `CheckSum`
6. 如果 checksum 字段也完整找到了，就认为一条完整 FIX 消息结束
7. 把这一段字符串切出来返回
8. 把剩余未消费字节继续留在缓冲区里

这里的 `\001` 就是 FIX 的分隔符 SOH。

所以你平时在日志里看到的：

- `8=FIX.4.2|9=...|35=D|...|10=...|`

那只是为了人眼可读常常把 SOH 显示成 `|`。

真正线上传输的分隔符是：

- `\001`

### 5.4 `SocketConnection::readMessage()`

这个函数很薄，它只是调用：

- `m_parser.readFixMessage(message)`

也就是说：

- `SocketConnection` 自己不懂 FIX 报文格式
- 它只负责把字节交给 `Parser`

### 5.5 `SocketConnection::readMessages()`

当 parser 已经能从缓冲区里读出完整消息后，`readMessages()` 就开始循环：

1. 连续 `readMessage(message)`
2. 每拿到一条完整字符串
3. 就调用 `m_pSession->next(message, UtcTimeStamp::now())`

这一点很关键：

> `Session` 接手的时候，输入还是原始 FIX 字符串，不是 socket 字节，也不是高层业务对象。

然后这条字符串才会进入第三阶段你已经看过的会话引擎。

---

## 6. acceptor 为什么第一次必须先靠 Logon 识别 session

这点特别值得单独讲，因为它解释了：

- 为什么对 acceptor 来说，第一条消息不是普通业务消息
- 为什么连接和 session 不是一开始就天然绑定的

### 6.1 initiator 比较简单

initiator 主动连出去时，本来就知道：

- 我要为哪个 `SessionID` 去建连接

所以在 `SocketInitiator` 的路径里：

- `SocketConnection` 一创建
- 就已经能拿到具体 `Session`

### 6.2 acceptor 不一样

acceptor 是被动接入。

它只知道：

- 有人连到了某个监听端口

但它一开始还不知道：

- 这个 TCP 连接到底对应哪个 `SenderCompID/TargetCompID`
- 也就不知道它最终该绑定到哪个 `Session`

### 6.3 所以 `SocketConnection` 在 acceptor 侧有一个“未绑定状态”

acceptor 场景下，`SocketConnection` 刚建出来时：

- `m_pSession` 还是空的

它会先不断从 socket 里读，直到 parser 拼出第一条完整 FIX 消息。

然后它做：

- `Session::lookupSession(message, true)`

再结合：

- `Acceptor::getSession(message, *this)`

去真正绑定 `Session`。

### 6.4 为什么第一条必须是 Logon

看 `Acceptor::getSession(const std::string&, Responder&)` 的逻辑就很清楚了。

它会：

1. 先只解析 header
2. 读出：
   - `BeginString`
   - `SenderCompID`
   - `TargetCompID`
   - `MsgType`
3. 如果 `MsgType != Logon`，直接返回空
4. 只有在 `Logon` 的情况下，才把 sender/target 反过来构造本地 `SessionID`
5. 找到对应 `Session`
6. 调 `setResponder(&responder)`

这意味着：

> 对 acceptor 而言，第一条消息不是用来做业务的，而是用来“认领这个连接属于谁”的。

### 6.5 绑定完以后才进入正常会话流程

一旦 `m_pSession` 绑定成功，后面的消息就走普通路径了：

- `readFromSocket()`
- `readMessages()`
- `m_pSession->next(...)`

所以 acceptor 的第一条消息和后续消息，处理路径其实略有不同。

---

## 7. `SocketInitiator.cpp` 和 `SocketAcceptor.cpp` 到底在干什么

现在可以回头看两个“网络总控类”。

### 7.1 `SocketInitiator` 的主职责

它的核心工作可以概括成四件事：

1. 读配置
   - `ReconnectInterval`
   - `SocketNodelay`
   - `SocketSendBufferSize`
   - `SocketReceiveBufferSize`

2. 负责主动连接
   - `doConnect(sessionID, dictionary)`

3. 维护连接生命周期
   - pending
   - connected
   - disconnected

4. 驱动事件循环
   - `onConnect`
   - `onData`
   - `onWrite`
   - `onDisconnect`
   - `onTimeout`

### 7.2 `SocketInitiator::doConnect()`

这是 initiator 真正去发起 TCP 连接的地方。

它会：

1. 根据 `SessionID` 找 `Session`
2. 检查当前是否在 session time 内
3. 用 `HostDetailsProvider` 取远端地址、端口、本地 source 地址等
4. 调 `m_connector.connect(...)`
5. 把返回的 socket 包装成一个新的 `SocketConnection`
6. 放进 `m_pendingConnections`

所以 initiator 的连接过程可以理解成：

- “先把 TCP 连接建起来，再挂上一个连接对象等后续事件驱动”

### 7.3 `SocketAcceptor` 的主职责

它和 initiator 的区别主要是：

- initiator 管“连出去”
- acceptor 管“监听并接进来”

`SocketAcceptor::onInitialize()` 会：

1. 遍历所有 acceptor session
2. 读每个 session 的 `SocketAcceptPort`
3. 建监听 socket
4. 建立：
   - 端口 -> session 集合 的映射
   - session -> 端口 的映射

这一步特别重要，因为 acceptor 后面要靠“监听端口 + Logon Header”一起确定归属。

### 7.4 两者共同点

虽然一个是主动连，一个是被动收，但它们结构很像：

- 都有一个 event loop
- 都在 socket 可读时调连接对象的 `read()`
- 都在 socket 可写时调连接对象的 `processQueue()`
- 都在超时时调连接对象的 `onTimeout()`

所以这两类文件主要是在做：

- “多连接调度”

而不是做：

- “具体单条消息处理”

具体消息处理还是在：

- `SocketConnection`
- `Session`

---

## 8. 为什么消息能落盘：关键不是 `FileStore`，而是 `Session` 会主动调用它

现在进入第二个核心问题：

- “消息为什么能落盘”

先说结论：

> QuickFIX 能落盘，不是因为 `SocketConnection` 一边收发一边顺手写文件，而是因为 `Session` 从创建开始就已经挂上了 `MessageStore` 和 `Log`，然后在固定的会话时机主动调用它们。

这点一定要分清。

### 8.1 `Session` 创建时，store 和 log 就已经挂上了

在 `Session` 构造函数里，`SessionState` 会拿到：

- `m_messageStoreFactory.create(...)`
- `m_pLogFactory->create(m_sessionID)`

也就是说：

- 某个 session 一创建出来
- 它就已经有自己的 store 和 log 后端了

不是第一次发消息时才临时建文件。

### 8.2 `SessionState` 是 store 和 log 的代理层

你在 `SessionState.h` 里会看到：

- `MessageStore *m_pStore`
- `Log *m_pLog`

同时它自己还提供：

- `set/get/incrNextSenderMsgSeqNum/...`
- `onIncoming/onOutgoing/onEvent`

所以可以把 `SessionState` 理解成：

- “`Session` 操作持久化和日志的统一门面”

`Session` 并不直接操纵 `FILE*` 或 `ofstream`。

它只是在合适的时候调：

- `m_state.set(...)`
- `m_state.incrNextSenderMsgSeqNum()`
- `m_state.onIncoming(...)`
- `m_state.onOutgoing(...)`
- `m_state.onEvent(...)`

---

## 9. `FileStore` 到底存了什么

`FileStore` 最值得记住的一点是：

> 它存的不只是“消息文本”，而是一整套和会话恢复有关的数据。

### 9.1 它会建立四类文件

每个 session 会生成四个文件：

- `.body`
- `.header`
- `.seqnums`
- `.session`

文件名前缀长这样：

```text
[BeginString]-[SenderCompID]-[TargetCompID][-Qualifier].
```

### 9.2 `.body` 文件

这是消息正文流。

里面存的是：

- 原始 FIX 消息字符串

注意：

- 它主要用于保存出站消息原文，供以后重传时取回

### 9.3 `.header` 文件

这个名字容易误导，它不是 FIX Header 文本。

它其实是索引文件。

里面记录的是：

- 某个 `MsgSeqNum`
- 对应消息在 `.body` 里的 offset
- 对应消息长度 size

所以它的作用更像：

- “序号 -> 文件偏移” 的查找表

### 9.4 `.seqnums` 文件

这个文件存的是：

- next sender seq num
- next target seq num

也就是说：

- 即使程序退出重启
- QuickFIX 也能知道这次会话下一条该发多少、期望收到多少

### 9.5 `.session` 文件

这个文件存的是：

- session creation time

它用于会话时间和重置相关逻辑。

---

## 10. 出站消息为什么会写进 `FileStore`

这条线你一定要和第三阶段的 `sendRaw()` 连起来看。

### 10.1 关键动作在 `Session::persist()`

在 `Session::sendRaw()` 里，当消息已经被序列化成字符串后，会调用：

- `persist(message, messageString)`

而 `persist()` 做两件事：

1. 如果 `PersistMessages=Y`
   - `m_state.set(msgSeqNum, messageString)`
2. `m_state.incrNextSenderMsgSeqNum()`

也就是说：

- 真正写入 `FileStore` 的，是已经序列化好的原始 FIX 字符串
- sender seq num 的推进，也是和这一步绑定的

### 10.2 `FileStore::set()` 做了什么

`FileStore::set(msgSeqNum, msg)` 的逻辑很清楚：

1. 把消息追加写入 `.body`
2. 记下当前 offset 和消息长度
3. 把 `msgSeqNum -> offset,size` 写入 `.header`
4. 刷盘

这就意味着：

- `.body` 负责存原文
- `.header` 负责帮你以后按序号定位

两者要配合起来才能做重传。

### 10.3 入站消息会不会存到 `FileStore`

这是个非常容易混淆的点。

通常：

- `FileStore` 重点存的是出站消息，用于 resend

但入站也不是完全不留痕迹，因为：

- target seq num 的推进会写进 `.seqnums`
- 如果有 `FileLog`，原始入站消息也会进入日志文件

所以要区分：

- “消息重放存储”
- “通信日志记录”

这两件事不是一回事。

---

## 11. 为什么能重传：因为 `FileStore` 支持按序号取消息

`FileStore` 真正的价值，是在对方发来 `ResendRequest` 时体现出来的。

第三阶段你看到过：

- `Session::nextResendRequest(...)`
- `Session::generateRetransmits(...)`

这里面最终会调用：

- `m_state.get(beginSeqNo, endSeqNo, messages)`

而 `SessionState` 再转给 `FileStore::get(...)`。

### 11.1 `FileStore::get(begin, end, vector<string>&)`

它会：

1. 根据 `MsgSeqNum` 查 `.header` 里的 offset/size
2. 去 `.body` 对应位置读取原文
3. 还原出那条历史 FIX 字符串

所以 QuickFIX 才能在会话恢复时做这种事情：

- 把你以前真的发过的报文重新拿出来
- 再按 FIX 重传规则重新发给对方

### 11.2 如果 `PersistMessages=N` 会怎样

那就意味着：

- QuickFIX 不再保留这些旧出站消息原文

因此当对方要求重传时，往往不能把原消息逐条重发。

这时引擎会更多依赖：

- `SequenceReset`
- `GapFill`

也就是告诉对方：

- “这些序号我跳过去了”

所以：

> `FileStore` 的意义，不只是“存档”，而是决定你能不能做真正的消息级重传。

---

## 12. `FileStore` 的启动、刷新、重置分别是什么意思

这点也很值得单独记。

### 12.1 `open(false)`

正常打开已有文件。

它会：

1. 读旧的 header 索引
2. 读旧的 seqnums
3. 读旧的 session creation time
4. 把这些数据放进内存缓存

### 12.2 `refresh()`

它的意思更像：

- “重新从文件加载当前 store 状态”

不是删文件。

### 12.3 `reset(now)`

它的意思更像：

- “把这个 session 的持久化状态清零重建”

它会：

- 清掉旧消息索引
- 重建序号状态
- 重置 session 创建时间

所以你可以把它理解成：

- `refresh` 是重读
- `reset` 是重开新账本

---

## 13. `FileLog` 到底存了什么

现在看另一条落盘支线。

`FileLog` 和 `FileStore` 不一样，它不是为了 resend。

它是为了让你观察系统。

### 13.1 它会建两类文件

每个 session 通常会有：

- `messages.current.log`
- `event.current.log`

前缀仍然是 session 维度的：

- `BeginString-SenderCompID-TargetCompID...`

### 13.2 `messages.current.log`

这里记录：

- incoming FIX 原始字符串
- outgoing FIX 原始字符串

它们对应的触发时机，基本就是：

- `Session::next(rawString, now)` 里的 `onIncoming(...)`
- `Session::send(string)` 里的 `onOutgoing(...)`

### 13.3 `event.current.log`

这里记录：

- 连接建立
- 断开
- 登录/登出
- Reject
- timeout
- SSL 错误
- 其他引擎事件

所以你平时看终端日志时常见的那些：

- `Received logon request`
- `Responding to logon request`
- `Disconnecting`
- `Timed out waiting for heartbeat`

本质上就是这些 event log 的内容。

### 13.4 `FileLog` 为什么也能自动写

原因和 `FileStore` 一样，不是它自己在“监听系统事件”。

而是因为 `Session` / `SessionState` 会在固定时机显式调用：

- `onIncoming(...)`
- `onOutgoing(...)`
- `onEvent(...)`

而 `FileLog` 在这些函数里只是简单把内容写到文件里。

所以你可以把 `FileLog` 理解成：

- “一个被引擎主动喂日志内容的 sink”

---

## 14. `FileStore` 和 `FileLog` 的关系，一定要分开记

这一对很容易背混。

### 14.1 `FileStore`

它更像交易系统的“账本”。

它关心：

- 消息能不能按序号找回来
- sender/target 序号能不能恢复
- 程序重启后 session 状态能不能延续

### 14.2 `FileLog`

它更像“流水日志”。

它关心：

- 你有没有看见这条消息
- 当时发生了什么事件
- 出问题时能不能排查

### 14.3 一句话区别

你可以记成：

- `FileStore` 是给引擎自己用的
- `FileLog` 是给人排查问题用的

虽然它们都落盘，但角色完全不同。

---

## 15. SSL 到底接在哪一层

现在回答第三个核心问题。

先说最重要的结论：

> SSL 不是接在 `Session` 层，也不是接在消息对象层，而是接在“传输层连接对象”这一层。

也就是说，它插在：

```text
Session
-> Responder
-> SocketConnection
```

这一段里。

普通版用：

- `SocketConnection`

SSL 版则换成：

- `SSLSocketConnection`

这就是 QuickFIX 的做法。

---

## 16. `UtilitySSL.cpp` 本身到底做什么

它主要是 SSL 工具库，不直接跑业务循环。

你可以把它按三类功能去理解。

### 16.1 第一类：全局初始化和清理

比如：

- `ssl_init()`
- `ssl_term()`

它们负责：

- 初始化 OpenSSL 全局状态
- 加载算法和错误字符串
- 准备线程锁
- 做一些 DH/ECDH 相关初始化

这就是为什么 SSL 功能不是“遇到第一条消息时临时现开”的。

### 16.2 第二类：创建和配置 SSL context

比如：

- `createSSLContext(...)`
- `loadSSLCert(...)`
- `loadCAInfo(...)`
- `loadCRLInfo(...)`

它们负责：

- 选 TLS 协议版本
- 选 cipher suite
- 装载证书和私钥
- 装载 CA 证书
- 装载 CRL
- 配置 verify level

### 16.3 第三类：握手和证书校验辅助

比如：

- `acceptSSLConnection(...)`
- `callbackVerify(...)`
- `ssl_set_sni_hostname(...)`
- `ssl_socket_close(...)`

它们负责：

- 做服务端握手
- 打证书校验日志
- 设置 SNI
- 优雅关闭 SSL socket

所以 `UtilitySSL.cpp` 更像：

- “SSL 基础设施层”

---

## 17. 真正把 SSL 插进来的是 `SSLSocket*`

这一步特别重要。

如果只看 `UtilitySSL.cpp`，你会知道 SSL 能做什么，但还看不见它怎样进入运行链路。

真正接线的是下面三类：

- `SSLSocketInitiator`
- `SSLSocketAcceptor`
- `SSLSocketConnection`

它们本质上就是：

- 普通 socket 版本的 SSL 替身

### 17.1 `SSLSocketInitiator`

它和 `SocketInitiator` 很像，但多了这些步骤：

1. `onInitialize()` 时先 `ssl_init()`
2. 创建 `SSL_CTX`
3. 加载客户端证书 / 私钥 / CA
4. `doConnect()` 建好 TCP socket 后：
   - `SSL_new(m_ctx)`
   - `BIO_new_socket(...)`
   - `SSL_set_bio(...)`
   - `ssl_set_sni_hostname(...)`
5. 把连接对象建成 `SSLSocketConnection`
6. 在 `onConnect()` / `onData()` / `onWrite()` 里继续推动 `SSL_connect()`
7. 只有握手成功后，才把连接转入正式 `m_connections`

这条线非常关键，因为它说明：

> TCP 连上不等于 FIX 会话已经开始，SSL 握手得先过。

### 17.2 `SSLSocketAcceptor`

它和 `SocketAcceptor` 也很像，但在接受新连接后会多做：

1. `SSL_new(m_ctx)`
2. `BIO_new_socket(s, BIO_NOCLOSE)`
3. `SSL_set_bio(...)`
4. `acceptSSLConnection(...)`

如果握手失败：

- 连接直接被丢掉

如果握手成功：

- 才创建成一个真正可用的 SSL 连接对象

### 17.3 `SSLSocketConnection`

它和 `SocketConnection` 的职责几乎一样：

- 发送队列
- parser
- 连接到 session
- 读写数据

但是底层 I/O 从：

- `socket_send / socket_recv`

换成了：

- `SSL_write / SSL_read`

所以你可以把它理解成：

- “把原本的明文传输，换成 TLS 包裹后的传输”

---

## 18. SSL 为什么没有改 `Session`

这正是 QuickFIX 这一层设计漂亮的地方。

因为：

- `Session` 面向的是 `Responder`
- 不面向“普通 socket”或“SSL socket”

所以切换成 SSL 时，不需要改：

- `Session::sendRaw()`
- `Session::verify()`
- `Application::fromApp()`
- `MessageCracker`

这些协议和业务层逻辑。

改变的只是：

- 下面那个负责收发字符串的连接对象

这就是很标准的“传输层替换，不动会话层”。

---

## 19. `SSLSocketConnection` 比普通版多出来的关键复杂度是什么

这一点你读源码时会明显感觉到。

普通 `SocketConnection` 的读写虽然也有非阻塞和部分发送，但整体还比较直接。

SSL 版会多出一类非常典型的问题：

- `SSL_ERROR_WANT_READ`
- `SSL_ERROR_WANT_WRITE`

意思是：

- 这次你本来在读，但 SSL 层要求先等可写
- 或者你本来在写，但 SSL 层要求先等可读

所以 `SSLSocketConnection` 里会多出这些状态：

- `m_processQueueNeedsToReadData`
- `m_readFromSocketNeedsToWriteData`

也就是说：

- SSL 读写比普通 socket 多了一层握手/记录层状态机
- 事件循环里要更细地协调“下一步该读还是该写”

这也是为什么 SSL 代码看起来会比普通 socket 版更绕一点。

---

## 20. 你之前遇到的那些 SSL 日志，放在这里就能解释了

比如你之前看过：

- `SNI not set: address is an IP address`
- `Certificate Verification: Error (10): certificate has expired`
- `SSL_connect failed with SSL error 1`

现在都能定位到这条链上。

### 20.1 `SNI not set: address is an IP address`

这是 `ssl_set_sni_hostname(...)` 那条逻辑。

它的意思不是错误，而是：

- 当前连接目标是 IP 地址，比如 `127.0.0.1`
- 不是域名
- 所以不设置 SNI

这是正常现象。

### 20.2 证书过期

这是 SSL 握手阶段的证书校验问题。

它发生在：

- FIX Logon 之前
- `Session::nextLogon()` 之前
- 甚至在 `Session` 还没真正开始处理业务之前

也就是说：

- 如果这里失败，后面的 FIX 会话根本起不来

### 20.3 `SSL_connect failed`

这说明失败点还在：

- `SSLSocketInitiator` 的握手阶段

而不是：

- 业务消息阶段

这在排错时很重要，因为它告诉你要去查：

- 证书
- CA
- verify level
- protocol/cipher

而不是先去怀疑：

- `fromApp`
- `crack()`
- `MessageCracker`

---

## 21. 现在把“网络 + Session + 落盘”串成一条完整链

这一段你最好能自己顺口讲出来。

### 21.1 出站完整链

```text
应用层构造 Message
-> Session::send()
-> Session::sendRaw()
-> fill Header / toAdmin or toApp / toString
-> persist()
-> Session::send(rawString)
-> Responder::send(rawString)
-> SocketConnection::send()
-> processQueue()
-> socket_send 或 SSL_write
-> 发到网络
```

### 21.2 入站完整链

```text
网络字节进入 socket
-> SocketConnection::readFromSocket()
-> Parser 缓存并拆包
-> 得到完整 FIX 原始字符串
-> Session::next(rawString, now)
-> Message 解析
-> DataDictionary::validate()
-> Session::verify()
-> fromAdmin / fromApp
-> 如果是业务消息，再 crack() -> onMessage()
```

### 21.3 持久化完整链

```text
Session 创建
-> 挂上 MessageStore(FileStore) 和 Log(FileLog)

出站消息
-> persist()
-> FileStore 保存消息正文和序号

入站/出站/事件
-> onIncoming / onOutgoing / onEvent
-> FileLog 追加写日志
```

只要这三条链你能讲顺，第四阶段的主干就已经抓住了。

---

## 22. 这阶段最容易混淆的几个点

### 22.1 `SocketInitiator` / `SocketAcceptor` 不负责消息业务

它们主要负责：

- 多连接
- 事件循环
- 连接建立和销毁

真正处理单条消息的是：

- `SocketConnection`
- `Session`

### 22.2 `SocketConnection` 也不做协议校验

它负责：

- 收发字节
- 拼完整 FIX 字符串
- 转发给 `Session`

协议校验还是第三阶段那一套：

- `Message` 解析
- `DataDictionary::validate`
- `Session::verify`

### 22.3 `FileStore` 不是普通日志

它不是为了给人看。

它是为了：

- 记住已发消息
- 记住序号
- 支撑 resend 和恢复

### 22.4 `FileLog` 不是会话恢复机制

它只是记录现象。

你可以把日志删掉，系统照样能用 `FileStore` 恢复序号。

### 22.5 `UtilitySSL.cpp` 不是完整的 SSL 运行时

它只是工具箱。

真正的运行时接线发生在：

- `SSLSocketInitiator`
- `SSLSocketAcceptor`
- `SSLSocketConnection`

---

## 23. 现在回头读源码，建议按什么顺序

这一阶段我建议你不要完全照文件名顺序读，而是按“问题驱动”顺序读。

### 第一步：先看字符串怎么拆包

按这个顺序：

1. `src/C++/Responder.h`
2. `src/C++/Parser.cpp`
3. `src/C++/SocketConnection.cpp`

你的目标只回答一个问题：

> socket 收到一串字节后，QuickFIX 是怎么把它切成一条条完整 FIX 报文的？

### 第二步：再看连接是怎么被驱动的

按这个顺序：

1. `src/C++/SocketInitiator.cpp`
2. `src/C++/SocketAcceptor.cpp`
3. `src/C++/Initiator.cpp`
4. `src/C++/Acceptor.cpp`

你的目标是：

> 一个 `SocketConnection` 是怎么被创建、绑定、驱动、断开的？

### 第三步：再看为什么能落盘

按这个顺序：

1. `src/C++/SessionState.h`
2. `src/C++/FileStore.cpp`
3. `src/C++/FileLog.cpp`
4. 回头对照 `src/C++/Session.cpp` 里的 `persist()`、`send()`、`next(rawString)`

你的目标是：

> 哪些时机会触发写文件，写进去的到底是什么？

### 第四步：最后补 SSL

按这个顺序：

1. `src/C++/UtilitySSL.cpp`
2. `src/C++/SSLSocketInitiator.cpp`
3. `src/C++/SSLSocketAcceptor.cpp`
4. `src/C++/SSLSocketConnection.cpp`

你的目标是：

> SSL 是怎样在不改 `Session` 逻辑的情况下，被插入到底层传输链路里的？

---

## 24. 第四阶段结束后，你应该能回答什么

如果这一阶段已经吃透，你现在应该能比较顺地回答这些问题：

1. `Session` 为什么不直接调 `socket_send`？
2. `Responder` 在 QuickFIX 里起什么作用？
3. `SocketConnection` 为什么既像传输层，又像 parser 驱动器？
4. initiator 和 acceptor 在连接建立时最大的区别是什么？
5. acceptor 为什么必须先收到 Logon 才能绑定 `Session`？
6. 原始字节流是如何被 `Parser` 切成完整 FIX 字符串的？
7. `FileStore` 的四个文件各自存什么？
8. 为什么 `FileStore` 能支撑 `ResendRequest`？
9. `FileStore` 和 `FileLog` 的职责到底有什么本质区别？
10. SSL 为什么只需要替换连接层，不需要改 `Session`？
11. 证书校验失败为什么会发生在 FIX Logon 之前？
12. `SSLSocketConnection` 比普通 `SocketConnection` 多出来的复杂度是什么？

如果这些问题你已经能自己讲出来，第四阶段就算真正过关了。

---

## 25. 把第四阶段压缩成一句话

最后把这一阶段压缩成一句最关键的话：

> QuickFIX 把“会话协议”和“网络传输”拆成了两层：`Session` 只负责 FIX 协议、序号、心跳、校验和回调，不直接碰 socket；真正的网络收发由 `SocketConnection` 这类 `Responder` 实现者完成，它先把字节流交给 `Parser` 切成完整 FIX 字符串，再送回 `Session`；而消息持久化和日志则通过 `SessionState` 统一转发给 `FileStore` 与 `FileLog`，因此 QuickFIX 既能恢复序号和重传旧消息，也能把通信过程落盘；SSL 则通过 `SSLSocket*` 在连接层替换普通 socket 读写，从而在不改 `Session` 逻辑的前提下接入 TLS。

---

## 26. 下一步最自然的衔接

到这里，前四个阶段其实已经把 QuickFIX 的主骨架串起来了：

1. 示例怎么用
2. 消息对象和协议定义
3. 会话引擎
4. 网络与持久化

下一步最适合做的，通常有两条路线：

1. 做一条“完整消息追踪”
   从 `tradeclient` 发一条 `NewOrderSingle`，一路追到对端 `onMessage`

2. 进入“代码生成和多语言绑定”
   去看 XML 规范如何生成 C++ / Python / Ruby 的消息类和字段类

如果你继续按现在这条学习路线往下走，最自然的下一篇，通常就是：

- 选一条真实业务消息
- 从发送端 `sendRaw()`
- 跟到网络层
- 跟到对端 `next(rawString)`
- 跟到 `DataDictionary::validate`
- 跟到 `crack()`
- 跟到具体 `onMessage()`

这会把前四阶段第一次真正串成一条活链路。
